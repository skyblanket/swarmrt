/*
 * SwarmRT Phase 6: IO System
 *
 * Architecture:
 * - Dedicated IO thread runs event loop
 *     macOS:   kqueue
 *     Linux:   epoll
 *     Windows: WSAPoll
 * - Ports are registered for read events
 * - When data arrives, IO thread sends SW_TAG_PORT_DATA to owning process
 * - TCP listen ports send SW_TAG_PORT_ACCEPT for new connections
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <mswsock.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sw_fd_t;
  #define SW_INVALID_FD INVALID_SOCKET
#else
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #ifdef __APPLE__
    #include <sys/event.h>
  #else
    #include <sys/epoll.h>
  #endif
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  typedef int sw_fd_t;
  #define SW_INVALID_FD (-1)
#endif

#include "swarmrt_io.h"

/* === Global IO State === */

#define SW_IO_MAX_EVENTS 64
#define SW_IO_RECV_BUF   4096

#ifndef _WIN32
static int g_kq = -1;
#endif
static pthread_t g_io_thread;
static volatile int g_io_running = 0;

static sw_port_t *g_ports = NULL;           /* Global port list */
static pthread_mutex_t g_ports_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t g_next_port_id = 1;

/* Wake pipe for signaling the IO thread */
#ifdef _WIN32
static SOCKET g_wake_pipe[2] = {INVALID_SOCKET, INVALID_SOCKET};
#else
static int g_wake_pipe[2] = {-1, -1};
#endif

/* === Internal Helpers === */

static void set_nonblocking(sw_fd_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void set_nodelay(sw_fd_t fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}

static void set_reuseaddr(sw_fd_t fd) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
}

static inline void close_fd(sw_fd_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static sw_port_t *port_alloc(sw_fd_t fd, sw_port_type_t type, sw_process_t *owner) {
    sw_port_t *p = (sw_port_t *)calloc(1, sizeof(sw_port_t));
    p->fd = fd;
    p->type = type;
    p->state = SW_PORT_OPEN;
    p->owner = owner;
    p->id = atomic_fetch_add(&g_next_port_id, 1);

    if (type == SW_PORT_TCP_CONN) {
        p->recv_buf = (uint8_t *)malloc(SW_IO_RECV_BUF);
        p->recv_buf_size = SW_IO_RECV_BUF;
    }

    /* Add to global list */
    pthread_mutex_lock(&g_ports_lock);
    p->next = g_ports;
    g_ports = p;
    pthread_mutex_unlock(&g_ports_lock);

    return p;
}

static void port_remove(sw_port_t *port) {
    pthread_mutex_lock(&g_ports_lock);
    sw_port_t **pp = &g_ports;
    while (*pp) {
        if (*pp == port) {
            *pp = port->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_ports_lock);
}

static void port_free(sw_port_t *port) {
    if (port->fd != SW_INVALID_FD) {
        close_fd(port->fd);
        port->fd = SW_INVALID_FD;
    }
    if (port->recv_buf) {
        free(port->recv_buf);
        port->recv_buf = NULL;
    }
    port->state = SW_PORT_CLOSED;
    free(port);
}

/* === Event registration (platform-specific) === */

#if defined(__APPLE__)

static void ev_register_read(sw_fd_t fd, void *udata) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, udata);
    kevent(g_kq, &ev, 1, NULL, 0, NULL);
}

static void ev_deregister(sw_fd_t fd) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(g_kq, &ev, 1, NULL, 0, NULL);
}

#elif defined(_WIN32)

/* Windows WSAPoll doesn't have register/deregister — we rebuild the poll set each iteration */
static void ev_register_read(sw_fd_t fd, void *udata) {
    (void)fd; (void)udata;
    /* No-op: WSAPoll builds fd set from port list each loop */
}

static void ev_deregister(sw_fd_t fd) {
    (void)fd;
    /* No-op */
}

#else /* Linux epoll */

static void ev_register_read(sw_fd_t fd, void *udata) {
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = udata };
    epoll_ctl(g_kq, EPOLL_CTL_ADD, fd, &ev);
}

static void ev_deregister(sw_fd_t fd) {
    epoll_ctl(g_kq, EPOLL_CTL_DEL, fd, NULL);
}

#endif

