/*
 * SwarmRT LiveView: HTTP/WebSocket Server
 *
 * Architecture:
 * - _builtin_http_listen → creates TCP listener + spawns bridge process
 * - Bridge process receives port events (accept, data, close)
 * - Translates raw TCP data → HTTP requests or WebSocket frames
 * - Sends sw_val_t tuples to .sw handler process
 * - Builtins (http_respond, ws_send, etc.) write directly to connections
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#define CC_SHA1(data, len, md) SHA1((const unsigned char *)(data), (unsigned long)(len), (md))
#define CC_SHA1_DIGEST_LENGTH 20
#define CC_LONG unsigned long
#endif
#include "swarmrt_http.h"
#include "swarmrt_liveview_js.h"

/* === Global Connection Table === */

static sw_http_conn_t g_http_conns[SW_HTTP_MAX_CONNS];
static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;

/* === Connection Management === */

static int conn_alloc(sw_port_t *port, sw_process_t *handler) {
    pthread_mutex_lock(&g_http_lock);
    for (int i = 0; i < SW_HTTP_MAX_CONNS; i++) {
        if (!g_http_conns[i].active) {
            sw_http_conn_t *c = &g_http_conns[i];
            c->id = i;
            c->port = port;
            c->mode = SW_HTTP_MODE_HTTP;
            c->handler = handler;
            c->active = 1;
            c->buf = (uint8_t *)malloc(4096);
            c->buf_len = 0;
            c->buf_cap = 4096;
            c->ws_key[0] = '\0';
            c->content_length = 0;
            c->body_pending = 0;
            c->keep_alive = 1; /* HTTP/1.1 default */
            pthread_mutex_unlock(&g_http_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_http_lock);
    return -1;
}

static void conn_free(int cid) {
    if (cid < 0 || cid >= SW_HTTP_MAX_CONNS) return;
    pthread_mutex_lock(&g_http_lock);
    sw_http_conn_t *c = &g_http_conns[cid];
    if (c->buf) { free(c->buf); c->buf = NULL; }
    c->buf_len = 0;
    c->buf_cap = 0;
    c->active = 0;
    c->port = NULL;
    c->handler = NULL;
    pthread_mutex_unlock(&g_http_lock);
}

static int conn_find_by_port(sw_port_t *port) {
    for (int i = 0; i < SW_HTTP_MAX_CONNS; i++) {
        if (g_http_conns[i].active && g_http_conns[i].port == port)
            return i;
    }
    return -1;
}

/* === Base64 Encode (for SHA-1 digest → WebSocket accept key) === */

static void base64_encode(const uint8_t *in, int len, char *out) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, j;
    for (i = 0, j = 0; i < len; i += 3, j += 4) {
        uint32_t n = ((uint32_t)in[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < len) n |= (uint32_t)in[i + 2];
        out[j]     = t[(n >> 18) & 63];
        out[j + 1] = t[(n >> 12) & 63];
        out[j + 2] = (i + 1 < len) ? t[(n >> 6) & 63] : '=';
        out[j + 3] = (i + 2 < len) ? t[n & 63] : '=';
    }
    out[j] = '\0';
}

/* === WebSocket Handshake === */

static const char *WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static void ws_do_handshake(int cid) {
    sw_http_conn_t *c = &g_http_conns[cid];

    /* SHA-1(key + magic GUID) */
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", c->ws_key, WS_MAGIC);

    uint8_t digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(combined, (CC_LONG)strlen(combined), digest);

    char accept[64];
    base64_encode(digest, CC_SHA1_DIGEST_LENGTH, accept);

    /* Send 101 Switching Protocols */
    char response[512];
    int rlen = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);

    sw_tcp_send(c->port, response, rlen);
}

/* === WebSocket Frame Parsing (client → server, masked) === */

static void ws_try_parse(int cid) {
    sw_http_conn_t *c = &g_http_conns[cid];

    while (c->buf_len >= 2) {
        uint8_t *p = c->buf;
        uint8_t opcode = p[0] & 0x0F;
        int masked = (p[1] & 0x80) != 0;
        uint64_t payload_len = p[1] & 0x7F;
        uint32_t hdr_len = 2;

        if (payload_len == 126) {
            if (c->buf_len < 4) return;
            payload_len = ((uint16_t)p[2] << 8) | p[3];
            hdr_len = 4;
        } else if (payload_len == 127) {
            if (c->buf_len < 10) return;
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | p[2 + i];
            hdr_len = 10;
        }

        if (masked) hdr_len += 4;

        /* Reject oversized payloads (16MB max) */
        if (payload_len > 16 * 1024 * 1024) {
            /* Close connection — payload too large */
            c->active = 0;
            return;
        }

        uint64_t frame_len64 = (uint64_t)hdr_len + payload_len;
        if ((uint64_t)c->buf_len < frame_len64) return; /* incomplete frame */
        uint32_t frame_len = (uint32_t)frame_len64;

        /* Unmask payload */
        uint8_t *payload = p + hdr_len;
        if (masked) {
            uint8_t *mask = p + hdr_len - 4;
            for (uint64_t i = 0; i < payload_len; i++)
                payload[i] ^= mask[i % 4];
        }

        if (opcode == 0x1) {
            /* Text frame → send ws_message to handler */
            char *text = (char *)malloc((size_t)payload_len + 1);
            if (!text) { c->active = 0; return; }  /* OOM — drop connection */
            memcpy(text, payload, (size_t)payload_len);
            text[payload_len] = '\0';

            sw_val_t *items[3];
            items[0] = sw_val_atom("ws_message");
            items[1] = sw_val_int(cid);
            items[2] = sw_val_string(text);
            sw_send_tagged(c->handler, SW_TAG_NONE, sw_val_tuple(items, 3));
            free(text);

        } else if (opcode == 0x8) {
            /* Close frame → notify handler */
            sw_val_t *items[2];
            items[0] = sw_val_atom("ws_close");
            items[1] = sw_val_int(cid);
            sw_send_tagged(c->handler, SW_TAG_NONE, sw_val_tuple(items, 2));

            /* Echo close frame back */
            uint8_t close_frame[2] = {0x88, 0x00};
            sw_tcp_send(c->port, close_frame, 2);

        } else if (opcode == 0x9) {
            /* Ping → Pong */
            if (payload_len < 126) {
                uint8_t pong[2];
                pong[0] = 0x8A;
                pong[1] = (uint8_t)payload_len;
                sw_tcp_send(c->port, pong, 2);
                if (payload_len > 0)
                    sw_tcp_send(c->port, payload, (uint32_t)payload_len);
            }
        }
        /* opcode 0xA (pong) — ignore */

        /* Consume frame from buffer */
        uint32_t remaining = c->buf_len - frame_len;
        if (remaining > 0)
            memmove(c->buf, c->buf + frame_len, remaining);
        c->buf_len = remaining;
    }
}

/* === HTTP Request Parsing === */

/* Search for \r\n\r\n in buffer */
static uint8_t *find_header_end(uint8_t *buf, uint32_t len) {
    if (len < 4) return NULL;
    for (uint32_t i = 0; i <= len - 4; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i;
    }
    return NULL;
}

static void http_try_parse(int cid) {
    sw_http_conn_t *c = &g_http_conns[cid];

    /* If we're waiting for body bytes, check if we have enough */
    if (c->body_pending) {
        if (c->buf_len < c->content_length) return; /* still incomplete */
        /* Body complete — fall through to deliver below */
        goto deliver_body;
    }

    {
    uint8_t *end = find_header_end(c->buf, c->buf_len);
    if (!end) return; /* incomplete headers */

    uint32_t header_len = (uint32_t)(end - c->buf) + 4;

    /* Parse request line: METHOD PATH HTTP/1.x */
    char method[16] = {0}, path[512] = {0}, version[16] = {0};
    sscanf((char *)c->buf, "%15s %511s %15s", method, path, version);

    /* Scan headers for WebSocket upgrade, Content-Length, Connection */
    int is_upgrade = 0;
    char ws_key[128] = {0};
    uint32_t content_length = 0;
    int keep_alive = (strstr(version, "1.1") != NULL) ? 1 : 0; /* HTTP/1.1 default */

    char *hdr = (char *)c->buf;
    char *hdr_end = (char *)end;
    /* Skip first line */
    char *line = strchr(hdr, '\n');
    if (line) line++;

    while (line && line < hdr_end) {
        char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end > hdr_end) break;

        if (strncasecmp(line, "Upgrade:", 8) == 0) {
            char *val = line + 8;
            while (*val == ' ') val++;
            if (strncasecmp(val, "websocket", 9) == 0)
                is_upgrade = 1;
        }
        if (strncasecmp(line, "Sec-WebSocket-Key:", 18) == 0) {
            char *val = line + 18;
            while (*val == ' ' || *val == '\t') val++;
            int klen = (int)(line_end - val);
            while (klen > 0 && (val[klen-1] == ' ' || val[klen-1] == '\t' || val[klen-1] == '\r'))
                klen--;
            if (klen > 0 && klen < 128) {
                memcpy(ws_key, val, klen);
                ws_key[klen] = '\0';
            }
        }
        if (strncasecmp(line, "Content-Length:", 14) == 0) {
            char *val = line + 14;
            while (*val == ' ' || *val == '\t') val++;
            content_length = (uint32_t)strtoul(val, NULL, 10);
        }
        if (strncasecmp(line, "Connection:", 11) == 0) {
            char *val = line + 11;
            while (*val == ' ' || *val == '\t') val++;
            if (strncasecmp(val, "close", 5) == 0) keep_alive = 0;
            else if (strncasecmp(val, "keep-alive", 10) == 0) keep_alive = 1;
        }

        line = line_end + 2;
    }

    c->keep_alive = keep_alive;

    /* Consume headers from buffer, leave body bytes */
    uint32_t remaining = c->buf_len - header_len;
    if (remaining > 0)
        memmove(c->buf, c->buf + header_len, remaining);
    c->buf_len = remaining;

    if (is_upgrade && ws_key[0]) {
        /* WebSocket upgrade */
        strncpy(c->ws_key, ws_key, sizeof(c->ws_key) - 1);
        ws_do_handshake(cid);
        c->mode = SW_HTTP_MODE_WS;

        /* Send {'ws_connect', conn, path} to handler */
        sw_val_t *items[3];
        items[0] = sw_val_atom("ws_connect");
        items[1] = sw_val_int(cid);
        items[2] = sw_val_string(path);
        sw_send_tagged(c->handler, SW_TAG_NONE, sw_val_tuple(items, 3));
        return;
    }

    /* If body expected but not yet fully received, buffer it */
    if (content_length > 0 && c->buf_len < content_length) {
        c->content_length = content_length;
        c->body_pending = 1;
        /* Store method/path for later delivery */
        /* We re-parse on delivery, so stash in ws_key temporarily */
        snprintf(c->ws_key, sizeof(c->ws_key), "%s\x01%s", method, path);
        return;
    }

    /* Regular HTTP request — deliver immediately */
    {
        char *body_str = "";
        char *body_buf = NULL;
        if (content_length > 0 && c->buf_len >= content_length) {
            body_buf = (char *)malloc(content_length + 1);
            memcpy(body_buf, c->buf, content_length);
            body_buf[content_length] = '\0';
            body_str = body_buf;
            uint32_t rem = c->buf_len - content_length;
            if (rem > 0)
                memmove(c->buf, c->buf + content_length, rem);
            c->buf_len = rem;
        }

        sw_val_t *items[6];
        items[0] = sw_val_atom("http_request");
        items[1] = sw_val_int(cid);
        items[2] = sw_val_atom(method);
        items[3] = sw_val_string(path);
        items[4] = sw_val_string(""); /* headers (simplified) */
        items[5] = sw_val_string(body_str);
        sw_send_tagged(c->handler, SW_TAG_NONE, sw_val_tuple(items, 6));
        if (body_buf) free(body_buf);
    }
    return;
    }

deliver_body:
    {
        /* Body is now complete — extract method/path from ws_key stash */
        char method[16] = {0}, path[512] = {0};
        char *sep = strchr(c->ws_key, '\x01');
        if (sep) {
            int mlen = (int)(sep - c->ws_key);
            if (mlen > 15) mlen = 15;
            memcpy(method, c->ws_key, mlen);
            strncpy(path, sep + 1, 511);
        }

        char *body_buf = (char *)malloc(c->content_length + 1);
        memcpy(body_buf, c->buf, c->content_length);
        body_buf[c->content_length] = '\0';

        uint32_t rem = c->buf_len - c->content_length;
        if (rem > 0)
            memmove(c->buf, c->buf + c->content_length, rem);
        c->buf_len = rem;
        c->body_pending = 0;
        c->content_length = 0;
        c->ws_key[0] = '\0';

        sw_val_t *items[6];
        items[0] = sw_val_atom("http_request");
        items[1] = sw_val_int(cid);
        items[2] = sw_val_atom(method);
        items[3] = sw_val_string(path);
        items[4] = sw_val_string("");
        items[5] = sw_val_string(body_buf);
        sw_send_tagged(c->handler, SW_TAG_NONE, sw_val_tuple(items, 6));
        free(body_buf);
    }
}

