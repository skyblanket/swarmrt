/*
 * SwarmRT LiveView: HTTP/WebSocket Server
 *
 * Minimal HTTP/1.1 server with WebSocket upgrade support.
 * Uses the existing kqueue IO system (swarmrt_io.c).
 * Bridge process translates port events → sw_val_t tuples for .sw handlers.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_HTTP_H
#define SWARMRT_HTTP_H

#include "swarmrt_native.h"
#include "swarmrt_io.h"
#include "swarmrt_lang.h"

/* Maximum concurrent HTTP/WS connections */
#define SW_HTTP_MAX_CONNS 256

/* Maximum bytes buffered for a single HTTP request (request line + headers +
 * body) AND maximum accepted Content-Length. Without this one client could
 * stream bytes forever and make the server buffer up to ~4GB (the old
 * `realloc((buf_len+len+1)*2)` growth had no ceiling). Override with the
 * SW_HTTP_MAX_REQUEST env var (bytes, min 4096; parsed once, on first use).
 * Chosen above the 16MB WebSocket frame/reassembly cap so WS traffic is
 * never the binding constraint. Oversized declared bodies get a 413;
 * a buffer that outgrows the cap without a parseable request gets the
 * connection closed. NOTE: idle/slow-loris TIMEOUTS are a separate,
 * still-open TODO (needs timer integration in the bridge loop). */
#define SW_HTTP_MAX_REQUEST_DEFAULT (32u * 1024u * 1024u)

/* Connection modes */
typedef enum {
    SW_HTTP_MODE_HTTP,      /* Awaiting HTTP request */
    SW_HTTP_MODE_WS,        /* WebSocket upgraded */
} sw_http_mode_t;

/* Per-connection state */
typedef struct {
    int                 id;         /* Connection handle (index in table) */
    sw_port_t          *port;       /* Underlying TCP port */
    sw_http_mode_t      mode;
    sw_process_t       *handler;    /* .sw process receiving messages */

    /* Receive buffer for partial data */
    uint8_t            *buf;
    uint32_t            buf_len;
    uint32_t            buf_cap;

    /* HTTP body buffering */
    uint32_t            content_length; /* Content-Length from headers, 0 = none */
    int                 body_pending;   /* 1 = still accumulating body bytes */

    /* Connection persistence */
    int                 keep_alive;     /* 1 = keep connection open after response */

    /* WebSocket state */
    char                ws_key[128]; /* Sec-WebSocket-Key from upgrade */
    int                 active;      /* 1 = in use, 0 = free slot */

    /* Parsed request headers, delivered to the handler.
     *  - For plain HTTP: built per request, handed off (by reference) inside
     *    the {'http_request', ...} tuple, then cleared. When a POST body has
     *    to be buffered across reads, this map is held here until the body
     *    completes and the request is dispatched.
     *  - For a WebSocket upgrade: kept for the lifetime of the connection so
     *    the handler can query it via ws_request_headers(conn) / the path via
     *    ws_request_path(conn) (the Origin/Authorization/signature headers the
     *    UPGRADE request carried).
     * NULL = no headers captured (yet). Owned by the connection; freed on
     * dispatch (HTTP) or conn_free (WS). */
    sw_val_t           *req_headers;
    char               *req_path;    /* request/upgrade path+query (WS); NULL = none */

    /* WebSocket fragmentation reassembly (continuation frames, opcode 0x0).
     * frag_buf accumulates payload across FIN=0 frames; frag_opcode holds
     * the opcode of the first (0x1 text / 0x2 binary) frame in the run. */
    uint8_t            *frag_buf;
    uint32_t            frag_len;
    uint32_t            frag_cap;
    int                 frag_opcode; /* 0 = not reassembling */
} sw_http_conn_t;

/* Bridge state (passed to bridge process) */
typedef struct {
    sw_port_t          *listener;
    sw_process_t       *handler;    /* Default handler (.sw process) */
    uint16_t            port;
} sw_http_bridge_t;

/* === API (called from builtins) === */

/* Start HTTP server on port, handler = calling process */
sw_process_t *sw_http_listen(uint16_t port, sw_process_t *handler);

/* Send HTTP response (text body) */
int sw_http_respond(int conn_id, int status, const char *headers, const char *body);

/* Send HTTP response with raw binary body (for file serving) */
int sw_http_respond_raw(int conn_id, int status, const char *headers, const void *data, uint32_t data_len);

/* Send WebSocket text frame */
int sw_ws_send_text(int conn_id, const char *data, uint32_t len);

/* Send WebSocket binary frame (opcode 0x2) */
int sw_ws_send_binary(int conn_id, const char *data, uint32_t len);

/* Close WebSocket connection */
int sw_ws_close(int conn_id);

/* Set WebSocket handler process */
int sw_ws_set_handler(int conn_id, sw_process_t *handler);

/* Request headers (MAP, lowercased keys) captured from a WS connection's
 * UPGRADE request — for Origin/Authorization/signature reads on the socket.
 * Always returns a non-NULL map (empty if none captured). */
sw_val_t *sw_ws_request_headers(int conn_id);

/* Request path+query from a WS connection's UPGRADE request (""=unknown). */
const char *sw_ws_request_path(int conn_id);

/* Get embedded LiveView JavaScript */
const char *sw_liveview_js(void);

#endif /* SWARMRT_HTTP_H */
