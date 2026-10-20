/*
 * test_http_native.c — C-level tests for the native HTTP client
 * (src/swarmrt_http_native.c).
 *
 * Each case starts a one-shot loopback server on a thread that speaks raw
 * bytes, so the client sees exactly the wire shapes under test: chunked
 * (whole and byte-by-byte), a 1 MB body each way, a reply cut short, a hung
 * server against a 500 ms deadline, an interrupt, read-to-EOF framing and a
 * 100 Continue. Build and run with `make test-http-native`.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "swarmrt_http_native.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, name) do {                                  \
    if (cond) { printf("PASS %s\n", name); g_pass++; }          \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

#define MB (1024u * 1024u)

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* === One-shot loopback server ================================= */

typedef enum {
    SRV_CHUNKED,        /* chunked reply, one write */
    SRV_CHUNKED_SLOW,   /* same reply, one byte per write */
    SRV_ECHO_BIG,       /* reads a Content-Length body, verifies it, replies with it */
    SRV_MIDCLOSE,       /* promises 100 bytes, sends 3, closes */
    SRV_HANG,           /* reads the request, never answers */
    SRV_EOF_FRAMED,     /* HTTP/1.0 reply, no length: body runs to the close */
    SRV_INTERIM,        /* 100 Continue, then the real reply */
} srv_mode_t;

typedef struct {
    srv_mode_t mode;
    uint16_t port;
    int lfd;
    int echo_ok;        /* SRV_ECHO_BIG: body matched the pattern */
    volatile int stop;  /* SRV_HANG: set by the test to release the server */
} srv_t;

static void pattern_fill(char *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = (char)('a' + (int)(i % 251) % 26);
}

static void send_all(int c, const char *p, size_t n) {
    while (n > 0) {
        ssize_t k = send(c, p, n, MSG_NOSIGNAL);
        if (k < 0) { if (errno == EINTR) continue; return; }
        p += k;
        n -= (size_t)k;
    }
}

/* Read the request head, then its Content-Length body. Returns the body
 * (malloc'd, may be NULL) and its length. */
static char *read_request(int c, size_t *blen) {
    char *hdr = malloc(65536);
    size_t hl = 0, want = 0;
    int have_end = 0;
    *blen = 0;
    while (!have_end && hl < 65536 - 1) {
        ssize_t k = recv(c, hdr + hl, 65536 - 1 - hl, 0);
        if (k <= 0) break;
        hl += (size_t)k;
        hdr[hl] = '\0';
        have_end = strstr(hdr, "\r\n\r\n") != NULL;
    }
    const char *cl = strcasestr(hdr, "Content-Length:");
    if (cl) want = (size_t)strtoull(cl + 15, NULL, 10);
    const char *end = strstr(hdr, "\r\n\r\n");
    size_t have_body = end ? hl - (size_t)(end + 4 - hdr) : 0;
    char *body = malloc(want ? want : 1);
    if (have_body > want) have_body = want;
    if (have_body) memcpy(body, end + 4, have_body);
    while (have_body < want) {
        ssize_t k = recv(c, body + have_body, want - have_body, 0);
        if (k <= 0) break;
        have_body += (size_t)k;
    }
    *blen = have_body;
    free(hdr);
    return body;
}

static void *srv_main(void *arg) {
    srv_t *g = (srv_t *)arg;
    int c = accept(g->lfd, NULL, NULL);
    if (c < 0) return NULL;
    size_t blen = 0;
    char *body = read_request(c, &blen);

    switch (g->mode) {
    case SRV_CHUNKED: {
        const char *r =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n"
            "5;ext=1\r\nhello\r\n"
            "1\r\n \r\n"
            "5\r\nworld\r\n"
            "0\r\nX-Trailer: t\r\n\r\n";
        send_all(c, r, strlen(r));
        break;
    }
    case SRV_CHUNKED_SLOW: {
        const char *r =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "5\r\nhello\r\n1\r\n \r\n5\r\nworld\r\n0\r\n\r\n";
        for (size_t i = 0; i < strlen(r); i++) {
            send_all(c, r + i, 1);
            usleep(200);
        }
        break;
    }
    case SRV_ECHO_BIG: {
        char *want = malloc(MB);
        pattern_fill(want, MB);
        g->echo_ok = (blen == MB && memcmp(body, want, MB) == 0);
        free(want);
        char head[128];
        int hl = snprintf(head, sizeof(head),
                          "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", blen);
        send_all(c, head, (size_t)hl);
        send_all(c, body, blen);
        break;
    }
    case SRV_MIDCLOSE: {
        const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc";
        send_all(c, r, strlen(r));
        break;
    }
    case SRV_HANG: {
        for (int i = 0; i < 500 && !g->stop; i++) usleep(10000);   /* up to 5 s */
        break;
    }
    case SRV_EOF_FRAMED: {
        const char *r = "HTTP/1.0 200 OK\r\nServer: t\r\n\r\nhello-eof";
        send_all(c, r, strlen(r));
        break;
    }
    case SRV_INTERIM: {
        const char *r =
            "HTTP/1.1 100 Continue\r\n\r\n"
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        send_all(c, r, strlen(r));
        break;
    }
    }
    free(body);
    close(c);
    return NULL;
}

