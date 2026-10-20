/*
 * SwarmRT native HTTP/1.1 client (plain http:// only).
 *
 * A blocking-style client that runs on the offload pool and talks to the
 * server over a plain socket: no curl, no temp files, request bodies written
 * straight from memory. One connection per request, closed afterwards
 * (keep-alive is a later step). https:// is NOT handled here; callers fall
 * back to curl for it.
 *
 * Enabled only with SW_HTTP_NATIVE=1. sw_http_native_accepts() is the gate the
 * http_* builtins use: it is false (so curl runs as before) unless the env
 * var is set, the URL is http:// and parses, and no proxy variable is set
 * (curl honours those and this client does not).
 *
 * otonomy.ai
 */

#ifndef SWARMRT_HTTP_NATIVE_H
#define SWARMRT_HTTP_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* Default whole-request budget when timeout_ms is 0 (matches the curl path's
 * --max-time 300 for http_post/http_request; http_get passes 120000). */
#define SW_HTTP_NATIVE_TIMEOUT_MS_DEFAULT 300000u

/* Connect budget within the whole-request deadline (curl's --connect-timeout 30). */
#define SW_HTTP_NATIVE_CONNECT_MS 30000u

/* 1 when SW_HTTP_NATIVE=1 is set. Read on every call (cheap getenv). */
int sw_http_native_enabled(void);

/* 1 when the native client should serve `url`: enabled, an http:// URL that
 * parses, and no http(s)/all proxy environment variable set. */
int sw_http_native_accepts(const char *url);

typedef struct {
    const char *method;             /* "GET", "POST", ...; NULL = "GET" */
    const char *url;                /* http://host[:port][/path[?query]] */
    const char *const *headers;     /* "Name: value" lines, no CR/LF */
    int nheaders;
    const char *body;               /* NULL = no body (no Content-Length) */
    size_t body_len;
    uint64_t timeout_ms;            /* whole-request budget; 0 = default */

    /* Optional interrupt source (the ESC/Ctrl-C watcher on a raw-mode TTY).
     * When interrupt_fd >= 0 and on_interrupt is set, the client polls it
     * alongside the socket. On readable, on_interrupt(ctx) must consume the
     * bytes and return 1 to abort (resp->interrupted = 1), 0 to keep going,
     * or -1 to stop watching the fd (e.g. it hit EOF). Returning 0 without
     * consuming anything makes the poll loop spin. */
    int interrupt_fd;
    int (*on_interrupt)(void *ctx);
    void *ctx;

    /* Optional cancel check for a caller that can die while we wait (the
     * offload pool's parked process). Polled about every 50 ms; non-zero
     * aborts the request like an interrupt (resp->interrupted = 1). */
    int (*cancelled)(void *cancel_ctx);
    void *cancel_ctx;
} sw_http_native_req_t;

typedef struct {
    int status;                     /* HTTP status; 0 = no complete response */
    char *headers;                  /* raw header block, status line first, NUL-terminated */
    char *body;                     /* NUL-terminated; NULL when status == 0 */
    size_t body_len;
    int interrupted;                /* aborted by on_interrupt */
    char err[384];                  /* transport error text when status == 0 */
} sw_http_native_resp_t;

/* Perform one request and return resp->status. Always fills *resp: status 0
 * on any transport failure (bad URL/header, resolve/connect failure, timeout,
 * interrupt, or a reply cut short of its declared length), with err set.
 * Free with sw_http_native_resp_free. */
int sw_http_native_request(const sw_http_native_req_t *req, sw_http_native_resp_t *resp);
void sw_http_native_resp_free(sw_http_native_resp_t *resp);

/* Fuzz hook: parse a complete response from memory exactly as the socket
 * path would (status line, headers, Content-Length / chunked / read-to-EOF
 * framing). Returns the status (0 on a malformed or truncated reply). */
int sw_http_native_fuzz_parse(const uint8_t *buf, size_t len);

#endif /* SWARMRT_HTTP_NATIVE_H */