static void io_wake(void) {
    char c = 1;
#ifdef _WIN32
    send(g_wake_pipe[1], &c, 1, 0);
#else
    (void)write(g_wake_pipe[1], &c, 1);
#endif
}

/* === Handle events === */

static void handle_accept(sw_port_t *listener) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    sw_fd_t fd = accept(listener->fd, (struct sockaddr *)&addr, &len);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) return;
#else
    if (fd < 0) return;
#endif

    set_nonblocking(fd);
    set_nodelay(fd);

    sw_port_t *conn = port_alloc(fd, SW_PORT_TCP_CONN, listener->owner);
    ev_register_read(fd, conn);

    sw_port_accept_t *msg = (sw_port_accept_t *)malloc(sizeof(sw_port_accept_t));
    msg->listener = listener;
    msg->conn = conn;
    sw_send_tagged(listener->owner, SW_TAG_PORT_ACCEPT, msg);
}

static void handle_read(sw_port_t *port) {
    if (port->state != SW_PORT_OPEN || !port->owner) return;

#ifdef _WIN32
    int n = recv(port->fd, (char *)port->recv_buf, port->recv_buf_size, 0);
#else
    ssize_t n = read(port->fd, port->recv_buf, port->recv_buf_size);
#endif

    if (n > 0) {
        uint8_t *copy = (uint8_t *)malloc(n);
        memcpy(copy, port->recv_buf, n);

        sw_port_data_t *msg = (sw_port_data_t *)malloc(sizeof(sw_port_data_t));
        msg->port = port;
        msg->data = copy;
        msg->len = (uint32_t)n;
        sw_send_tagged(port->owner, SW_TAG_PORT_DATA, msg);
    } else if (n == 0 || (n < 0 &&
#ifdef _WIN32
        WSAGetLastError() != WSAEWOULDBLOCK
#else
        errno != EAGAIN && errno != EWOULDBLOCK
#endif
    )) {
        ev_deregister(port->fd);
        port->state = SW_PORT_CLOSING;

        sw_port_event_t *msg = (sw_port_event_t *)malloc(sizeof(sw_port_event_t));
        msg->port = port;
#ifdef _WIN32
        msg->error = (n == 0) ? 0 : WSAGetLastError();
#else
        msg->error = (n == 0) ? 0 : errno;
#endif
        sw_send_tagged(port->owner, SW_TAG_PORT_CLOSED, msg);
    }
}

/* === IO Thread === */

static void *io_loop(void *arg) {
    (void)arg;

#if defined(__APPLE__)
    /* ---- kqueue ---- */
    struct kevent events[SW_IO_MAX_EVENTS];
    while (g_io_running) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        int n = kevent(g_kq, NULL, 0, events, SW_IO_MAX_EVENTS, &ts);
        for (int i = 0; i < n; i++) {
            if ((int)events[i].ident == g_wake_pipe[0]) {
                char buf[64];
                (void)read(g_wake_pipe[0], buf, sizeof(buf));
                continue;
            }
            sw_port_t *port = (sw_port_t *)events[i].udata;
            if (!port) continue;
            if (events[i].flags & EV_EOF) {
                if (port->type == SW_PORT_TCP_CONN) handle_read(port);
                continue;
            }
            switch (port->type) {
            case SW_PORT_TCP_LISTEN: handle_accept(port); break;
            case SW_PORT_TCP_CONN:   handle_read(port);   break;
            case SW_PORT_PIPE:       handle_read(port);   break;
            }
        }
    }