/* === Connection Data Handler === */

static void conn_on_data(int cid, uint8_t *data, uint32_t len) {
    sw_http_conn_t *c = &g_http_conns[cid];

    /* Append to buffer */
    if (c->buf_len + len > c->buf_cap) {
        c->buf_cap = (c->buf_len + len) * 2;
        c->buf = (uint8_t *)realloc(c->buf, c->buf_cap);
    }
    memcpy(c->buf + c->buf_len, data, len);
    c->buf_len += len;

    if (c->mode == SW_HTTP_MODE_HTTP) {
        http_try_parse(cid);
        /* After upgrade, parse any remaining data as WS */
        if (c->mode == SW_HTTP_MODE_WS && c->buf_len > 0)
            ws_try_parse(cid);
    } else {
        ws_try_parse(cid);
    }
}

/* === Connection Close Handler === */

static void conn_on_close(int cid) {
    sw_http_conn_t *c = &g_http_conns[cid];

    if (c->mode == SW_HTTP_MODE_WS && c->handler) {
        /* Notify handler process */
        sw_val_t *items[2];
        items[0] = sw_val_atom("ws_close");
        items[1] = sw_val_int(cid);
        sw_send_tagged(c->handler, SW_TAG_NONE, sw_val_tuple(items, 2));
    }

    conn_free(cid);
}

