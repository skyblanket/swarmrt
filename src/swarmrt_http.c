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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
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
#include "swarmrt_audio.h"   /* base64 helpers for delivering binary WS frames */

/* === Global Connection Table === */

static sw_http_conn_t g_http_conns[SW_HTTP_MAX_CONNS];
static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;

/* === Request-size cap (SW_HTTP_MAX_REQUEST) ===
 * 0 = not yet parsed; first caller resolves env → default. _Atomic because
 * two listeners mean two bridge fibers on different scheduler threads can
 * race the lazy init (both compute the same value; atomics keep it TSan-
 * clean). Values below 4096 are rejected (a cap smaller than one read
 * buffer would break every request). */
static _Atomic uint32_t g_http_max_request;

static uint32_t http_max_request(void) {
    uint32_t v = atomic_load_explicit(&g_http_max_request, memory_order_relaxed);
    if (v) return v;
    unsigned long long n = SW_HTTP_MAX_REQUEST_DEFAULT;
    const char *e = getenv("SW_HTTP_MAX_REQUEST");
    if (e && *e) {
        unsigned long long p = strtoull(e, NULL, 10);
        if (p >= 4096ULL && p <= 0xFFFFFFFFULL) n = p;
    }
    atomic_store_explicit(&g_http_max_request, (uint32_t)n, memory_order_relaxed);
    return (uint32_t)n;
}

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
            c->req_headers = NULL;
            c->req_path = NULL;
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
    if (c->frag_buf) { free(c->frag_buf); c->frag_buf = NULL; }
    /* req_headers is a managed sw_val_t; drop our reference and let the GC
     * reclaim it (it may also be referenced by a delivered message that the
     * handler still holds). req_path is a plain heap string we own. */
    c->req_headers = NULL;
    if (c->req_path) { free(c->req_path); c->req_path = NULL; }
    c->frag_len = 0;
    c->frag_cap = 0;
    c->frag_opcode = 0;
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

/* Deliver a fully-assembled WS data message to the connection's handler.
 *   text   (opcode 0x1) → {'ws_message', cid, text}
 *   binary (opcode 0x2) → {'ws_binary',  cid, base64(payload)}
 * Binary payloads are base64-encoded so NUL-bearing bytes survive in a
 * sw string (sw strings are NUL-terminated; see swarmrt_audio.h). */
static void ws_deliver_message(int cid, int opcode, const uint8_t *payload, uint32_t len) {
    sw_http_conn_t *c = &g_http_conns[cid];
    if (opcode == 0x2) {
        char *b64 = _sw_audio_b64_encode(payload, (size_t)len);
        if (!b64) { c->active = 0; return; }
        sw_val_t *items[3];
        items[0] = sw_val_atom("ws_binary");
        items[1] = sw_val_int(cid);
        items[2] = sw_val_string(b64);
        sw_send_value(c->handler, SW_TAG_NONE, sw_val_tuple(items, 3));
        free(b64);
    } else {
        /* text */
        char *text = (char *)malloc((size_t)len + 1);
        if (!text) { c->active = 0; return; }
        memcpy(text, payload, (size_t)len);
        text[len] = '\0';
        sw_val_t *items[3];
        items[0] = sw_val_atom("ws_message");
        items[1] = sw_val_int(cid);
        items[2] = sw_val_string(text);
        sw_send_value(c->handler, SW_TAG_NONE, sw_val_tuple(items, 3));
        free(text);
    }
}

