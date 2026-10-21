/* http_keepalive_client.c — concurrent keep-alive client for the HTTP
 * response-delivery gate (tests/stress/http_keepalive_gate.sh). Plain
 * sockets, no runtime link.
 *
 * Usage: http_keepalive_client <port> <clients> <requests-per-client>
 *
 * Each client thread holds ONE keep-alive connection and sends
 * <requests-per-client> POSTs in sequence, each body unique to (client,
 * request). The server echoes the body, so every response must be exactly
 * the body that was sent, in order, with no extra bytes after it. Every
 * other request is split into a header write and a body write, so the
 * server sees the body arrive in a later read (the body_pending path).
 *
 * A failure is: no response (EOF / reset / timeout), a non-200 status, a
 * body that isn't the one sent (a response routed to the wrong request), or
 * leftover bytes after a response (a duplicate). Exit 0 = every request got
 * exactly one correct response; 1 = otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define REPLY_TIMEOUT_MS 10000

typedef struct {
    int port;
    int id;
    int requests;
    int ok;
    int failed;
    int bad_req;
    char why[512];
} client_t;

static int tcp_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static int send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* Read until `want` bytes are buffered or the deadline passes. Returns the
 * number of bytes in buf, or -1 on EOF/error/timeout. */
static long read_until(int fd, char *buf, size_t cap, size_t *len, size_t want) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (*len < want) {
        if (*len >= cap) return -1;
        int pr = poll(&pfd, 1, REPLY_TIMEOUT_MS);
        if (pr <= 0) return -1;
        ssize_t r = recv(fd, buf + *len, cap - *len, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        *len += (size_t)r;
    }
    return (long)*len;
}

/* Read one full HTTP response whose body must equal `expect`. Leaves any
 * bytes after the response in buf (so a duplicate is caught). */
static int read_reply(int fd, const char *expect, size_t elen, char *buf,
                      size_t cap, size_t *len, char *why) {
    char *hend = NULL;
    while (!(hend = memmem(buf, *len, "\r\n\r\n", 4))) {
        if (read_until(fd, buf, cap, len, *len + 1) < 0) {
            snprintf(why, 256, "no response (EOF, reset or timeout)");
            return 0;
        }
    }
    size_t hlen = (size_t)(hend - buf) + 4;
    if (strncmp(buf, "HTTP/1.1 200 ", 13) != 0) {
        snprintf(why, 256, "status line not 200: %.40s", buf);
        return 0;
    }
    const char *cl = memmem(buf, hlen, "Content-Length: ", 16);
    if (!cl) { snprintf(why, 256, "no Content-Length"); return 0; }
    size_t blen = strtoul(cl + 16, NULL, 10);
    if (read_until(fd, buf, cap, len, hlen + blen) < 0) {
        snprintf(why, 256, "body cut short (want %zu bytes)", blen);
        return 0;
    }
    if (blen != elen || memcmp(buf + hlen, expect, elen) != 0) {
        snprintf(why, 256, "body is not the one sent (got %zu bytes)", blen);
        return 0;
    }
    memmove(buf, buf + hlen + blen, *len - hlen - blen);
    *len -= hlen + blen;
    return 1;
}

static void *run_client(void *arg) {
    client_t *cl = (client_t *)arg;
    size_t cap = 65536;
    char *buf = malloc(cap);
    char *body = malloc(256);
    int fd = tcp_connect(cl->port);
    if (fd < 0) { snprintf(cl->why, sizeof(cl->why), "connect failed"); cl->failed = 1; free(buf); free(body); return NULL; }

    size_t len = 0;
    for (int r = 0; r < cl->requests; r++) {
        int blen = snprintf(body, 256, "c%03d-r%04d-%s", cl->id, r, "abcdefghijklmnopqrstuvwxyz");
        char head[256];
        int hlen = snprintf(head, sizeof(head),
            "POST /echo HTTP/1.1\r\nHost: l\r\nContent-Type: text/plain\r\n"
            "Content-Length: %d\r\n\r\n", blen);
        if (r % 2 == 0) {
            if (send_all(fd, head, (size_t)hlen) < 0 || send_all(fd, body, (size_t)blen) < 0) {
                snprintf(cl->why, sizeof(cl->why), "send failed at request %d", r);
                cl->failed = 1; break;
            }
        } else {
            if (send_all(fd, head, (size_t)hlen) < 0) { cl->failed = 1; break; }
            usleep(200);
            if (send_all(fd, body, (size_t)blen) < 0) { cl->failed = 1; break; }
        }
        if (!read_reply(fd, body, (size_t)blen, buf, cap, &len, cl->why)) {
            cl->failed = 1;
            cl->bad_req = r;
            break;
        }
        cl->ok++;
    }
    if (!cl->failed && len > 0) {
        snprintf(cl->why, sizeof(cl->why), "%zu stray bytes after the last reply (duplicate response)", len);
        cl->failed = 1;
    }
    close(fd);
    free(buf);
    free(body);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <port> <clients> <requests-per-client>\n", argv[0]);
        return 2;
    }
    int port = atoi(argv[1]);
    int n = atoi(argv[2]);
    int reqs = atoi(argv[3]);
    if (n <= 0 || reqs <= 0) return 2;

    pthread_t *th = calloc((size_t)n, sizeof(pthread_t));
    client_t *cl = calloc((size_t)n, sizeof(client_t));
    for (int i = 0; i < n; i++) {
        cl[i].port = port;
        cl[i].id = i;
        cl[i].requests = reqs;
        pthread_create(&th[i], NULL, run_client, &cl[i]);
    }

    int ok = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        pthread_join(th[i], NULL);
        ok += cl[i].ok;
        if (cl[i].failed) {
            bad++;
            if (bad <= 5) fprintf(stderr, "client %d, request %d: %s\n", cl[i].id, cl[i].bad_req, cl[i].why);
        }
    }
    printf("keepalive: %d/%d requests answered correctly, %d/%d clients clean\n",
           ok, n * reqs, n - bad, n);
    free(th);
    free(cl);
    return bad ? 1 : 0;
}