/* === Bridge Process === */

static void http_bridge_entry(void *arg) {
    sw_http_bridge_t *bridge = (sw_http_bridge_t *)arg;

    while (1) {
        uint64_t tag;
        void *raw = sw_receive_any((uint64_t)-1, &tag);
        if (!raw) continue;

        if (tag == SW_TAG_PORT_ACCEPT) {
            sw_port_accept_t *acc = (sw_port_accept_t *)raw;
            int cid = conn_alloc(acc->conn, bridge->handler);
            if (cid < 0) {
                /* Table full — close the connection */
                sw_port_close(acc->conn);
            }
            free(acc);

        } else if (tag == SW_TAG_PORT_DATA) {
            sw_port_data_t *data = (sw_port_data_t *)raw;
            int cid = conn_find_by_port(data->port);
            if (cid >= 0) {
                conn_on_data(cid, data->data, data->len);
            }
            free(data->data);
            free(data);

        } else if (tag == SW_TAG_PORT_CLOSED) {
            sw_port_event_t *evt = (sw_port_event_t *)raw;
            int cid = conn_find_by_port(evt->port);
            if (cid >= 0) {
                conn_on_close(cid);
            }
            free(evt);
        }
    }
}

/* === Public API === */

sw_process_t *sw_http_listen(uint16_t port, sw_process_t *handler) {
    /* Create TCP listener (owned by caller initially) */
    sw_port_t *listener = sw_tcp_listen(NULL, port);
    if (!listener) return NULL;

    /* Allocate bridge state */
    sw_http_bridge_t *bridge = (sw_http_bridge_t *)calloc(1, sizeof(sw_http_bridge_t));
    bridge->listener = listener;
    bridge->handler = handler;
    bridge->port = port;

    /* Spawn bridge process */
    sw_process_t *bp = sw_spawn(http_bridge_entry, bridge);

    /* Transfer listener ownership to bridge */
    sw_port_controlling_process(listener, bp);

    return bp;
}

