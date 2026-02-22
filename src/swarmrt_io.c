/*
 * SwarmRT Phase 6: IO System (kqueue + TCP)
 *
 * Architecture:
 * - Dedicated IO thread runs kqueue event loop
 * - Ports are registered with kqueue for read events
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
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include "swarmrt_io.h"

/* === Global IO State === */

#define SW_IO_MAX_EVENTS 64
#define SW_IO_RECV_BUF   4096

static int g_kq = -1;
static pthread_t g_io_thread;
static volatile int g_io_running = 0;

static sw_port_t *g_ports = NULL;           /* Global port list */
static pthread_mutex_t g_ports_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t g_next_port_id = 1;

/* Wake pipe for signaling the IO thread */
static int g_wake_pipe[2] = {-1, -1};

/* === Internal Helpers === */

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void set_reuseaddr(int fd) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

static sw_port_t *port_alloc(int fd, sw_port_type_t type, sw_process_t *owner) {
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
    if (port->fd >= 0) {
        close(port->fd);
        port->fd = -1;
    }
    if (port->recv_buf) {
        free(port->recv_buf);
        port->recv_buf = NULL;
    }
    port->state = SW_PORT_CLOSED;
    free(port);
}

static void kq_register_read(int fd, void *udata) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, udata);
    kevent(g_kq, &ev, 1, NULL, 0, NULL);
}

static void kq_deregister(int fd) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(g_kq, &ev, 1, NULL, 0, NULL);
}

static void io_wake(void) {
    char c = 1;
    (void)write(g_wake_pipe[1], &c, 1);
}

/* === Handle kqueue events === */

static void handle_accept(sw_port_t *listener) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(listener->fd, (struct sockaddr *)&addr, &len);
    if (fd < 0) return;

    set_nonblocking(fd);
    set_nodelay(fd);

    /* Create new connection port owned by the listener's owner */
    sw_port_t *conn = port_alloc(fd, SW_PORT_TCP_CONN, listener->owner);

    /* Register for read events */
    kq_register_read(fd, conn);

    /* Send accept notification */
    sw_port_accept_t *msg = (sw_port_accept_t *)malloc(sizeof(sw_port_accept_t));
    msg->listener = listener;
    msg->conn = conn;
    sw_send_tagged(listener->owner, SW_TAG_PORT_ACCEPT, msg);
}

static void handle_read(sw_port_t *port) {
    if (port->state != SW_PORT_OPEN || !port->owner) return;

    ssize_t n = read(port->fd, port->recv_buf, port->recv_buf_size);

    if (n > 0) {
        /* Copy data and send to owner */
        uint8_t *copy = (uint8_t *)malloc(n);
        memcpy(copy, port->recv_buf, n);

        sw_port_data_t *msg = (sw_port_data_t *)malloc(sizeof(sw_port_data_t));
        msg->port = port;
        msg->data = copy;
        msg->len = (uint32_t)n;
        sw_send_tagged(port->owner, SW_TAG_PORT_DATA, msg);
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        /* Connection closed or error */
        kq_deregister(port->fd);
        port->state = SW_PORT_CLOSING;

        sw_port_event_t *msg = (sw_port_event_t *)malloc(sizeof(sw_port_event_t));
        msg->port = port;
        msg->error = (n == 0) ? 0 : errno;
        sw_send_tagged(port->owner, SW_TAG_PORT_CLOSED, msg);
    }
}

/* === IO Thread === */

static void *io_loop(void *arg) {
    (void)arg;

    struct kevent events[SW_IO_MAX_EVENTS];

    while (g_io_running) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        int n = kevent(g_kq, NULL, 0, events, SW_IO_MAX_EVENTS, &ts);

        for (int i = 0; i < n; i++) {
            if ((int)events[i].ident == g_wake_pipe[0]) {
                /* Wake signal — drain pipe */
                char buf[64];
                (void)read(g_wake_pipe[0], buf, sizeof(buf));
                continue;
            }

            sw_port_t *port = (sw_port_t *)events[i].udata;
            if (!port) continue;

            if (events[i].flags & EV_EOF) {
                if (port->type == SW_PORT_TCP_CONN) {
                    handle_read(port); /* Will detect EOF */
                }
                continue;
            }

            switch (port->type) {
            case SW_PORT_TCP_LISTEN:
                handle_accept(port);
                break;
            case SW_PORT_TCP_CONN:
                handle_read(port);
                break;
            case SW_PORT_PIPE:
                handle_read(port);
                break;
            }
        }
    }

    return NULL;
}