#elif defined(_WIN32)
    /* ---- WSAPoll ---- */
    WSAPOLLFD pollfds[SW_IO_MAX_EVENTS];
    sw_port_t *pollports[SW_IO_MAX_EVENTS];

    while (g_io_running) {
        int nfds = 0;

        /* Always poll the wake socket */
        pollfds[nfds].fd = g_wake_pipe[0];
        pollfds[nfds].events = POLLIN;
        pollfds[nfds].revents = 0;
        pollports[nfds] = NULL;
        nfds++;

        /* Build poll set from open ports */
        pthread_mutex_lock(&g_ports_lock);
        sw_port_t *p = g_ports;
        while (p && nfds < SW_IO_MAX_EVENTS) {
            if (p->state == SW_PORT_OPEN && p->fd != INVALID_SOCKET) {
                pollfds[nfds].fd = p->fd;
                pollfds[nfds].events = POLLIN;
                pollfds[nfds].revents = 0;
                pollports[nfds] = p;
                nfds++;
            }
            p = p->next;
        }
        pthread_mutex_unlock(&g_ports_lock);

        int rc = WSAPoll(pollfds, nfds, 100); /* 100ms timeout */
        if (rc <= 0) continue;

        for (int i = 0; i < nfds; i++) {
            if (pollfds[i].revents == 0) continue;

            if (i == 0) {
                /* Wake socket */
                char buf[64];
                recv(g_wake_pipe[0], buf, sizeof(buf), 0);
                continue;
            }

            sw_port_t *port = pollports[i];
            if (!port) continue;

            if (pollfds[i].revents & (POLLHUP | POLLERR)) {
                if (port->type == SW_PORT_TCP_CONN) handle_read(port);
                continue;
            }

            if (pollfds[i].revents & POLLIN) {
                switch (port->type) {
                case SW_PORT_TCP_LISTEN: handle_accept(port); break;
                case SW_PORT_TCP_CONN:   handle_read(port);   break;
                case SW_PORT_PIPE:       handle_read(port);   break;
                }
            }
        }
    }

#else
    /* ---- epoll ---- */
    struct epoll_event events[SW_IO_MAX_EVENTS];
    while (g_io_running) {
        int n = epoll_wait(g_kq, events, SW_IO_MAX_EVENTS, 100); /* 100ms */
        for (int i = 0; i < n; i++) {
            sw_port_t *port = (sw_port_t *)events[i].data.ptr;
            if (!port) {
                /* Wake pipe */
                char buf[64];
                (void)read(g_wake_pipe[0], buf, sizeof(buf));
                continue;
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                if (port->type == SW_PORT_TCP_CONN) handle_read(port);
                continue;
            }
            switch (port->type) {
            case SW_PORT_TCP_LISTEN: handle_accept(port); break;
            case SW_PORT_TCP_CONN:   handle_read(port);   break;
            case SW_PORT_PIPE:       handle_read(port);   break;
            }
        }
    }
#endif

    return NULL;
}

/* === Wake Pipe (loopback socket pair on Windows) === */