int sw_http_respond(int conn_id, int status, const char *headers, const char *body) {
    if (conn_id < 0 || conn_id >= SW_HTTP_MAX_CONNS) return -1;
    sw_http_conn_t *c = &g_http_conns[conn_id];
    if (!c->active || !c->port) return -1;

    const char *status_text;
    switch (status) {
    case 200: status_text = "OK"; break;
    case 301: status_text = "Moved Permanently"; break;
    case 302: status_text = "Found"; break;
    case 304: status_text = "Not Modified"; break;
    case 400: status_text = "Bad Request"; break;
    case 404: status_text = "Not Found"; break;
    case 500: status_text = "Internal Server Error"; break;
    default:  status_text = "OK"; break;
    }

    uint32_t body_len = body ? (uint32_t)strlen(body) : 0;

    /* Build response header */
    const char *conn_hdr = c->keep_alive ? "keep-alive" : "close";
    char head[1024];
    int hlen = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: %s\r\n"
        "%s"
        "\r\n",
        status, status_text, body_len, conn_hdr,
        headers ? headers : "");

    sw_tcp_send(c->port, head, hlen);
    if (body_len > 0)
        sw_tcp_send(c->port, body, body_len);

    /* Close connection if not keep-alive */
    if (!c->keep_alive) {
        if (c->port) sw_port_close(c->port);
        conn_free(conn_id);
    }

    return 0;
}

