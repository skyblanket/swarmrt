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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
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
static _Atomic int64_t g_ports_live;       /* allocated sw_port_t structs */

/* Ports handed to sw_port_close_free, waiting for the IO thread to free
 * them between event batches (io_reap_retired). */
static sw_port_t *g_retired = NULL;
static pthread_mutex_t g_retired_lock = PTHREAD_MUTEX_INITIALIZER;

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
    atomic_fetch_add_explicit(&g_ports_live, 1, memory_order_relaxed);

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
    atomic_fetch_sub_explicit(&g_ports_live, 1, memory_order_relaxed);
}

/* Free the ports sw_port_close_free retired. Runs on the IO thread at the
 * top of each loop iteration, i.e. strictly between event batches: every
 * retired port was deregistered before it was retired, so no batch fetched
 * from here on can name it, and the batch that might have is finished. A
 * port someone still pins (refs > 0) goes back on the list for the next
 * pass (at most one poll timeout, 100ms, later). */
static void io_reap_retired(void) {
    pthread_mutex_lock(&g_retired_lock);
    sw_port_t *p = g_retired;
    g_retired = NULL;
    pthread_mutex_unlock(&g_retired_lock);

    sw_port_t *keep = NULL, *keep_tail = NULL;
    while (p) {
        sw_port_t *next = p->retire_next;
        if (atomic_load_explicit(&p->refs, memory_order_acquire) == 0) {
            port_free(p);
        } else {
            p->retire_next = NULL;
            if (keep_tail) keep_tail->retire_next = p; else keep = p;
            keep_tail = p;
        }
        p = next;
    }
    if (keep) {
        pthread_mutex_lock(&g_retired_lock);
        keep_tail->retire_next = g_retired;
        g_retired = keep;
        pthread_mutex_unlock(&g_retired_lock);
    }
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

    /* owner is handed over by sw_port_controlling_process on another thread
     * (http_listen transfers its listener to the bridge after creating it). */
    sw_process_t *owner = __atomic_load_n(&listener->owner, __ATOMIC_ACQUIRE);
    sw_port_t *conn = port_alloc(fd, SW_PORT_TCP_CONN, owner);
    ev_register_read(fd, conn);

    sw_port_accept_t *msg = (sw_port_accept_t *)malloc(sizeof(sw_port_accept_t));
    msg->listener = listener;
    msg->conn = conn;
    sw_send_tagged(owner, SW_TAG_PORT_ACCEPT, msg);
}

static void handle_read(sw_port_t *port) {
    /* Acquire: a port closed (and maybe retired) on another thread while it
     * sat in this batch reads as not-OPEN here and is left alone. */
    if (__atomic_load_n(&port->state, __ATOMIC_ACQUIRE) != SW_PORT_OPEN ||
        !port->owner) return;

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
        __atomic_store_n(&port->state, SW_PORT_CLOSING, __ATOMIC_RELEASE);

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
        io_reap_retired();
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
        io_reap_retired();
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
        io_reap_retired();
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

static int sw_io_init_locked(void);

int sw_io_init(void) {
    /* Idempotent. Every compiled main() initialises IO and http_listen() used
     * to call this again, which started a SECOND io thread on a fresh epoll
     * set (and overwrote g_kq under the first one): two threads then polled
     * the same sockets, so a connection's DATA could be delivered to the
     * bridge before its ACCEPT and was dropped (~1 in 200 requests lost
     * under concurrency), and reads could be split between threads. */
    static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&init_lock);
    if (g_io_running) { pthread_mutex_unlock(&init_lock); return 0; }
    int rc = sw_io_init_locked();
    pthread_mutex_unlock(&init_lock);
    return rc;
}