#ifdef _WIN32
static int create_wake_pipe(SOCKET fds[2]) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    int addrlen = sizeof(addr);
    getsockname(listener, (struct sockaddr *)&addr, &addrlen);

    fds[1] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fds[1] == INVALID_SOCKET) { closesocket(listener); return -1; }

    if (connect(fds[1], (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(fds[1]); closesocket(listener); return -1;
    }

    fds[0] = accept(listener, NULL, NULL);
    closesocket(listener);
    if (fds[0] == INVALID_SOCKET) { closesocket(fds[1]); return -1; }

    return 0;
}
#endif

/* === Public API === */

int sw_io_init(void) {
#ifdef _WIN32
    /* Init Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    if (create_wake_pipe(g_wake_pipe) < 0) return -1;
    set_nonblocking(g_wake_pipe[0]);
    set_nonblocking(g_wake_pipe[1]);
#else
  #ifdef __APPLE__
    g_kq = kqueue();
  #else
    g_kq = epoll_create1(0);
  #endif
    if (g_kq < 0) return -1;

    if (pipe(g_wake_pipe) < 0) {
        close(g_kq);
        return -1;
    }
    set_nonblocking(g_wake_pipe[0]);
    set_nonblocking(g_wake_pipe[1]);

    ev_register_read(g_wake_pipe[0], NULL);
#endif

    g_io_running = 1;
    pthread_create(&g_io_thread, NULL, io_loop, NULL);
    return 0;
}

void sw_io_shutdown(void) {
    g_io_running = 0;
    io_wake();
    pthread_join(g_io_thread, NULL);

    pthread_mutex_lock(&g_ports_lock);
    sw_port_t *p = g_ports;
    while (p) {
        sw_port_t *next = p->next;
        if (p->fd != SW_INVALID_FD) close_fd(p->fd);
        if (p->recv_buf) free(p->recv_buf);
        free(p);
        p = next;
    }
    g_ports = NULL;
    pthread_mutex_unlock(&g_ports_lock);

#ifdef _WIN32
    if (g_wake_pipe[0] != INVALID_SOCKET) closesocket(g_wake_pipe[0]);
    if (g_wake_pipe[1] != INVALID_SOCKET) closesocket(g_wake_pipe[1]);
    g_wake_pipe[0] = g_wake_pipe[1] = INVALID_SOCKET;
    WSACleanup();
#else
    if (g_wake_pipe[0] >= 0) close(g_wake_pipe[0]);
    if (g_wake_pipe[1] >= 0) close(g_wake_pipe[1]);
    g_wake_pipe[0] = g_wake_pipe[1] = -1;

    if (g_kq >= 0) close(g_kq);
    g_kq = -1;
#endif
}

sw_port_t *sw_tcp_listen(const char *addr, uint16_t port) {
    sw_fd_t fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) return NULL;
#else
    if (fd < 0) return NULL;
#endif

    set_reuseaddr(fd);
    set_nonblocking(fd);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (addr && addr[0]) {
        inet_pton(AF_INET, addr, &sin.sin_addr);
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close_fd(fd);
        return NULL;
    }

    if (listen(fd, 128) < 0) {
        close_fd(fd);
        return NULL;
    }

    sw_port_t *p = port_alloc(fd, SW_PORT_TCP_LISTEN, sw_self());
    ev_register_read(fd, p);
    io_wake();

    return p;
}

sw_port_t *sw_tcp_connect(const char *addr, uint16_t port) {
    sw_fd_t fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) return NULL;
#else
    if (fd < 0) return NULL;
#endif

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, addr, &sin.sin_addr);

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close_fd(fd);
        return NULL;
    }

    set_nonblocking(fd);
    set_nodelay(fd);

    sw_port_t *p = port_alloc(fd, SW_PORT_TCP_CONN, sw_self());
    ev_register_read(fd, p);
    io_wake();

    return p;
}

int sw_tcp_send(sw_port_t *port, const void *data, uint32_t len) {
    if (!port || port->state != SW_PORT_OPEN || port->type != SW_PORT_TCP_CONN)
        return -1;

#ifdef _WIN32
    int sent = send(port->fd, (const char *)data, len, 0);
#else
    ssize_t sent = write(port->fd, data, len);
#endif
    if (sent < 0) return -1;
    return (int)sent;
}

void sw_port_set_active(sw_port_t *port, int active) {
    if (!port) return;
    if (active) {
        ev_register_read(port->fd, port);
    } else {
        ev_deregister(port->fd);
    }
    io_wake();
}

void sw_port_close(sw_port_t *port) {
    if (!port || port->state == SW_PORT_CLOSED) return;

    ev_deregister(port->fd);
    port->state = SW_PORT_CLOSING;
    close_fd(port->fd);
    port->fd = SW_INVALID_FD;

    if (port->owner) {
        sw_port_event_t *msg = (sw_port_event_t *)malloc(sizeof(sw_port_event_t));
        msg->port = port;
        msg->error = 0;
        sw_send_tagged(port->owner, SW_TAG_PORT_CLOSED, msg);
    }

    port_remove(port);
    if (port->recv_buf) free(port->recv_buf);
    port->recv_buf = NULL;
    port->state = SW_PORT_CLOSED;
}

void sw_port_controlling_process(sw_port_t *port, sw_process_t *new_owner) {
    if (port) port->owner = new_owner;
}

void sw_io_cleanup_owner(sw_process_t *proc) {
    if (!proc) return;

    pthread_mutex_lock(&g_ports_lock);
    sw_port_t *p = g_ports;
    sw_port_t *to_close[256];
    int nclose = 0;
    while (p && nclose < 256) {
        if (p->owner == proc && p->state == SW_PORT_OPEN) {
            to_close[nclose++] = p;
        }
        p = p->next;
    }
    pthread_mutex_unlock(&g_ports_lock);

    for (int i = 0; i < nclose; i++) {
        to_close[i]->owner = NULL;
        ev_deregister(to_close[i]->fd);
        if (to_close[i]->fd != SW_INVALID_FD) {
            close_fd(to_close[i]->fd);
            to_close[i]->fd = SW_INVALID_FD;
        }
        to_close[i]->state = SW_PORT_CLOSED;
        port_remove(to_close[i]);
    }
}