static void ws_try_parse(int cid) {
    sw_http_conn_t *c = &g_http_conns[cid];

    while (c->buf_len >= 2) {
        uint8_t *p = c->buf;
        int fin = (p[0] & 0x80) != 0;
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

        /* Data frames: text (0x1), binary (0x2), continuation (0x0).
         * Reassemble fragmented messages (FIN=0 runs) before delivery.
         * A FIN=1 single frame is the common (non-fragmented) case. */
        if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
            int eff_opcode = opcode;
            if (opcode == 0x0) {
                /* Continuation — must be part of an in-progress message. */
                if (c->frag_opcode == 0) {
                    /* Stray continuation; drop the connection defensively. */
                    c->active = 0; return;
                }
                eff_opcode = c->frag_opcode;
            }

            int fragmenting = (c->frag_opcode != 0) || (!fin);
            if (fragmenting) {
                /* Append this frame's payload to the reassembly buffer. */
                if (opcode != 0x0 && c->frag_opcode == 0) c->frag_opcode = opcode;
                uint32_t need = c->frag_len + (uint32_t)payload_len;
                if (need > 16 * 1024 * 1024) { c->active = 0; return; }
                if (need > c->frag_cap) {
                    uint32_t ncap = c->frag_cap ? c->frag_cap : 4096;
                    while (ncap < need) ncap *= 2;
                    uint8_t *nb = (uint8_t *)realloc(c->frag_buf, ncap);
                    if (!nb) { c->active = 0; return; }
                    c->frag_buf = nb; c->frag_cap = ncap;
                }
                memcpy(c->frag_buf + c->frag_len, payload, (size_t)payload_len);
                c->frag_len += (uint32_t)payload_len;

                if (fin) {
                    /* Message complete — deliver and reset. */
                    ws_deliver_message(cid, eff_opcode, c->frag_buf, c->frag_len);
                    c->frag_len = 0;
                    c->frag_opcode = 0;
                }
            } else {
                /* Single, unfragmented frame — deliver directly. */
                ws_deliver_message(cid, eff_opcode, payload, (uint32_t)payload_len);
            }

        } else if (opcode == 0x8) {
            /* Close frame → notify handler */
            sw_val_t *items[2];
            items[0] = sw_val_atom("ws_close");
            items[1] = sw_val_int(cid);
            sw_send_value(c->handler, SW_TAG_NONE, sw_val_tuple(items, 2));

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

/* Bound the header parse — this is a classic overflow surface, so every
 * count/length is capped and the temporaries are stack arrays we never
 * write past. A request that exceeds these limits simply gets a truncated
 * map (still safe), never an overrun. */
#define SW_HTTP_MAX_HEADERS   128   /* distinct header lines kept */
#define SW_HTTP_MAX_HDR_NAME  128   /* bytes of a header name (incl. NUL) */
#define SW_HTTP_MAX_HDR_VALUE 4096  /* bytes of a header value (incl. NUL) */

/* Parse the header block [hdr_start, hdr_end) (i.e. the lines AFTER the
 * request line, up to but excluding the terminating CRLF CRLF) into a fresh
 * sw_val_t MAP. Keys are the header names lowercased (so handlers can match
 * `map_get(headers, "authorization")` regardless of on-the-wire casing);
 * values are the raw field text with surrounding whitespace trimmed.
 *
 * A duplicate header name overwrites the earlier value via sw_val_map_put
 * (last wins) — adequate for auth/signature reads; combine-with-comma is not
 * required by any caller. Returns an empty map if there are no header lines.
 * Never reads past hdr_end; never writes past the bounded stack temporaries. */
static sw_val_t *http_parse_headers(const char *hdr_start, const char *hdr_end) {
    sw_val_t *map = sw_val_map_new(NULL, NULL, 0);
    if (!hdr_start || hdr_start >= hdr_end) return map;

    const char *line = hdr_start;
    int kept = 0;
    while (line < hdr_end && kept < SW_HTTP_MAX_HEADERS) {
        /* Find end of this header line (CRLF). strstr is NUL-safe because
         * conn_on_data always NUL-terminates c->buf one past buf_len. */
        const char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end > hdr_end) break;
        if (line_end == line) break; /* empty line — end of headers */

        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon && colon > line) {
            /* Header NAME — lowercased into a bounded buffer. */
            char name[SW_HTTP_MAX_HDR_NAME];
            size_t nlen = (size_t)(colon - line);
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            for (size_t i = 0; i < nlen; i++) {
                char ch = line[i];
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                name[i] = ch;
            }
            name[nlen] = '\0';

            /* Header VALUE — skip OWS after the colon, trim trailing OWS. */
            const char *vstart = colon + 1;
            while (vstart < line_end && (*vstart == ' ' || *vstart == '\t'))
                vstart++;
            const char *vend = line_end;
            while (vend > vstart && (vend[-1] == ' ' || vend[-1] == '\t'))
                vend--;
            char value[SW_HTTP_MAX_HDR_VALUE];
            size_t vlen = (size_t)(vend - vstart);
            if (vlen >= sizeof(value)) vlen = sizeof(value) - 1;
            memcpy(value, vstart, vlen);
            value[vlen] = '\0';

            sw_val_t *k = sw_val_string(name);
            sw_val_t *v = sw_val_string(value);
            map = sw_val_map_put(map, k, v); /* last-wins on duplicate name */
            kept++;
        }
        /* Advance past CRLF. */
        line = line_end + 2;
    }
    return map;
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
    int has_content_length = 0;
    int is_chunked = 0;
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
        /* "Content-Length:" is 15 chars (note the trailing colon) — earlier
         * versions used 14 here, which left `val` pointing at the ':' and made
         * strtoul return 0 for every POST body. */
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *val = line + 15;
            while (*val == ' ' || *val == '\t') val++;
            content_length = (uint32_t)strtoul(val, NULL, 10);
            has_content_length = 1;
        }
        if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            char *val = line + 18;
            while (*val == ' ' || *val == '\t') val++;
            /* RFC 7230: chunked is always the last (or only) coding */
            if (strncasecmp(val, "chunked", 7) == 0 ||
                (line_end - val >= 7 && strncasecmp(line_end - 7, "chunked", 7) == 0)) {
                is_chunked = 1;
            }
        }
        if (strncasecmp(line, "Connection:", 11) == 0) {
            char *val = line + 11;
            while (*val == ' ' || *val == '\t') val++;
            if (strncasecmp(val, "close", 5) == 0) keep_alive = 0;
            else if (strncasecmp(val, "keep-alive", 10) == 0) keep_alive = 1;
        }

        line = line_end + 2;
    }

    /* Reject oversized declared bodies up front with a 413 instead of
     * buffering toward the conn_on_data cap: strtoul gave the client a
     * free uint32-range Content-Length, i.e. a request to buffer ~4GB.
     * keep_alive=0 makes sw_http_respond close + conn_free the slot. */
    if (has_content_length && content_length > http_max_request()) {
        fprintf(stderr,
            "swarmrt_http: rejecting %s %s — Content-Length %u exceeds "
            "SW_HTTP_MAX_REQUEST (%u bytes)\n",
            method, path, content_length, http_max_request());
        c->keep_alive = 0;
        sw_http_respond(cid, 413, NULL, "Payload Too Large");
        return;
    }

    /* Build the request-header MAP now, while c->buf is still intact (the
     * consume/memmove below mutates it). `line` after the request line is
     * the first header; hdr_end is the start of the terminating CRLF CRLF.
     * Stash on the conn so it survives a buffered POST body and is reachable
     * for a WS upgrade. Replaces any stale map from a prior keep-alive
     * request on this connection. */
    {
        char *hdr1 = strchr(hdr, '\n');
        if (hdr1) hdr1++;
        c->req_headers = http_parse_headers(hdr1, hdr_end);
        if (c->req_path) { free(c->req_path); c->req_path = NULL; }
        c->req_path = strdup(path);
    }

    /* Chunked transfer-encoding: not fully supported. Don't hang waiting for
     * a Content-Length that will never arrive — drain anything that looks
     * like a terminating chunk and dispatch with empty body. */
    if (is_chunked && !has_content_length) {
        fprintf(stderr,
            "swarmrt_http: Transfer-Encoding: chunked is not yet supported; "
            "dispatching %s %s with empty body\n", method, path);
        /* Best-effort: if "0\r\n\r\n" is in the buffer, skip past it */
        if (c->buf_len >= 5) {
            for (uint32_t i = 0; i + 5 <= c->buf_len; i++) {
                if (c->buf[i] == '0' && c->buf[i+1] == '\r' && c->buf[i+2] == '\n' &&
                    c->buf[i+3] == '\r' && c->buf[i+4] == '\n') {
                    uint32_t skip = i + 5;
                    uint32_t rem = c->buf_len - skip;
                    if (rem > 0)
                        memmove(c->buf, c->buf + skip, rem);
                    c->buf_len = rem;
                    break;
                }
            }
        }
        content_length = 0;
    }

    c->keep_alive = keep_alive;

    /* Consume headers from buffer, leave body bytes */
    uint32_t remaining = c->buf_len - header_len;
    if (remaining > 0)
        memmove(c->buf, c->buf + header_len, remaining);
    c->buf_len = remaining;

    if (is_upgrade && ws_key[0]) {
        /* WebSocket upgrade. The upgrade request's headers (Origin,
         * Authorization, the Telnyx signature headers, etc.) and its
         * path+query are retained on the connection (c->req_headers /
         * c->req_path, set above) for the whole WS lifetime, queryable by
         * the handler via ws_request_headers(conn) / ws_request_path(conn).
         * The {'ws_connect', conn, path} message keeps its original 3-tuple
         * shape so existing handlers (studio LiveView, swarm-live) keep
         * working untouched. */
        strncpy(c->ws_key, ws_key, sizeof(c->ws_key) - 1);
        ws_do_handshake(cid);
        c->mode = SW_HTTP_MODE_WS;

        /* Send {'ws_connect', conn, path} to handler */
        sw_val_t *items[3];
        items[0] = sw_val_atom("ws_connect");
        items[1] = sw_val_int(cid);
        items[2] = sw_val_string(path);
        sw_send_value(c->handler, SW_TAG_NONE, sw_val_tuple(items, 3));
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
        /* headers: the real request-header MAP (lowercased keys), built
         * above into c->req_headers. Hand it off by reference and drop our
         * pointer — the delivered message keeps it alive. */
        items[4] = c->req_headers ? c->req_headers : sw_val_map_new(NULL, NULL, 0);
        items[5] = sw_val_string(body_str);
        sw_send_value(c->handler, SW_TAG_NONE, sw_val_tuple(items, 6));
        c->req_headers = NULL;
        if (c->req_path) { free(c->req_path); c->req_path = NULL; }
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
        /* headers MAP captured when the header block first arrived (before
         * the body was buffered). Hand off by reference, then drop. */
        items[4] = c->req_headers ? c->req_headers : sw_val_map_new(NULL, NULL, 0);
        items[5] = sw_val_string(body_buf);
        sw_send_value(c->handler, SW_TAG_NONE, sw_val_tuple(items, 6));
        c->req_headers = NULL;
        if (c->req_path) { free(c->req_path); c->req_path = NULL; }
        free(body_buf);
    }
}