static int srv_start(srv_t *g, srv_mode_t mode, pthread_t *th) {
    memset(g, 0, sizeof(*g));
    g->mode = mode;
    g->lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(g->lfd, (struct sockaddr *)&a, sizeof(a)) != 0) return -1;
    if (listen(g->lfd, 4) != 0) return -1;
    socklen_t l = sizeof(a);
    getsockname(g->lfd, (struct sockaddr *)&a, &l);
    g->port = ntohs(a.sin_port);
    return pthread_create(th, NULL, srv_main, g);
}

static void srv_finish(srv_t *g, pthread_t th) {
    pthread_join(th, NULL);
    close(g->lfd);
}

static void url_for(char *buf, size_t cap, uint16_t port, const char *path) {
    snprintf(buf, cap, "http://127.0.0.1:%u%s", (unsigned)port, path);
}

/* === Cases ==================================================== */

static void test_chunked(srv_mode_t mode, const char *label) {
    srv_t g; pthread_t th;
    if (srv_start(&g, mode, &th) != 0) { CHECK(0, label); return; }
    char url[128]; url_for(url, sizeof(url), g.port, "/c");
    sw_http_native_req_t rq = { .method = "GET", .url = url, .interrupt_fd = -1 };
    sw_http_native_resp_t rs;
    int st = sw_http_native_request(&rq, &rs);
    srv_finish(&g, th);
    CHECK(st == 200 && rs.status == 200, label);
    CHECK(rs.body && strcmp(rs.body, "hello world") == 0 && rs.body_len == 11,
          mode == SRV_CHUNKED ? "chunked: body reassembled from chunks" : "chunked slow: body reassembled");
    CHECK(rs.headers && strstr(rs.headers, "Transfer-Encoding: chunked") != NULL,
          mode == SRV_CHUNKED ? "chunked: headers kept" : "chunked slow: headers kept");
    sw_http_native_resp_free(&rs);
}

static void test_big_both_ways(void) {
    srv_t g; pthread_t th;
    if (srv_start(&g, SRV_ECHO_BIG, &th) != 0) { CHECK(0, "1MB: server"); return; }
    char url[128]; url_for(url, sizeof(url), g.port, "/echo");
    char *payload = malloc(MB);
    pattern_fill(payload, MB);
    static const char *hdrs[] = { "Content-Type: application/octet-stream" };
    sw_http_native_req_t rq = {
        .method = "POST", .url = url, .headers = hdrs, .nheaders = 1,
        .body = payload, .body_len = MB, .interrupt_fd = -1,
    };
    sw_http_native_resp_t rs;
    int st = sw_http_native_request(&rq, &rs);
    srv_finish(&g, th);
    CHECK(g.echo_ok, "1MB request body arrives intact");
    CHECK(st == 200 && rs.body_len == MB && memcmp(rs.body, payload, MB) == 0,
          "1MB response body read intact");
    sw_http_native_resp_free(&rs);
    free(payload);
}

static void test_midclose(void) {
    srv_t g; pthread_t th;
    if (srv_start(&g, SRV_MIDCLOSE, &th) != 0) { CHECK(0, "midclose: server"); return; }
    char url[128]; url_for(url, sizeof(url), g.port, "/m");
    sw_http_native_req_t rq = { .url = url, .interrupt_fd = -1 };
    sw_http_native_resp_t rs;
    int st = sw_http_native_request(&rq, &rs);
    srv_finish(&g, th);
    CHECK(st == 0 && rs.status == 0 && rs.body == NULL, "server closes mid-reply: transport error");
    CHECK(strstr(rs.err, "closed") != NULL, "server closes mid-reply: error says closed");
    sw_http_native_resp_free(&rs);
}

