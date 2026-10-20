/*
 * SwarmRT native HTTP/1.1 client — see swarmrt_http_native.h.
 *
 * Shape of a request:
 *   sw_http_native_request
 *     native_parse_url        http:// only, no userinfo, host/port/target checked
 *     native_build_request    request line + headers (CR/LF in a header is refused)
 *     native_connect          getaddrinfo, non-blocking connect under one deadline
 *     native_send_all         request head, then the body straight from memory
 *     native_read_response    status line, headers, then Content-Length / chunked /
 *                             read-to-EOF framing, through a small read buffer
 *
 * Every wait goes through sock_wait, which polls the socket against the
 * whole-request deadline and, when set, the caller's interrupt fd. So a hung
 * server returns at its deadline and a keypress aborts at once, with no
 * helper thread.
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
#include "swarmrt_http_native.h"

#ifdef _WIN32
/* Plain-socket client is POSIX-only; Windows keeps using curl. */
int sw_http_native_enabled(void) { return 0; }
int sw_http_native_accepts(const char *url) { (void)url; return 0; }
int sw_http_native_request(const sw_http_native_req_t *req, sw_http_native_resp_t *resp) {
    (void)req;
    memset(resp, 0, sizeof(*resp));
    snprintf(resp->err, sizeof(resp->err), "native http client is not available on Windows");
    return 0;
}
void sw_http_native_resp_free(sw_http_native_resp_t *resp) {
    if (!resp) return;
    free(resp->headers);
    free(resp->body);
    resp->headers = NULL;
    resp->body = NULL;
}
int sw_http_native_fuzz_parse(const uint8_t *buf, size_t len) { (void)buf; (void)len; return 0; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "swarmrt_platform.h"

/* === Limits ===
 * Response headers and any single body (after de-chunking) are capped so a
 * hostile or broken server cannot make the client buffer without bound. */
#define NATIVE_MAX_HDR        (64u * 1024u)
#define NATIVE_MAX_BODY       (256u * 1024u * 1024u)
#define NATIVE_MAX_LINE       1024u
#define NATIVE_MAX_TRAILERS   64u
#define NATIVE_READ_CHUNK     (64u * 1024u)
#define NATIVE_BODY_CHUNK     (64u * 1024u)

/* Source results below 0 (any negative return of native_src_t.recv). */
#define SRC_ERR      (-1)
#define SRC_INTR     (-2)
#define SRC_TIMEOUT  (-3)

#ifdef MSG_NOSIGNAL
#define NATIVE_SEND_FLAGS MSG_NOSIGNAL
#else
#define NATIVE_SEND_FLAGS 0   /* Darwin: SO_NOSIGPIPE is set per socket instead */
#endif

/* === Byte source ===
 * The response parser reads through this, so the socket path and the fuzz
 * hook (memory) share one parser. recv returns >0 bytes, 0 for EOF, or a SRC_*
 * code. */
typedef struct {
    long (*recv)(void *ctx, char *dst, size_t cap);
    void *ctx;
} native_src_t;

static uint64_t native_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* === Gate ===================================================== */

int sw_http_native_enabled(void) {
    const char *e = getenv("SW_HTTP_NATIVE");
    return e && strcmp(e, "1") == 0;
}

/* === URL ====================================================== */

typedef struct {
    char host[256];
    char port[8];
    char target[4096];
    int  v6;
} native_url_t;

static int native_parse_url(const char *url, native_url_t *u) {
    static const char pfx[] = "http://";
    if (!url || strncasecmp(url, pfx, sizeof(pfx) - 1) != 0) return 0;
    const char *p = url + (sizeof(pfx) - 1);
    size_t alen = strcspn(p, "/?#");
    const char *auth_end = p + alen;
    if (alen == 0 || memchr(p, '@', alen)) return 0;   /* userinfo: let curl handle it */

    const char *hs = p, *he, *ps = NULL, *pe = auth_end;
    u->v6 = 0;
    if (p[0] == '[') {
        const char *rb = memchr(p, ']', alen);
        if (!rb || rb == p + 1) return 0;
        hs = p + 1; he = rb; u->v6 = 1;
        if (rb + 1 < auth_end) {
            if (rb[1] != ':') return 0;
            ps = rb + 2;
        }
    } else {
        const char *colon = memchr(p, ':', alen);
        he = colon ? colon : auth_end;
        if (colon) ps = colon + 1;
    }

    size_t hl = (size_t)(he - hs);
    if (hl == 0 || hl >= sizeof(u->host)) return 0;
    for (size_t i = 0; i < hl; i++) {
        unsigned char c = (unsigned char)hs[i];
        int ok = isalnum(c) || c == '.' || c == '-' || c == '_';
        if (u->v6) ok = ok || c == ':' || c == '%';
        if (!ok) return 0;
    }
    memcpy(u->host, hs, hl);
    u->host[hl] = '\0';

    if (ps) {
        if (ps >= pe) return 0;
        unsigned long v = 0;
        for (const char *q = ps; q < pe; q++) {
            if (*q < '0' || *q > '9') return 0;
            v = v * 10 + (unsigned long)(*q - '0');
            if (v > 65535) return 0;
        }
        if (v == 0) return 0;
        snprintf(u->port, sizeof(u->port), "%lu", v);
    } else {
        snprintf(u->port, sizeof(u->port), "80");
    }

    /* Target is the path + query; a fragment is never sent. */
    const char *ts = auth_end;
    size_t tl = strcspn(ts, "#");
    if (tl == 0) {
        snprintf(u->target, sizeof(u->target), "/");
    } else {
        if (tl >= sizeof(u->target)) return 0;
        for (size_t i = 0; i < tl; i++) {
            unsigned char c = (unsigned char)ts[i];
            if (c <= 0x20 || c == 0x7f) return 0;   /* spaces/controls: curl's job */
        }
        memcpy(u->target, ts, tl);
        u->target[tl] = '\0';
    }
    return 1;
}

int sw_http_native_accepts(const char *url) {
    if (!sw_http_native_enabled()) return 0;
    native_url_t u;
    if (!native_parse_url(url, &u)) return 0;
    /* curl honours these; this client does not, so leave them to curl. */
    static const char *const proxy_vars[] = {
        "http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY"
    };
    for (size_t i = 0; i < sizeof(proxy_vars) / sizeof(proxy_vars[0]); i++) {
        const char *v = getenv(proxy_vars[i]);
        if (v && *v) return 0;
    }
    return 1;
}

/* === Request building ========================================= */

typedef struct {
    char *p;
    size_t len, cap;
    int oom;
} sbuf_t;

static void sb_put(sbuf_t *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 512;
        while (b->len + n + 1 > nc) nc *= 2;
        char *np = (char *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_puts(sbuf_t *b, const char *s) { sb_put(b, s, strlen(s)); }

/* RFC 7230 token character (method and header-name grammar). */
static int native_is_tchar(unsigned char c) {
    if (isalnum(c)) return 1;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int native_valid_token(const char *s) {
    if (!*s) return 0;
    for (const char *q = s; *q; q++)
        if (!native_is_tchar((unsigned char)*q)) return 0;
    return 1;
}

/* Does the header line `line` (length `len`) carry this name, ignoring case?
 * The name ends at the first colon. */
static int native_hdr_is(const char *line, size_t len, const char *name) {
    const char *colon = memchr(line, ':', len);
    if (!colon) return 0;
    size_t nl = strlen(name);
    if ((size_t)(colon - line) != nl) return 0;
    return strncasecmp(line, name, nl) == 0;
}

/* The request head: request line, the headers the caller gave, and the
 * framing headers this client owns. Connection is always close (one request
 * per connection). A caller's Host or Content-Length replaces ours; a
 * Transfer-Encoding header is refused (bodies go out with Content-Length). */
static char *native_build_request(const sw_http_native_req_t *req, const char *method,
                                  const native_url_t *u, size_t *out_len,
                                  char *err, size_t errcap) {
    int has_host = 0, has_cl = 0, has_accept = 0, has_ua = 0;
    for (int i = 0; i < req->nheaders; i++) {
        const char *h = req->headers[i];
        const char *colon = h ? strchr(h, ':') : NULL;
        if (!colon || colon == h) {
            snprintf(err, errcap, "invalid request header");
            return NULL;
        }
        for (const char *q = h; q < colon; q++) {
            if (!native_is_tchar((unsigned char)*q)) {
                snprintf(err, errcap, "invalid request header name");
                return NULL;
            }
        }
        for (const char *q = colon + 1; *q; q++) {
            unsigned char c = (unsigned char)*q;
            if ((c < 0x20 && c != '\t') || c == 0x7f) {
                snprintf(err, errcap, "invalid request header value");
                return NULL;
            }
        }
        size_t hl = strlen(h);
        if (native_hdr_is(h, hl, "transfer-encoding")) {
            snprintf(err, errcap, "Transfer-Encoding request headers are not supported");
            return NULL;
        }
        if (native_hdr_is(h, hl, "host")) has_host = 1;
        else if (native_hdr_is(h, hl, "content-length")) has_cl = 1;
        else if (native_hdr_is(h, hl, "accept")) has_accept = 1;
        else if (native_hdr_is(h, hl, "user-agent")) has_ua = 1;
    }

    sbuf_t b = {0};
    char tmp[64];
    sb_puts(&b, method);
    sb_puts(&b, " ");
    sb_puts(&b, u->target);
    sb_puts(&b, " HTTP/1.1\r\n");
    if (!has_host) {
        sb_puts(&b, "Host: ");
        if (u->v6) { sb_puts(&b, "["); sb_puts(&b, u->host); sb_puts(&b, "]"); }
        else sb_puts(&b, u->host);
        if (strcmp(u->port, "80") != 0) { sb_puts(&b, ":"); sb_puts(&b, u->port); }
        sb_puts(&b, "\r\n");
    }
    sb_puts(&b, "Connection: close\r\n");
    if (!has_accept) sb_puts(&b, "Accept: */*\r\n");
    if (!has_ua) sb_puts(&b, "User-Agent: swarmrt-http/1\r\n");
    for (int i = 0; i < req->nheaders; i++) {
        const char *h = req->headers[i];
        if (native_hdr_is(h, strlen(h), "connection")) continue;   /* ours: always close */
        sb_puts(&b, h);
        sb_puts(&b, "\r\n");
    }
    if (req->body && !has_cl) {
        snprintf(tmp, sizeof(tmp), "Content-Length: %zu\r\n", req->body_len);
        sb_puts(&b, tmp);
    }
    sb_puts(&b, "\r\n");
    if (b.oom) {
        free(b.p);
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    *out_len = b.len;
    return b.p;
}

/* === Socket ==================================================== */

typedef struct {
    int fd;
    uint64_t deadline;               /* monotonic ms; 0 = none */
    const sw_http_native_req_t *req;
} native_sock_t;

static void native_tune_socket(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

/* Longest a poll may sleep when a cancel hook is set, so a dead caller is
 * noticed within this bound even when the socket stays quiet. */
#define NATIVE_CANCEL_POLL_MS 50

/* Wait until the socket is ready for `events`. Returns 1 when ready (the
 * following recv/send reports any error), SRC_TIMEOUT at the deadline,
 * SRC_INTR when the interrupt callback or the cancel hook aborts, SRC_ERR on
 * a poll failure. */
static long sock_wait(native_sock_t *s, short events, uint64_t deadline) {
    int watch_intr = s->req->on_interrupt && s->req->interrupt_fd >= 0;
    for (;;) {
        if (s->req->cancelled && s->req->cancelled(s->req->cancel_ctx)) return SRC_INTR;
        struct pollfd pf[2];
        nfds_t n = 0;
        pf[n].fd = s->fd;
        pf[n].events = events;
        pf[n].revents = 0;
        n++;
        if (watch_intr) {
            pf[n].fd = s->req->interrupt_fd;
            pf[n].events = POLLIN;
            pf[n].revents = 0;
            n++;
        }
        int tmo = -1;
        if (deadline) {
            uint64_t now = native_now_ms();
            if (now >= deadline) return SRC_TIMEOUT;
            uint64_t left = deadline - now;
            tmo = left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
        }
        if (s->req->cancelled && (tmo < 0 || tmo > NATIVE_CANCEL_POLL_MS)) tmo = NATIVE_CANCEL_POLL_MS;
        int rc = poll(pf, n, tmo);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return SRC_ERR;
        }
        if (rc == 0) continue;   /* deadline and cancel re-checked at the top */
        if (watch_intr && pf[1].revents) {
            int r = s->req->on_interrupt(s->req->ctx);
            if (r > 0) return SRC_INTR;
            if (r < 0) watch_intr = 0;   /* e.g. EOF on the fd: stop polling it */
        }
        if (pf[0].revents) return 1;
    }
}

static long sock_recv(void *ctx, char *dst, size_t cap) {
    native_sock_t *s = (native_sock_t *)ctx;
    for (;;) {
        long w = sock_wait(s, POLLIN, s->deadline);
        if (w != 1) return w;
        ssize_t n = recv(s->fd, dst, cap, 0);
        if (n >= 0) return (long)n;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return SRC_ERR;
    }
}

static long native_send_all(native_sock_t *s, const char *p, size_t n) {
    while (n > 0) {
        long w = sock_wait(s, POLLOUT, s->deadline);
        if (w != 1) return w;
        ssize_t k = send(s->fd, p, n, NATIVE_SEND_FLAGS);
        if (k < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return SRC_ERR;
        }
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static void native_set_io_error(sw_http_native_resp_t *resp, long code, const char *what) {
    if (code == SRC_INTR) {
        resp->interrupted = 1;
        snprintf(resp->err, sizeof(resp->err), "interrupted");
    } else if (code == SRC_TIMEOUT) {
        snprintf(resp->err, sizeof(resp->err), "%s timed out", what);
    } else {
        snprintf(resp->err, sizeof(resp->err), "%s failed", what);
    }
}

/* Resolve host and connect to the first address that accepts. The connect
 * waits are bounded by connect_deadline (which never exceeds the request
 * deadline). Leaves the connected, non-blocking fd in s->fd. */
static int native_connect(native_sock_t *s, const native_url_t *u, uint64_t connect_deadline,
                          sw_http_native_resp_t *resp) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gx = getaddrinfo(u->host, u->port, &hints, &res);
    if (gx != 0) {
        snprintf(resp->err, sizeof(resp->err), "resolve %s failed: %s", u->host, gai_strerror(gx));
        return -1;
    }

    int last_errno = 0;
    long last_wait = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        int fd = sw_socket_cloexec(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last_errno = errno; continue; }
        native_tune_socket(fd);

        int ok = 0;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            ok = 1;
        } else if (errno == EINPROGRESS) {
            s->fd = fd;
            long w = sock_wait(s, POLLOUT, connect_deadline);
            if (w == 1) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
                    ok = 1;
                } else {
                    last_errno = soerr ? soerr : errno;
                }
            } else {
                last_wait = w;
            }
        } else {
            last_errno = errno;
        }

        if (ok) {
            freeaddrinfo(res);
            s->fd = fd;
            return 0;
        }
        close(fd);
        s->fd = -1;
        if (last_wait) break;   /* interrupt or deadline: do not try other addresses */
    }
    freeaddrinfo(res);
    if (last_wait) {
        native_set_io_error(resp, last_wait, "connect");
    } else {
        snprintf(resp->err, sizeof(resp->err), "connect to %s:%s failed: %s", u->host, u->port,
                 last_errno ? strerror(last_errno) : "no usable address");
    }
    return -1;
}

/* === Buffered reader over a native_src_t ======================= */

typedef struct {
    const native_src_t *src;
    sw_http_native_resp_t *resp;     /* where failure text goes */
    char *buf;                       /* unread bytes are buf[pos..len) */
    size_t cap, pos, len;
    int eof;
} native_rd_t;

static int rd_fail(native_rd_t *rd, const char *msg) {
    snprintf(rd->resp->err, sizeof(rd->resp->err), "%s", msg);
    return -1;
}

/* Pull more bytes into the buffer. 1 = bytes added, 0 = clean EOF, -1 =
 * failure (resp->err set; resp->interrupted set on an interrupt). Compacts
 * first, so pos is always 0 after a fill. */
static long rd_fill(native_rd_t *rd) {
    if (rd->eof) return 0;
    if (rd->pos > 0) {
        memmove(rd->buf, rd->buf + rd->pos, rd->len - rd->pos);
        rd->len -= rd->pos;
        rd->pos = 0;
    }
    if (rd->len == rd->cap) {
        char *nb = (char *)realloc(rd->buf, rd->cap * 2);
        if (!nb) return rd_fail(rd, "out of memory");
        rd->buf = nb;
        rd->cap *= 2;
    }
    long n = rd->src->recv(rd->src->ctx, rd->buf + rd->len, rd->cap - rd->len);
    if (n == 0) { rd->eof = 1; return 0; }
    if (n == SRC_INTR) {
        rd->resp->interrupted = 1;
        return rd_fail(rd, "interrupted");
    }
    if (n == SRC_TIMEOUT) return rd_fail(rd, "timed out waiting for the response");
    if (n < 0) return rd_fail(rd, "socket read failed");
    rd->len += (size_t)n;
    return 1;
}

/* Append n body bytes; enforces NATIVE_MAX_BODY. */
static int body_put(native_rd_t *rd, size_t *cap, const char *p, size_t n) {
    sw_http_native_resp_t *resp = rd->resp;
    if (n == 0) return 0;
    if (n > NATIVE_MAX_BODY - resp->body_len) return rd_fail(rd, "response body too large");
    size_t need = resp->body_len + n;
    if (need + 1 > *cap) {
        size_t nc = *cap ? *cap : NATIVE_BODY_CHUNK;
        while (need + 1 > nc) nc *= 2;
        char *nb = (char *)realloc(resp->body, nc);
        if (!nb) return rd_fail(rd, "out of memory");
        resp->body = nb;
        *cap = nc;
    }
    memcpy(resp->body + resp->body_len, p, n);
    resp->body_len = need;
    return 0;
}

/* One header block: everything through the blank line, copied into a
 * NUL-terminated string. Accepts CRLF or bare LF line ends. */
static int rd_header_block(native_rd_t *rd, char **out) {
    size_t scanned = 0;
    for (;;) {
        size_t unread = rd->len - rd->pos;
        const char *p = rd->buf + rd->pos;
        size_t blk = 0;
        for (size_t i = scanned; i < unread && !blk; i++) {
            if (p[i] != '\n') continue;
            if (i + 1 < unread && p[i + 1] == '\n') blk = i + 2;
            else if (i + 2 < unread && p[i + 1] == '\r' && p[i + 2] == '\n') blk = i + 3;
        }
        if (blk) {
            char *s = (char *)malloc(blk + 1);
            if (!s) return rd_fail(rd, "out of memory");
            memcpy(s, p, blk);
            s[blk] = '\0';
            rd->pos += blk;
            *out = s;
            return 0;
        }
        if (unread > NATIVE_MAX_HDR) return rd_fail(rd, "response headers too large");
        /* A terminator can straddle the end of what we have; rescan the tail. */
        scanned = unread > 3 ? unread - 3 : 0;
        long f = rd_fill(rd);
        if (f == 0) return rd_fail(rd, "connection closed before the response headers ended");
        if (f < 0) return -1;
    }
}

/* One line (CR stripped) copied into out (cap bytes incl. NUL). Returns its
 * length, or -1 with resp->err set. */
static long rd_line(native_rd_t *rd, char *out, size_t cap) {
    size_t scanned = 0;
    for (;;) {
        size_t unread = rd->len - rd->pos;
        const char *p = rd->buf + rd->pos;
        for (size_t i = scanned; i < unread; i++) {
            if (p[i] != '\n') continue;
            size_t L = i;
            if (L && p[L - 1] == '\r') L--;
            if (L + 1 > cap) { rd_fail(rd, "chunk framing line too long"); return -1; }
            memcpy(out, p, L);
            out[L] = '\0';
            rd->pos += i + 1;
            return (long)L;
        }
        if (unread > NATIVE_MAX_LINE) { rd_fail(rd, "chunk framing line too long"); return -1; }
        scanned = unread;
        long f = rd_fill(rd);
        if (f == 0) { rd_fail(rd, "connection closed inside a chunked body"); return -1; }
        if (f < 0) return -1;
    }
}

/* Exactly n body bytes, moved from the buffer (or read more) into the body. */
static int rd_copy_exact(native_rd_t *rd, size_t *cap, size_t n) {
    while (n > 0) {
        size_t avail = rd->len - rd->pos;
        if (avail == 0) {
            long f = rd_fill(rd);
            if (f == 0) return rd_fail(rd, "connection closed before the full body arrived");
            if (f < 0) return -1;
            continue;
        }
        size_t k = avail < n ? avail : n;
        if (body_put(rd, cap, rd->buf + rd->pos, k) < 0) return -1;
        rd->pos += k;
        n -= k;
    }
    return 0;
}

/* === Response parsing ========================================== */

/* Next header line in a block (CR stripped). NULL at the NUL. *len is its length. */
static const char *hdr_line(const char **cur, size_t *len) {
    const char *s = *cur;
    if (!s || !*s) return NULL;
    const char *nl = strchr(s, '\n');
    size_t L = nl ? (size_t)(nl - s) : strlen(s);
    *cur = nl ? nl + 1 : s + L;
    if (L && s[L - 1] == '\r') L--;
    *len = L;
    return s;
}

/* Status code from "HTTP/x.y DDD ..." or -1. Never reads past the NUL. */
static int native_status_code(const char *hdr) {
    if (strncmp(hdr, "HTTP/", 5) != 0) return -1;
    const char *sp = strchr(hdr, ' ');
    if (!sp) return -1;
    int v = 0;
    for (int i = 1; i <= 3; i++) {
        char c = sp[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    char after = sp[4];
    if (after != '\0' && after != ' ' && after != '\r' && after != '\n') return -1;
    return v;
}

/* Value of a header line (leading blanks skipped, trailing blanks trimmed). */
static const char *hdr_value(const char *line, size_t len, size_t *vlen) {
    const char *colon = memchr(line, ':', len);
    if (!colon) { *vlen = 0; return line + len; }
    const char *v = colon + 1;
    const char *end = line + len;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *vlen = (size_t)(end - v);
    return v;
}

static int value_has_token_ci(const char *v, size_t vl, const char *tok) {
    size_t tl = strlen(tok);
    for (size_t i = 0; i + tl <= vl; i++)
        if (strncasecmp(v + i, tok, tl) == 0) return 1;
    return 0;
}

/* Framing from the header block: Content-Length (or -1), Transfer-Encoding
 * chunked. Returns -1 on a malformed or conflicting Content-Length. */
static int native_framing(const char *hdr, long long *cl, int *chunked) {
    *cl = -1;
    *chunked = 0;
    const char *cur = hdr;
    size_t len;
    hdr_line(&cur, &len);   /* status line */
    const char *line;
    while ((line = hdr_line(&cur, &len)) != NULL && len > 0) {
        if (native_hdr_is(line, len, "content-length")) {
            size_t vl;
            const char *v = hdr_value(line, len, &vl);
            if (vl == 0) return -1;
            long long n = 0;
            for (size_t i = 0; i < vl; i++) {
                if (v[i] < '0' || v[i] > '9') return -1;
                n = n * 10 + (v[i] - '0');
                if (n > (long long)NATIVE_MAX_BODY) return -1;
            }
            if (*cl >= 0 && *cl != n) return -1;
            *cl = n;
        } else if (native_hdr_is(line, len, "transfer-encoding")) {
            size_t vl;
            const char *v = hdr_value(line, len, &vl);
            if (value_has_token_ci(v, vl, "chunked")) *chunked = 1;
        }
    }
    return 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

#define NATIVE_MAX_CHUNK_SIZE_DIGITS 7   /* keeps the size under 2^28 before the body cap */

static int native_read_chunked(native_rd_t *rd, size_t *cap) {
    char line[NATIVE_MAX_LINE + 1];
    for (;;) {
        long n = rd_line(rd, line, sizeof(line));
        if (n < 0) return -1;
        /* chunk-size [; extension] */
        unsigned long long sz = 0;
        int digits = 0;
        for (long i = 0; i < n && line[i] != ';' && line[i] != ' ' && line[i] != '\t'; i++) {
            int d = hexval(line[i]);
            if (d < 0 || ++digits > NATIVE_MAX_CHUNK_SIZE_DIGITS) return rd_fail(rd, "bad chunk size");
            sz = sz * 16 + (unsigned long long)d;
        }
        if (digits == 0) return rd_fail(rd, "bad chunk size");
        if (sz == 0) {
            /* last chunk: optional trailer lines, then a blank line */
            for (unsigned t = 0;; t++) {
                n = rd_line(rd, line, sizeof(line));
                if (n < 0) return -1;
                if (n == 0) return 0;
                if (t >= NATIVE_MAX_TRAILERS) return rd_fail(rd, "too many chunk trailer lines");
            }
        }
        if (rd_copy_exact(rd, cap, (size_t)sz) < 0) return -1;
        n = rd_line(rd, line, sizeof(line));   /* CRLF closing the chunk data */
        if (n < 0) return -1;
        if (n != 0) return rd_fail(rd, "bad chunk framing");
    }
}

static int native_read_response_rd(native_rd_t *rd, int head_only, sw_http_native_resp_t *resp) {
    char *hdr = NULL;
    int status;
    for (;;) {
        if (rd_header_block(rd, &hdr) < 0) return -1;
        status = native_status_code(hdr);
        if (status < 0) { free(hdr); return rd_fail(rd, "malformed status line"); }
        /* 1xx interim replies (100 Continue, ...) are not the response. */
        if (status >= 100 && status < 200 && status != 101) { free(hdr); hdr = NULL; continue; }
        break;
    }

    long long cl;
    int chunked;
    if (native_framing(hdr, &cl, &chunked) < 0) {
        free(hdr);
        return rd_fail(rd, "malformed Content-Length");
    }
    resp->status = status;
    resp->headers = hdr;

    size_t cap = 0;
    if (head_only || status == 204 || status == 304) {
        /* no body on the wire */
    } else if (chunked) {
        if (native_read_chunked(rd, &cap) < 0) return -1;
    } else if (cl >= 0) {
        if (rd_copy_exact(rd, &cap, (size_t)cl) < 0) return -1;
    } else {
        /* no framing: the body runs to the server's close */
        for (;;) {
            size_t avail = rd->len - rd->pos;
            if (avail) {
                if (body_put(rd, &cap, rd->buf + rd->pos, avail) < 0) return -1;
                rd->pos += avail;
            }
            long f = rd_fill(rd);
            if (f == 0) break;
            if (f < 0) return -1;
        }
    }

    /* NUL-terminate the body (an empty body still gets a string). */
    char *nb = (char *)realloc(resp->body, resp->body_len + 1);
    if (!nb) return rd_fail(rd, "out of memory");
    resp->body = nb;
    resp->body[resp->body_len] = '\0';
    return 0;
}

/* Parse one response from src into resp. On failure status is 0 and the
 * headers and body are released, so the caller sees only the error. */
static int native_read_response(const native_src_t *src, int head_only, sw_http_native_resp_t *resp) {
    native_rd_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.src = src;
    rd.resp = resp;
    rd.cap = NATIVE_READ_CHUNK;
    rd.buf = (char *)malloc(rd.cap);
    int rc = -1;
    if (!rd.buf) {
        rd_fail(&rd, "out of memory");
    } else {
        rc = native_read_response_rd(&rd, head_only, resp);
    }
    free(rd.buf);
    if (rc < 0) {
        resp->status = 0;
        free(resp->headers);
        free(resp->body);
        resp->headers = NULL;
        resp->body = NULL;
        resp->body_len = 0;
    }
    return rc;
}

/* === Public API ================================================ */

void sw_http_native_resp_free(sw_http_native_resp_t *resp) {
    if (!resp) return;
    free(resp->headers);
    free(resp->body);
    resp->headers = NULL;
    resp->body = NULL;
    resp->body_len = 0;
}

int sw_http_native_request(const sw_http_native_req_t *req, sw_http_native_resp_t *resp) {
    memset(resp, 0, sizeof(*resp));
    if (!req || !req->url) {
        snprintf(resp->err, sizeof(resp->err), "missing url");
        return 0;
    }
    native_url_t u;
    if (!native_parse_url(req->url, &u)) {
        snprintf(resp->err, sizeof(resp->err), "unsupported url (plain http:// only)");
        return 0;
    }
    const char *method = (req->method && *req->method) ? req->method : "GET";
    if (!native_valid_token(method)) {
        snprintf(resp->err, sizeof(resp->err), "invalid HTTP method");
        return 0;
    }

    size_t head_len = 0;
    char *head = native_build_request(req, method, &u, &head_len, resp->err, sizeof(resp->err));
    if (!head) return 0;

    uint64_t budget = req->timeout_ms ? req->timeout_ms : SW_HTTP_NATIVE_TIMEOUT_MS_DEFAULT;
    uint64_t now = native_now_ms();
    native_sock_t s;
    s.fd = -1;
    s.deadline = now + budget;
    s.req = req;

    uint64_t connect_deadline = now + SW_HTTP_NATIVE_CONNECT_MS;
    if (connect_deadline > s.deadline) connect_deadline = s.deadline;

    if (native_connect(&s, &u, connect_deadline, resp) == 0) {
        long w = native_send_all(&s, head, head_len);
        if (w == 1 && req->body && req->body_len > 0)
            w = native_send_all(&s, req->body, req->body_len);
        if (w != 1) {
            native_set_io_error(resp, w, "send");
        } else {
            native_src_t src = { sock_recv, &s };
            native_read_response(&src, strcmp(method, "HEAD") == 0, resp);
        }
        close(s.fd);
    }
    free(head);
    return resp->status;
}

/* === Fuzz hook ================================================= */

typedef struct {
    const uint8_t *p;
    size_t n, off;
} native_mem_t;

static long mem_recv(void *ctx, char *dst, size_t cap) {
    native_mem_t *m = (native_mem_t *)ctx;
    size_t k = m->n - m->off;
    if (k > cap) k = cap;
    if (k == 0) return 0;
    memcpy(dst, m->p + m->off, k);
    m->off += k;
    return (long)k;
}

int sw_http_native_fuzz_parse(const uint8_t *buf, size_t len) {
    native_mem_t m = { buf, len, 0 };
    native_src_t src = { mem_recv, &m };
    sw_http_native_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    native_read_response(&src, 0, &resp);
    int status = resp.status;
    sw_http_native_resp_free(&resp);
    return status;
}

#endif /* _WIN32 */
