/* slowloris_client.c — attack/probe client for the HTTP idle-timeout gate
 * (SW_HTTP_IDLE_TIMEOUT_MS; tests/stress/slowloris_gate.sh). Plain sockets,
 * no runtime link.
 *
 * Modes:
 *   probe  <port>       one real request; exit 0 iff "200" comes back.
 *                       (Used by the gate to wait for server boot.)
 *   sweep  <port> <n>   THE FIX WORKS: establish 1 WS conn (upgrade
 *                       completed), then open n idle HTTP conns (send
 *                       nothing). Expect: every idle conn is CLOSED by the
 *                       server within the deadline (slot freed), the quiet
 *                       WS conn SURVIVES (established WS exempt by default),
 *                       and a real request then succeeds.
 *   pinned <port> <n>   THE BUG IS PRESENT (timeout disabled): open n idle
 *                       conns, wait well past the would-be timeout, expect
 *                       ZERO closes (slots stay pinned) — and with the table
 *                       full (n = SW_HTTP_MAX_CONNS) a real request FAILS.
 *
 * Exit 0 = expectations met; 1 = not; prints one PASS/FAIL line per check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MAX_IDLE 1024

static int tcp_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    return fd;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* One real HTTP request; returns 1 iff an HTTP 200 status line comes back
 * (a table-full server closes the socket without any response). */
static int real_request(int port) {
    int fd = tcp_connect(port);
    if (fd < 0) return 0;
    const char *req = "GET / HTTP/1.1\r\nHost: l\r\nConnection: close\r\n\r\n";
    if (send(fd, req, strlen(req), 0) < 0) { close(fd); return 0; }
    char buf[512];
    int n = 0, total = 0;
    /* Read with a 3s deadline until close or buffer full. */
    struct pollfd p = { fd, POLLIN, 0 };
    uint64_t deadline = now_ms() + 3000;
    while (total < (int)sizeof(buf) - 1) {
        int left = (int)(deadline - now_ms());
        if (left <= 0) break;
        if (poll(&p, 1, left) <= 0) break;
        n = (int)recv(fd, buf + total, sizeof(buf) - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += n;
        if (strstr(buf, "\r\n\r\n")) break;   /* headers in; status line known */
    }
    close(fd);
    buf[total] = '\0';
    return strncmp(buf, "HTTP/1.1 200", 12) == 0 || strncmp(buf, "HTTP/1.0 200", 12) == 0;
}

/* Establish a WebSocket connection (handshake to 101 complete). Returns fd
 * or -1. Key is static — the gate doesn't validate the accept hash. */
static int ws_connect_upgrade(int port) {
    int fd = tcp_connect(port);
    if (fd < 0) return -1;
    const char *req =
        "GET /ws HTTP/1.1\r\nHost: l\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (send(fd, req, strlen(req), 0) < 0) { close(fd); return -1; }
    char buf[1024];
    int total = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    uint64_t deadline = now_ms() + 3000;
    while (total < (int)sizeof(buf) - 1) {
        int left = (int)(deadline - now_ms());
        if (left <= 0) break;
        if (poll(&p, 1, left) <= 0) break;
        int n = (int)recv(fd, buf + total, sizeof(buf) - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[total] = '\0';
    if (strncmp(buf, "HTTP/1.1 101", 12) != 0) { close(fd); return -1; }
    return fd;
}

/* Count how many of fds[] are CLOSED (EOF/RST readable) right now; closed
 * entries are set to -1. Idle HTTP conns are sent nothing by the server, so
 * any POLLIN is EOF (or an error — either way, the slot was released). */
static int reap_closed(int *fds, int n) {
    int closed = 0;
    for (int i = 0; i < n; i++) {
        if (fds[i] < 0) { closed++; continue; }
        struct pollfd p = { fds[i], POLLIN, 0 };
        if (poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            char b[64];
            int r = (int)recv(fds[i], b, sizeof(b), 0);
            if (r <= 0) { close(fds[i]); fds[i] = -1; closed++; }
        }
    }
    return closed;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3) { fprintf(stderr, "usage: %s probe|sweep|pinned <port> [n]\n", argv[0]); return 2; }
    const char *mode = argv[1];
    int port = atoi(argv[2]);
    int n = argc > 3 ? atoi(argv[3]) : 64;
    if (n > MAX_IDLE) n = MAX_IDLE;

    if (strcmp(mode, "probe") == 0)
        return real_request(port) ? 0 : 1;

    int fails = 0;

    /* sweep mode: quiet-but-established WS conn first (one slot). */
    int wsfd = -1;
    if (strcmp(mode, "sweep") == 0) {
        wsfd = ws_connect_upgrade(port);
        if (wsfd < 0) { printf("FAIL ws_establish\n"); return 1; }
        printf("PASS ws_establish\n");
    }

    /* Open n idle connections (send nothing — the attack). */
    static int fds[MAX_IDLE];
    int opened = 0;
    for (int i = 0; i < n; i++) {
        fds[i] = tcp_connect(port);
        if (fds[i] >= 0) opened++;
    }
    if (opened != n) {
        printf("FAIL idle_open (opened %d of %d)\n", opened, n);
        return 1;
    }
    printf("PASS idle_open (%d conns)\n", opened);

    if (strcmp(mode, "sweep") == 0) {
        /* Expect ALL idle conns closed by the sweep within 10s
         * (gate runs SW_HTTP_IDLE_TIMEOUT_MS=500, tick 125ms). */
        uint64_t deadline = now_ms() + 10000;
        int closed = 0;
        while (now_ms() < deadline) {
            closed = reap_closed(fds, n);
            if (closed == n) break;
            usleep(100 * 1000);
        }
        if (closed == n) printf("PASS idle_closed (%d/%d within deadline)\n", closed, n);
        else { printf("FAIL idle_closed (%d/%d)\n", closed, n); fails++; }

        /* The quiet WS conn must have SURVIVED (established WS exempt). */
        struct pollfd wp = { wsfd, POLLIN, 0 };
        int ws_dead = 0;
        if (poll(&wp, 1, 0) > 0 && (wp.revents & (POLLIN | POLLHUP | POLLERR))) {
            char b[64];
            if (recv(wsfd, b, sizeof(b), 0) <= 0) ws_dead = 1;
        }
        if (!ws_dead) printf("PASS ws_survives_idle\n");
        else { printf("FAIL ws_survives_idle (server closed quiet WS conn)\n"); fails++; }
        close(wsfd);

        /* Slots are free again: a real request must succeed. */
        if (real_request(port)) printf("PASS request_after_sweep\n");
        else { printf("FAIL request_after_sweep\n"); fails++; }
        return fails ? 1 : 0;
    }

    /* pinned mode (timeout disabled): wait well past the would-be timeout —
     * NOTHING may be closed (the slots stay pinned forever)... */
    usleep(2500 * 1000);
    int closed = reap_closed(fds, n);
    if (closed == 0) printf("PASS slots_pinned (0/%d closed after 2.5s)\n", n);
    else { printf("FAIL slots_pinned (%d/%d closed — timeout should be off)\n", closed, n); fails++; }

    /* ...and with the conn table full, a real request FAILS (this is the
     * DoS the timeout exists to prevent). */
    if (!real_request(port)) printf("PASS request_starved (table full, request refused)\n");
    else { printf("FAIL request_starved (request succeeded — table not pinned?)\n"); fails++; }

    for (int i = 0; i < n; i++) if (fds[i] >= 0) close(fds[i]);
    return fails ? 1 : 0;
}