/* === Public API === */

int sw_io_init(void) {
    g_kq = kqueue();
    if (g_kq < 0) return -1;

    /* Wake pipe */
    if (pipe(g_wake_pipe) < 0) {
        close(g_kq);
        return -1;
    }
    set_nonblocking(g_wake_pipe[0]);
    set_nonblocking(g_wake_pipe[1]);

    /* Register wake pipe for reading */
    kq_register_read(g_wake_pipe[0], NULL);

    g_io_running = 1;
    pthread_create(&g_io_thread, NULL, io_loop, NULL);
    return 0;
}

void sw_io_shutdown(void) {
    g_io_running = 0;
    io_wake();
    pthread_join(g_io_thread, NULL);

    /* Close all ports */
    pthread_mutex_lock(&g_ports_lock);
    sw_port_t *p = g_ports;
    while (p) {
        sw_port_t *next = p->next;
        if (p->fd >= 0) close(p->fd);
        if (p->recv_buf) free(p->recv_buf);
        free(p);
        p = next;
    }
    g_ports = NULL;
    pthread_mutex_unlock(&g_ports_lock);

    if (g_wake_pipe[0] >= 0) close(g_wake_pipe[0]);
    if (g_wake_pipe[1] >= 0) close(g_wake_pipe[1]);
    g_wake_pipe[0] = g_wake_pipe[1] = -1;

    if (g_kq >= 0) close(g_kq);
    g_kq = -1;
}

sw_port_t *sw_tcp_listen(const char *addr, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

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
        close(fd);
        return NULL;
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        return NULL;
    }

    sw_port_t *p = port_alloc(fd, SW_PORT_TCP_LISTEN, sw_self());
    kq_register_read(fd, p);
    io_wake(); /* Poke IO thread to pick up new event */

    return p;
}

sw_port_t *sw_tcp_connect(const char *addr, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, addr, &sin.sin_addr);

    /* Blocking connect (then switch to nonblocking for IO) */
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(fd);
        return NULL;
    }

    set_nonblocking(fd);
    set_nodelay(fd);

    sw_port_t *p = port_alloc(fd, SW_PORT_TCP_CONN, sw_self());
    kq_register_read(fd, p);
    io_wake();

    return p;
}

int sw_tcp_send(sw_port_t *port, const void *data, uint32_t len) {
    if (!port || port->state != SW_PORT_OPEN || port->type != SW_PORT_TCP_CONN)
        return -1;

    ssize_t sent = write(port->fd, data, len);
    if (sent < 0) return -1;
    return (int)sent;
}

void sw_port_set_active(sw_port_t *port, int active) {
    if (!port) return;
    if (active) {
        kq_register_read(port->fd, port);
    } else {
        kq_deregister(port->fd);
    }
    io_wake();
}

void sw_port_close(sw_port_t *port) {
    if (!port || port->state == SW_PORT_CLOSED) return;

    kq_deregister(port->fd);
    port->state = SW_PORT_CLOSING;
    close(port->fd);
    port->fd = -1;

    /* Notify owner */
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
    /* Don't free the port struct yet — owner might still reference it */
}

void sw_port_controlling_process(sw_port_t *port, sw_process_t *new_owner) {
    if (port) port->owner = new_owner;
}

void sw_io_cleanup_owner(sw_process_t *proc) {
    if (!proc) return;

    /* Close all ports owned by this process */
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
        to_close[i]->owner = NULL; /* Prevent notifications to dead process */
        kq_deregister(to_close[i]->fd);
        if (to_close[i]->fd >= 0) {
            close(to_close[i]->fd);
            to_close[i]->fd = -1;
        }
        to_close[i]->state = SW_PORT_CLOSED;
        port_remove(to_close[i]);
    }
}