int sw_http_respond_raw(int conn_id, int status, const char *headers, const void *data, uint32_t data_len) {
    if (conn_id < 0 || conn_id >= SW_HTTP_MAX_CONNS) return -1;
    sw_http_conn_t *c = &g_http_conns[conn_id];
    if (!c->active || !c->port) return -1;

    const char *status_text;
    switch (status) {
    case 200: status_text = "OK"; break;
    case 404: status_text = "Not Found"; break;
    case 413: status_text = "Payload Too Large"; break;
    default:  status_text = "OK"; break;
    }

    const char *conn_hdr = c->keep_alive ? "keep-alive" : "close";
    char head[1024];
    int hlen = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: %s\r\n"
        "%s"
        "\r\n",
        status, status_text, data_len, conn_hdr,
        headers ? headers : "");

    sw_tcp_send(c->port, head, hlen);
    if (data_len > 0 && data)
        sw_tcp_send(c->port, data, data_len);

    if (!c->keep_alive) {
        if (c->port) sw_port_close(c->port);
        conn_free(conn_id);
    }
    return 0;
}

int sw_ws_send_text(int conn_id, const char *data, uint32_t len) {
    if (conn_id < 0 || conn_id >= SW_HTTP_MAX_CONNS) return -1;
    sw_http_conn_t *c = &g_http_conns[conn_id];
    if (!c->active || c->mode != SW_HTTP_MODE_WS || !c->port) return -1;

    /* Build WebSocket frame header (server → client, no mask) */
    uint8_t hdr[10];
    int hdr_len;
    hdr[0] = 0x81; /* FIN + text opcode */

    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hdr_len = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (len >> 8) & 0xFF;
        hdr[3] = len & 0xFF;
        hdr_len = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (len >> (56 - i * 8)) & 0xFF;
        hdr_len = 10;
    }

    sw_tcp_send(c->port, hdr, hdr_len);
    sw_tcp_send(c->port, data, len);
    return 0;
}

int sw_ws_close(int conn_id) {
    if (conn_id < 0 || conn_id >= SW_HTTP_MAX_CONNS) return -1;
    sw_http_conn_t *c = &g_http_conns[conn_id];
    if (!c->active) return -1;

    if (c->mode == SW_HTTP_MODE_WS && c->port) {
        uint8_t close_frame[2] = {0x88, 0x00};
        sw_tcp_send(c->port, close_frame, 2);
    }

    if (c->port) sw_port_close(c->port);
    conn_free(conn_id);
    return 0;
}

int sw_ws_set_handler(int conn_id, sw_process_t *handler) {
    if (conn_id < 0 || conn_id >= SW_HTTP_MAX_CONNS) return -1;
    sw_http_conn_t *c = &g_http_conns[conn_id];
    if (!c->active) return -1;

    c->handler = handler;
    return 0;
}

const char *sw_liveview_js(void) {
    return SWARMRT_LIVEVIEW_JS;
}