/* === Connection Data Handler === */

static void conn_on_data(int cid, uint8_t *data, uint32_t len) {
    sw_http_conn_t *c = &g_http_conns[cid];

    /* Request-size cap: one client must not be able to grow this buffer
     * without bound (headers that never terminate, a body the client keeps
     * streaming, WS bytes piling up behind a stalled parse). 64-bit sum so
     * buf_len+len can't wrap. Close + free the slot — same defensive shape
     * as the WS oversize rejection, but conn_free'd so the slot and buffer
     * are actually reclaimed. Rate-limited stderr keeps the drop LOUD. */
    if ((uint64_t)c->buf_len + (uint64_t)len + 1 > (uint64_t)http_max_request()) {
        static _Atomic uint32_t g_http_oversize_drops;
        uint32_t d = atomic_fetch_add_explicit(&g_http_oversize_drops, 1,
                                               memory_order_relaxed);
        if ((d & 0xFF) == 0) {
            fprintf(stderr,
                "swarmrt_http: closing conn %d — request buffer would exceed "
                "SW_HTTP_MAX_REQUEST (%u bytes); %u oversize connections "
                "dropped so far\n",
                cid, http_max_request(), d + 1);
        }
        if (c->port) sw_port_close(c->port);
        conn_free(cid);
        return;
    }

    /* Append to buffer. Reserve one extra byte beyond buf_len so we can
     * always NUL-terminate: http_try_parse() scans c->buf with sscanf /
     * strchr / strstr, which over-read past the allocation when an inbound
     * request exactly fills buf_cap and leaves no terminator (ASAN:
     * heap-buffer-overflow READ of size N+1 just past the read buffer). */
    if (c->buf_len + len + 1 > c->buf_cap) {
        c->buf_cap = (c->buf_len + len + 1) * 2;
        c->buf = (uint8_t *)realloc(c->buf, c->buf_cap);
    }
    memcpy(c->buf + c->buf_len, data, len);
    c->buf_len += len;
    c->buf[c->buf_len] = '\0'; /* keep the scan buffer C-string safe */

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
        sw_send_value(c->handler, SW_TAG_NONE, sw_val_tuple(items, 2));
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
    case 413: status_text = "Payload Too Large"; break;
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

int sw_ws_send_binary(int conn_id, const char *data, uint32_t len) {
    if (conn_id < 0 || conn_id >= SW_HTTP_MAX_CONNS) return -1;
    sw_http_conn_t *c = &g_http_conns[conn_id];
    if (!c->active || c->mode != SW_HTTP_MODE_WS || !c->port) return -1;

    /* WebSocket frame header (server → client, no mask), opcode 0x2. */
    uint8_t hdr[10];
    int hdr_len;
    hdr[0] = 0x82; /* FIN + binary opcode */

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

/* Request-header MAP captured from the UPGRADE request, for a live WS conn.
 * Returns a fresh empty map (never NULL) if the connection has no captured
 * headers, so handlers can map_get without a nil guard. */
sw_val_t *sw_ws_request_headers(int conn_id) {
    if (conn_id < 0 || conn_id >= SW_HTTP_MAX_CONNS)
        return sw_val_map_new(NULL, NULL, 0);
    sw_http_conn_t *c = &g_http_conns[conn_id];
    if (!c->active || !c->req_headers)
        return sw_val_map_new(NULL, NULL, 0);
    return c->req_headers;
}

/* Request path+query from the UPGRADE request, for a live WS conn.
 * Returns "" if unknown. */
const char *sw_ws_request_path(int conn_id) {
    if (conn_id < 0 || conn_id >= SW_HTTP_MAX_CONNS) return "";
    sw_http_conn_t *c = &g_http_conns[conn_id];
    if (!c->active || !c->req_path) return "";
    return c->req_path;
}

const char *sw_liveview_js(void) {
    return SWARMRT_LIVEVIEW_JS;
}

#ifdef SW_FUZZ_HTTP
/* Fuzz entry: drive the request-line scan + header-block parser over
 * arbitrary bytes, exactly as conn_on_data → http_try_parse would, but
 * scheduler-free (no port, no handler). Exercises http_parse_headers and
 * the find_header_end / request-line sscanf surface I touched. The caller
 * (tests/fuzz/fuzz_http.c) passes a NUL-terminated copy. */
void sw_http_fuzz_parse(const char *buf, uint32_t len) {
    if (!buf) return;
    uint8_t *end = find_header_end((uint8_t *)buf, len);
    if (!end) return; /* incomplete headers — nothing to parse */

    /* Request line (same bounded sscanf as http_try_parse). */
    char method[16] = {0}, path[512] = {0}, version[16] = {0};
    sscanf(buf, "%15s %511s %15s", method, path, version);

    /* Header block = first line after the request line, up to the CRLFCRLF. */
    const char *hdr1 = strchr(buf, '\n');
    if (hdr1) hdr1++;
    sw_val_t *map = http_parse_headers(hdr1, (const char *)end);
    /* Touch the map so the build isn't optimized away; value is discarded
     * (GC reclaims it — we run ASAN with detect_leaks=0). */
    (void)sw_val_map_get(map, sw_val_string("authorization"));
}
#endif /* SW_FUZZ_HTTP */
