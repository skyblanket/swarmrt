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

/* Close WebSocket connection */
int sw_ws_close(int conn_id);

/* Set WebSocket handler process */
int sw_ws_set_handler(int conn_id, sw_process_t *handler);

/* Get embedded LiveView JavaScript */
const char *sw_liveview_js(void);

#endif /* SWARMRT_HTTP_H */