static void test_timeout(void) {
    srv_t g; pthread_t th;
    g.stop = 0;
    if (srv_start(&g, SRV_HANG, &th) != 0) { CHECK(0, "timeout: server"); return; }
    char url[128]; url_for(url, sizeof(url), g.port, "/hang");
    sw_http_native_req_t rq = { .url = url, .timeout_ms = 500, .interrupt_fd = -1 };
    sw_http_native_resp_t rs;
    uint64_t t0 = now_ms();
    int st = sw_http_native_request(&rq, &rs);
    uint64_t dt = now_ms() - t0;
    g.stop = 1;
    srv_finish(&g, th);
    CHECK(st == 0 && rs.status == 0, "hung server: request fails");
    CHECK(dt >= 450 && dt < 1500, "hung server: returns near timeout_ms (500)");
    CHECK(strstr(rs.err, "timed out") != NULL, "hung server: error says timed out");
    sw_http_native_resp_free(&rs);
}

static int on_intr(void *ctx) {
    int fd = *(int *)ctx;
    char b;
    return read(fd, &b, 1) == 1 ? 1 : -1;
}

static void *poke_later(void *arg) {
    usleep(200000);
    int fd = *(int *)arg;
    ssize_t k = write(fd, "x", 1);
    (void)k;
    return NULL;
}

static void test_interrupt(void) {
    srv_t g; pthread_t th;
    g.stop = 0;
    if (srv_start(&g, SRV_HANG, &th) != 0) { CHECK(0, "interrupt: server"); return; }
    int p[2];
    if (pipe(p) != 0) { CHECK(0, "interrupt: pipe"); g.stop = 1; srv_finish(&g, th); return; }
    char url[128]; url_for(url, sizeof(url), g.port, "/hang");
    pthread_t poker;
    pthread_create(&poker, NULL, poke_later, &p[1]);
    sw_http_native_req_t rq = {
        .url = url, .timeout_ms = 10000, .interrupt_fd = p[0],
        .on_interrupt = on_intr, .ctx = &p[0],
    };
    sw_http_native_resp_t rs;
    uint64_t t0 = now_ms();
    int st = sw_http_native_request(&rq, &rs);
    uint64_t dt = now_ms() - t0;
    pthread_join(poker, NULL);
    g.stop = 1;
    srv_finish(&g, th);
    close(p[0]);
    close(p[1]);
    CHECK(st == 0 && rs.interrupted == 1, "interrupt fd aborts the request");
    CHECK(dt < 3000, "interrupt returns well before the 10 s budget");
    sw_http_native_resp_free(&rs);
}

/* The cancel hook stands in for a caller that dies: it reports true once
 * 200 ms have passed. */
static uint64_t g_cancel_after;
static int on_cancel(void *ctx) {
    (void)ctx;
    return now_ms() >= g_cancel_after;
}

static void test_cancel(void) {
    srv_t g; pthread_t th;
    g.stop = 0;
    if (srv_start(&g, SRV_HANG, &th) != 0) { CHECK(0, "cancel: server"); return; }
    char url[128]; url_for(url, sizeof(url), g.port, "/hang");
    g_cancel_after = now_ms() + 200;
    sw_http_native_req_t rq = {
        .url = url, .timeout_ms = 10000, .interrupt_fd = -1,
        .cancelled = on_cancel,
    };
    sw_http_native_resp_t rs;
    uint64_t t0 = now_ms();
    int st = sw_http_native_request(&rq, &rs);
    uint64_t dt = now_ms() - t0;
    g.stop = 1;
    srv_finish(&g, th);
    CHECK(st == 0 && rs.interrupted == 1, "cancel hook aborts the request");
    CHECK(dt >= 150 && dt < 3000, "cancel returns soon after the caller dies, not at the budget");
    sw_http_native_resp_free(&rs);
}

static void test_eof_framed_and_interim(void) {
    srv_t g; pthread_t th;
    if (srv_start(&g, SRV_EOF_FRAMED, &th) == 0) {
        char url[128]; url_for(url, sizeof(url), g.port, "/e");
        sw_http_native_req_t rq = { .url = url, .interrupt_fd = -1 };
        sw_http_native_resp_t rs;
        int st = sw_http_native_request(&rq, &rs);
        srv_finish(&g, th);
        CHECK(st == 200 && rs.body && strcmp(rs.body, "hello-eof") == 0,
              "no framing: body runs to the close");
        sw_http_native_resp_free(&rs);
    } else {
        CHECK(0, "no framing: server");
    }
    if (srv_start(&g, SRV_INTERIM, &th) == 0) {
        char url[128]; url_for(url, sizeof(url), g.port, "/i");
        sw_http_native_req_t rq = { .url = url, .interrupt_fd = -1 };
        sw_http_native_resp_t rs;
        int st = sw_http_native_request(&rq, &rs);
        srv_finish(&g, th);
        CHECK(st == 200 && rs.body && strcmp(rs.body, "ok") == 0,
              "100 Continue is skipped; the real reply is used");
        sw_http_native_resp_free(&rs);
    } else {
        CHECK(0, "interim: server");
    }
}