static int sw_io_init_locked(void) {
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
        atomic_fetch_sub_explicit(&g_ports_live, 1, memory_order_relaxed);
        p = next;
    }
    g_ports = NULL;
    pthread_mutex_unlock(&g_ports_lock);
    /* IO thread is joined: nothing can be mid-batch, so free retired ports
     * whatever their pins say. */
    pthread_mutex_lock(&g_retired_lock);
    p = g_retired;
    g_retired = NULL;
    pthread_mutex_unlock(&g_retired_lock);
    while (p) {
        sw_port_t *next = p->retire_next;
        port_free(p);
        p = next;
    }

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
#ifndef _WIN32
    // Close the listening socket in any forked child (shell() / etc.)
    // so a child can't accidentally accept inherited HTTP connections,
    // which causes incoming requests to hang. (Without CLOEXEC, the
    // kernel's accept queue picks ANY process holding the FD.)
    {
        int fl = fcntl(fd, F_GETFD, 0);
        if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
#endif

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (addr && addr[0]) {
        /* A malformed address used to leave sin_addr zeroed, i.e. silently
         * bind every interface — the opposite of what a bind setting asks
         * for. Refuse instead. */
        if (inet_pton(AF_INET, addr, &sin.sin_addr) != 1) {
            close_fd(fd);
            return NULL;
        }
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
    if (!port || __atomic_load_n(&port->state, __ATOMIC_ACQUIRE) != SW_PORT_OPEN ||
        port->type != SW_PORT_TCP_CONN)
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
    __atomic_store_n(&port->state, SW_PORT_CLOSING, __ATOMIC_RELEASE);
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
    __atomic_store_n(&port->state, SW_PORT_CLOSED, __ATOMIC_RELEASE);
}

void sw_port_close_free(sw_port_t *port) {
    if (!port) return;
    ev_deregister(port->fd);
    __atomic_store_n(&port->state, SW_PORT_CLOSING, __ATOMIC_RELEASE);
    /* shutdown, not close: the peer gets its FIN now, but the fd NUMBER stays
     * ours until the reaper closes it, so a read the IO thread already has in
     * flight on it can't land on a freshly accepted socket reusing the
     * number. (sw_port_close closes here and has that race.) */
    if (port->fd != SW_INVALID_FD) {
#ifdef _WIN32
        shutdown(port->fd, SD_BOTH);
#else
        shutdown(port->fd, SHUT_RDWR);
#endif
    }
    port_remove(port);

    pthread_mutex_lock(&g_retired_lock);
    port->retire_next = g_retired;
    g_retired = port;
    pthread_mutex_unlock(&g_retired_lock);
    io_wake();   /* reap (and close the fd) promptly, not on the poll timeout */
}

void sw_port_ref(sw_port_t *port) {
    if (port) atomic_fetch_add_explicit(&port->refs, 1, memory_order_relaxed);
}

void sw_port_unref(sw_port_t *port) {
    if (port) atomic_fetch_sub_explicit(&port->refs, 1, memory_order_release);
}

int64_t sw_io_ports_live(void) {
    return atomic_load_explicit(&g_ports_live, memory_order_relaxed);
}

void sw_port_controlling_process(sw_port_t *port, sw_process_t *new_owner) {
    if (port) __atomic_store_n(&port->owner, new_owner, __ATOMIC_RELEASE);
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

/* Count ports still capable of delivering an event (SW_PORT_OPEN with a
 * live owner). The deadlock watchdog calls this: an idle TCP/HTTP server
 * legitimately parks every process in `receive` with an empty mailbox
 * while it waits for the I/O thread to deliver an accept/data event, so a
 * non-zero count means "not deadlocked, just waiting on I/O". */
int sw_io_active_port_count(void) {
    if (!g_io_running) return 0;
    int n = 0;
    pthread_mutex_lock(&g_ports_lock);
    for (sw_port_t *p = g_ports; p; p = p->next) {
        if (p->state == SW_PORT_OPEN && p->owner) n++;
    }
    pthread_mutex_unlock(&g_ports_lock);
    return n;
}