static void test_request_validation(void) {
    setenv("SW_HTTP_NATIVE", "1", 1);
    unsetenv("http_proxy");
    unsetenv("HTTP_PROXY");
    unsetenv("all_proxy");
    unsetenv("ALL_PROXY");
    CHECK(sw_http_native_accepts("http://127.0.0.1:1/") == 1, "accepts: plain http:// with gate on");
    CHECK(sw_http_native_accepts("https://127.0.0.1/") == 0, "accepts: https:// stays on curl");
    CHECK(sw_http_native_accepts("http://u:p@127.0.0.1/") == 0, "accepts: userinfo stays on curl");
    CHECK(sw_http_native_accepts("http://h x/") == 0, "accepts: space in URL stays on curl");
    setenv("http_proxy", "http://proxy.invalid:3128", 1);
    CHECK(sw_http_native_accepts("http://127.0.0.1:1/") == 0, "accepts: a proxy env var stays on curl");
    unsetenv("http_proxy");
    unsetenv("SW_HTTP_NATIVE");
    CHECK(sw_http_native_accepts("http://127.0.0.1:1/") == 0, "accepts: gate off stays on curl");
    setenv("SW_HTTP_NATIVE", "1", 1);

    sw_http_native_resp_t rs;
    sw_http_native_req_t rq = { .url = "http://127.0.0.1:1/", .interrupt_fd = -1 };

    static const char *inj[] = { "X-A: b\r\nInjected: 1" };
    rq.headers = inj; rq.nheaders = 1;
    CHECK(sw_http_native_request(&rq, &rs) == 0 && strstr(rs.err, "invalid") != NULL,
          "header with CR/LF is refused, nothing sent");
    sw_http_native_resp_free(&rs);

    static const char *te[] = { "Transfer-Encoding: chunked" };
    rq.headers = te; rq.nheaders = 1;
    CHECK(sw_http_native_request(&rq, &rs) == 0 && strstr(rs.err, "Transfer-Encoding") != NULL,
          "caller Transfer-Encoding is refused");
    sw_http_native_resp_free(&rs);

    rq.headers = NULL; rq.nheaders = 0;
    rq.method = "GET /x HTTP/1.0";
    CHECK(sw_http_native_request(&rq, &rs) == 0 && strstr(rs.err, "method") != NULL,
          "method with a space is refused");
    sw_http_native_resp_free(&rs);

    rq.method = NULL;
    rq.url = "https://127.0.0.1:1/";
    CHECK(sw_http_native_request(&rq, &rs) == 0 && rs.status == 0,
          "https:// is refused by the client itself");
    sw_http_native_resp_free(&rs);
}

static void test_fuzz_hook(void) {
    const char *good = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n";
    CHECK(sw_http_native_fuzz_parse((const uint8_t *)good, strlen(good)) == 200,
          "fuzz hook: well-formed chunked reply parses");
    const char *short_cl = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nab";
    CHECK(sw_http_native_fuzz_parse((const uint8_t *)short_cl, strlen(short_cl)) == 0,
          "fuzz hook: truncated Content-Length body is an error");
    const char *junk = "\x00\xff garbage\r\n\r\n";
    CHECK(sw_http_native_fuzz_parse((const uint8_t *)junk, strlen(junk)) == 0,
          "fuzz hook: non-HTTP bytes are an error");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_request_validation();
    test_chunked(SRV_CHUNKED, "chunked: whole reply in one write");
    test_chunked(SRV_CHUNKED_SLOW, "chunked: reply arriving one byte at a time");
    test_big_both_ways();
    test_midclose();
    test_timeout();
    test_interrupt();
    test_cancel();
    test_eof_framed_and_interim();
    test_fuzz_hook();

    if (g_fail == 0) printf("OK test_http_native %d/%d\n", g_pass, g_pass);
    else printf("FAIL test_http_native %d failed\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
