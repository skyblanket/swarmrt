/*
 * SwarmRT Phase 9: Node & Distribution
 *
 * Each node is a SwarmRT instance with a TCP listener. Nodes connect to each
 * other and exchange messages using a simple binary protocol:
 *
 * Wire format: [4-byte length][sw_remote_msg_t header][payload bytes]
 *
 * The distribution process handles incoming connections and message routing.
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
#include <stdatomic.h>
#include <arpa/inet.h>
#include "swarmrt_node.h"
#include "swarmrt_lang.h"

/* === Global State === */

static sw_node_t g_node;
static sw_peer_t g_peers[SW_NODE_MAX_PEERS];
static uint32_t g_peer_count = 0;
static pthread_mutex_t g_dist_lock = PTHREAD_MUTEX_INITIALIZER;
static sw_process_t *g_dist_proc = NULL; /* Distribution handler process */

/* === Type-preserving binary marshal/unmarshal ===
 *
 * JSON loses the tuples-vs-lists and atoms-vs-strings distinction, so a
 * `{'echo', from, msg}` tuple round-tripped through JSON arrives as an
 * `["echo", from, msg]` list of strings — the sw receive pattern stops
 * matching. This compact tagged-binary format preserves the types that
 * the runtime pattern-matcher cares about.
 *
 * Wire format per value: 1 type byte + length-prefixed payload.
 *
 *   0x00  nil
 *   0x01  int64  (8 bytes, little-endian)
 *   0x02  double (8 bytes, IEEE 754)
 *   0x03  atom   (uint16 len + bytes, no terminator)
 *   0x04  string (uint32 len + bytes, no terminator)
 *   0x05  tuple  (uint16 count + N values)
 *   0x06  list   (uint32 count + N values)
 *   0x07  map    (uint16 count + N key-value pairs)
 *   0x08  pid    (8 bytes pid id; receiver annotates it with the sender
 *                 node so replies can route back over distribution)
 *
 * Multi-byte fields use native byte order; both nodes are assumed to
 * have the same word size and endianness for v1. A version byte can be
 * added later if cross-arch distribution becomes a goal.
 */

#define SW_MARSHAL_NIL    0x00
#define SW_MARSHAL_INT    0x01
#define SW_MARSHAL_FLOAT  0x02
#define SW_MARSHAL_ATOM   0x03
#define SW_MARSHAL_STRING 0x04
#define SW_MARSHAL_TUPLE  0x05
#define SW_MARSHAL_LIST   0x06
#define SW_MARSHAL_MAP    0x07
#define SW_MARSHAL_PID    0x08

/* Thread-local node name used by unmarsh_val to construct
 * SW_VAL_REMOTE_PID values when it decodes a SW_MARSHAL_PID byte.
 * Set on entry to sw_unmarshal, cleared on exit. unmarsh_val doesn't
 * take this as a parameter because it's recursive and most callers
 * don't care; this keeps the recursive plumbing clean. */
static __thread const char *tls_unmarshal_default_node = NULL;

static void marsh_grow(uint8_t **buf, uint32_t *cap, uint32_t need) {
    if (need <= *cap) return;
    uint32_t nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    *buf = (uint8_t *)realloc(*buf, nc);
    *cap = nc;
}

static int marsh_val(sw_val_t *v, uint8_t **buf, uint32_t *cap, uint32_t *pos) {
    if (!v || v->type == SW_VAL_NIL) {
        marsh_grow(buf, cap, *pos + 1);
        (*buf)[(*pos)++] = SW_MARSHAL_NIL;
        return 0;
    }
    switch (v->type) {
    case SW_VAL_INT: {
        marsh_grow(buf, cap, *pos + 1 + 8);
        (*buf)[(*pos)++] = SW_MARSHAL_INT;
        int64_t i = v->v.i;
        memcpy(*buf + *pos, &i, 8); *pos += 8;
        return 0;
    }
    case SW_VAL_FLOAT: {
        marsh_grow(buf, cap, *pos + 1 + 8);
        (*buf)[(*pos)++] = SW_MARSHAL_FLOAT;
        double d = v->v.f;
        memcpy(*buf + *pos, &d, 8); *pos += 8;
        return 0;
    }
    case SW_VAL_ATOM: {
        uint16_t len = (uint16_t)strlen(v->v.str);
        marsh_grow(buf, cap, *pos + 1 + 2 + len);
        (*buf)[(*pos)++] = SW_MARSHAL_ATOM;
        memcpy(*buf + *pos, &len, 2); *pos += 2;
        memcpy(*buf + *pos, v->v.str, len); *pos += len;
        return 0;
    }
    case SW_VAL_STRING: {
        uint32_t len = (uint32_t)strlen(v->v.str);
        marsh_grow(buf, cap, *pos + 1 + 4 + len);
        (*buf)[(*pos)++] = SW_MARSHAL_STRING;
        memcpy(*buf + *pos, &len, 4); *pos += 4;
        memcpy(*buf + *pos, v->v.str, len); *pos += len;
        return 0;
    }
    case SW_VAL_TUPLE:
    case SW_VAL_LIST: {
        uint8_t tag = (v->type == SW_VAL_TUPLE) ? SW_MARSHAL_TUPLE : SW_MARSHAL_LIST;
        marsh_grow(buf, cap, *pos + 1 + 4);
        (*buf)[(*pos)++] = tag;
        if (tag == SW_MARSHAL_TUPLE) {
            uint16_t c = (uint16_t)v->v.tuple.count;
            memcpy(*buf + *pos, &c, 2); *pos += 2;
        } else {
            uint32_t c = (uint32_t)v->v.tuple.count;
            memcpy(*buf + *pos, &c, 4); *pos += 4;
        }
        for (int i = 0; i < v->v.tuple.count; i++)
            if (marsh_val(v->v.tuple.items[i], buf, cap, pos) < 0) return -1;
        return 0;
    }
    case SW_VAL_MAP: {
        uint16_t c = (uint16_t)v->v.map.count;
        marsh_grow(buf, cap, *pos + 1 + 2);
        (*buf)[(*pos)++] = SW_MARSHAL_MAP;
        memcpy(*buf + *pos, &c, 2); *pos += 2;
        for (int i = 0; i < v->v.map.count; i++) {
            if (marsh_val(v->v.map.keys[i], buf, cap, pos) < 0) return -1;
            if (marsh_val(v->v.map.vals[i], buf, cap, pos) < 0) return -1;
        }
        return 0;
    }
    case SW_VAL_PID: {
        /* Encode the pid id only — the sending node's name is set on the
         * receiver side via tls_unmarshal_default_node so the receiver
         * can reconstruct this as a SW_VAL_REMOTE_PID that routes back
         * over TCP when used in `send()`. */
        marsh_grow(buf, cap, *pos + 1 + 8);
        (*buf)[(*pos)++] = SW_MARSHAL_PID;
        uint64_t pid = v->v.pid ? v->v.pid->pid : 0;
        memcpy(*buf + *pos, &pid, 8); *pos += 8;
        return 0;
    }
    case SW_VAL_REMOTE_PID: {
        /* Forwarding an already-remote pid: same wire format as a
         * local pid (id only), but the receiver's default_remote_node
         * will be the FORWARDER's node, not the original owner. We
         * encode the owner's node here as a string sentinel to preserve
         * identity — kept as SW_MARSHAL_PID for now since cross-node
         * forwarding is a future-iteration feature. */
        marsh_grow(buf, cap, *pos + 1 + 8);
        (*buf)[(*pos)++] = SW_MARSHAL_PID;
        uint64_t id = v->v.rpid.id;
        memcpy(*buf + *pos, &id, 8); *pos += 8;
        return 0;
    }
    default:
        /* Unsupported (FUN, etc.) — encode as nil to avoid silent corruption */
        marsh_grow(buf, cap, *pos + 1);
        (*buf)[(*pos)++] = SW_MARSHAL_NIL;
        return 0;
    }
}

/* Returns the marshalled byte length; -1 on failure. Caller frees *out. */
int sw_marshal(sw_val_t *v, uint8_t **out, uint32_t *out_len) {
    uint8_t *buf = NULL;
    uint32_t cap = 0, pos = 0;
    if (marsh_val(v, &buf, &cap, &pos) < 0) { free(buf); return -1; }
    *out = buf;
    *out_len = pos;
    return 0;
}

static sw_val_t *unmarsh_val(const uint8_t *buf, uint32_t len, uint32_t *pos) {
    if (*pos >= len) return sw_val_nil();
    uint8_t tag = buf[(*pos)++];
    switch (tag) {
    case SW_MARSHAL_NIL: return sw_val_nil();
    case SW_MARSHAL_INT: {
        if (*pos + 8 > len) return sw_val_nil();
        int64_t i; memcpy(&i, buf + *pos, 8); *pos += 8;
        return sw_val_int(i);
    }
    case SW_MARSHAL_FLOAT: {
        if (*pos + 8 > len) return sw_val_nil();
        double d; memcpy(&d, buf + *pos, 8); *pos += 8;
        return sw_val_float(d);
    }
    case SW_MARSHAL_ATOM: {
        if (*pos + 2 > len) return sw_val_nil();
        uint16_t l; memcpy(&l, buf + *pos, 2); *pos += 2;
        if (*pos + l > len) return sw_val_nil();
        char *tmp = (char *)malloc(l + 1);
        memcpy(tmp, buf + *pos, l); tmp[l] = 0; *pos += l;
        sw_val_t *r = sw_val_atom(tmp);
        free(tmp);
        return r;
    }
    case SW_MARSHAL_STRING: {
        if (*pos + 4 > len) return sw_val_nil();
        uint32_t l; memcpy(&l, buf + *pos, 4); *pos += 4;
        if (*pos + l > len) return sw_val_nil();
        char *tmp = (char *)malloc(l + 1);
        memcpy(tmp, buf + *pos, l); tmp[l] = 0; *pos += l;
        sw_val_t *r = sw_val_string(tmp);
        free(tmp);
        return r;
    }
    case SW_MARSHAL_TUPLE:
    case SW_MARSHAL_LIST: {
        int is_tuple = (tag == SW_MARSHAL_TUPLE);
        uint32_t c;
        if (is_tuple) {
            if (*pos + 2 > len) return sw_val_nil();
            uint16_t s; memcpy(&s, buf + *pos, 2); *pos += 2; c = s;
        } else {
            if (*pos + 4 > len) return sw_val_nil();
            memcpy(&c, buf + *pos, 4); *pos += 4;
        }
        if (c > 100000) return sw_val_nil(); /* sanity cap */
        sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * (c ? c : 1));
        for (uint32_t i = 0; i < c; i++) items[i] = unmarsh_val(buf, len, pos);
        sw_val_t *r = is_tuple ? sw_val_tuple(items, (int)c)
                               : sw_val_list(items, (int)c);
        free(items);
        return r;
    }
    case SW_MARSHAL_MAP: {
        if (*pos + 2 > len) return sw_val_nil();
        uint16_t c; memcpy(&c, buf + *pos, 2); *pos += 2;
        sw_val_t **k = (sw_val_t **)malloc(sizeof(sw_val_t *) * (c ? c : 1));
        sw_val_t **v = (sw_val_t **)malloc(sizeof(sw_val_t *) * (c ? c : 1));
        for (int i = 0; i < c; i++) {
            k[i] = unmarsh_val(buf, len, pos);
            v[i] = unmarsh_val(buf, len, pos);
        }
        sw_val_t *r = sw_val_map_new(k, v, c);
        free(k); free(v);
        return r;
    }
    case SW_MARSHAL_PID: {
        if (*pos + 8 > len) return sw_val_nil();
        uint64_t id; memcpy(&id, buf + *pos, 8); *pos += 8;
        /* Reconstruct as a SW_VAL_REMOTE_PID tagged with the sending
         * node's name. `tls_unmarshal_default_node` is set by
         * sw_unmarshal at entry — see that function for why. If unset
         * (decode path that isn't a cross-node message), fall back to
         * nil; the user shouldn't be seeing pids in that case. */
        if (tls_unmarshal_default_node && tls_unmarshal_default_node[0])
            return sw_val_remote_pid(tls_unmarshal_default_node, id);
        return sw_val_nil();
    }
    default:
        return sw_val_nil();
    }
}

sw_val_t *sw_unmarshal(const uint8_t *buf, uint32_t len,
                       const char *default_remote_node) {
    uint32_t pos = 0;
    tls_unmarshal_default_node = default_remote_node;
    sw_val_t *r = unmarsh_val(buf, len, &pos);
    tls_unmarshal_default_node = NULL;
    return r;
}

const char *sw_node_local_name(void) {
    return g_node.active ? g_node.name : "";
}

/* Send dispatch — handles both local pids and remote pids. Local sends
 * use the existing scheduler mailbox path; remote sends marshal the
 * message and write to the peer's TCP connection.
 *
 * The codegen emit_send used to call sw_send_tagged directly on
 * `target->v.pid`, which crashes for SW_VAL_REMOTE_PID (no pid pointer)
 * and silently drops for non-pid types. This wrapper handles both. */
void sw_send_dispatch(sw_val_t *target, sw_val_t *msg) {
    if (!target) return;
    if (target->type == SW_VAL_PID) {
        sw_send_tagged(target->v.pid, SW_TAG_NONE, msg);
    } else if (target->type == SW_VAL_REMOTE_PID && target->v.rpid.node) {
        uint8_t *buf = NULL;
        uint32_t blen = 0;
        if (sw_marshal(msg, &buf, &blen) == 0) {
            sw_node_send_pid(target->v.rpid.node, target->v.rpid.id,
                             SW_TAG_NONE, buf, blen);
            free(buf);
        }
    }
    /* else: silent no-op for nil / wrong type. Same forgiveness as
     * the old direct sw_send_tagged path. */
}

/* === Internal Helpers === */

static sw_peer_t *find_peer(const char *name) {
    for (uint32_t i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peers[i].name, name) == 0) {
            return &g_peers[i];
        }
    }
    return NULL;
}

/* Locate the peer entry that owns this TCP connection. Used by
 * dist_handler to find where to append incoming bytes for framing.
 * Caller must hold g_dist_lock. */
static sw_peer_t *find_peer_by_conn_locked(sw_port_t *conn) {
    if (!conn) return NULL;
    for (uint32_t i = 0; i < g_peer_count; i++) {
        if (g_peers[i].conn == conn) return &g_peers[i];
    }
    return NULL;
}

/* Append bytes to a peer's rx buffer, growing as needed. Single-writer
 * (dist_handler) so no lock needed on the buffer fields themselves.
 *
 * Returns 0 on success, -1 if the append would exceed SW_NODE_MAX_FRAME,
 * overflow the size arithmetic, or hit an allocation failure. On -1 the
 * existing buffer is left intact (caller drops the connection's stream).
 * `data` is untrusted network input, so every size step is bounds-checked
 * before it can wrap. */
static int peer_rx_append(sw_peer_t *peer, const uint8_t *data, uint32_t len) {
    if (len == 0) return 0;
    /* Reject before `rx_len + len` can overflow uint32 or blow the cap.
     * Written as subtraction so the RHS can't wrap. */
    if (len > SW_NODE_MAX_FRAME - peer->rx_len) return -1;
    uint32_t need = peer->rx_len + len;
    if (need > peer->rx_cap) {
        uint32_t nc = peer->rx_cap ? peer->rx_cap : 1024;
        while (nc < need) {
            if (nc > SW_NODE_MAX_FRAME / 2) { nc = need; break; } /* no *2 overflow */
            nc *= 2;
        }
        uint8_t *nb = (uint8_t *)realloc(peer->rx_buf, nc);
        if (!nb) return -1; /* old buffer still valid; caller bails */
        peer->rx_buf = nb;
        peer->rx_cap = nc;
    }
    memcpy(peer->rx_buf + peer->rx_len, data, len);
    peer->rx_len += len;
    return 0;
}

/* Discard `consumed` bytes from the head of the rx buffer. */
static void peer_rx_consume(sw_peer_t *peer, uint32_t consumed) {
    if (consumed >= peer->rx_len) {
        peer->rx_len = 0;
        return;
    }
    memmove(peer->rx_buf, peer->rx_buf + consumed, peer->rx_len - consumed);
    peer->rx_len -= consumed;
}

/* Create a placeholder peer for a freshly-accepted incoming connection.
 * The name is filled in lazily by handle_remote_data when the first
 * framed message arrives (we don't know who connected to us until we
 * parse the sw_remote_msg_t header). Returns NULL if no slot available
 * — caller falls back to inline single-frame processing. */
static sw_peer_t *register_inbound_conn(sw_port_t *conn) {
    pthread_mutex_lock(&g_dist_lock);
    /* Already registered? */
    for (uint32_t i = 0; i < g_peer_count; i++) {
        if (g_peers[i].conn == conn) {
            pthread_mutex_unlock(&g_dist_lock);
            return &g_peers[i];
        }
    }
    if (g_peer_count >= SW_NODE_MAX_PEERS) {
        pthread_mutex_unlock(&g_dist_lock);
        return NULL;
    }
    sw_peer_t *p = &g_peers[g_peer_count++];
    memset(p, 0, sizeof(*p));
    p->conn = conn;
    p->connected = 1;
    pthread_mutex_unlock(&g_dist_lock);
    return p;
}

/* Serialize and send a remote message over TCP */
static int send_remote_msg(sw_port_t *conn, sw_remote_msg_t *hdr,
                           const void *payload, uint32_t payload_len) {
    uint32_t total = sizeof(sw_remote_msg_t) + payload_len;
    uint32_t net_len = htonl(total);

    /* Send length prefix */
    if (sw_tcp_send(conn, &net_len, 4) < 0) return -1;

    /* Send header */
    hdr->payload_len = payload_len;
    if (sw_tcp_send(conn, hdr, sizeof(sw_remote_msg_t)) < 0) return -1;

    /* Send payload */
    if (payload_len > 0 && payload) {
        if (sw_tcp_send(conn, payload, payload_len) < 0) return -1;
    }

    return 0;
}

/* Handle received data from a remote node */
static void handle_remote_data(uint8_t *data, uint32_t len, sw_port_t *conn) {
    if (len < sizeof(sw_remote_msg_t)) return;

    sw_remote_msg_t *hdr = (sw_remote_msg_t *)data;

    /* Auto-register the sender as a peer if this is the first time
     * we've seen it on this connection — happens when alpha connects
     * to beta (only alpha's side called node_connect, but beta needs
     * to know alpha as a peer to send replies via sw_node_send_pid).
     *
     * dist_handler creates a placeholder peer (name="") for every
     * incoming PORT_ACCEPT so the rx buffer has a home. The first
     * framed message gives us hdr->from_node — we promote the
     * placeholder to a named peer here. If we already have a named
     * peer for this name (rare; would require a reconnect or
     * crossed connect), keep the placeholder named for this conn so
     * find_peer_by_conn_locked still resolves it. */
    if (conn && hdr->from_node[0]) {
        pthread_mutex_lock(&g_dist_lock);
        sw_peer_t *placeholder = NULL;
        for (uint32_t i = 0; i < g_peer_count; i++) {
            if (g_peers[i].conn == conn && !g_peers[i].name[0]) {
                placeholder = &g_peers[i];
                break;
            }
        }
        if (placeholder) {
            strncpy(placeholder->name, hdr->from_node, SW_NODE_NAME_MAX - 1);
        } else {
            sw_peer_t *existing = NULL;
            for (uint32_t i = 0; i < g_peer_count; i++) {
                if (strcmp(g_peers[i].name, hdr->from_node) == 0) {
                    existing = &g_peers[i];
                    break;
                }
            }
            if (!existing && g_peer_count < SW_NODE_MAX_PEERS) {
                sw_peer_t *p = &g_peers[g_peer_count++];
                memset(p, 0, sizeof(*p));
                strncpy(p->name, hdr->from_node, SW_NODE_NAME_MAX - 1);
                p->conn = conn;
                p->connected = 1;
            } else if (existing && !existing->conn) {
                existing->conn = conn;
                existing->connected = 1;
            }
        }
        pthread_mutex_unlock(&g_dist_lock);
    }

    void *payload = NULL;
    uint32_t plen = hdr->payload_len;

    /* `plen` comes off the wire untrusted. Cap it, and bounds-check
     * against the bytes we actually hold using subtraction so
     * `sizeof(hdr) + plen` can't overflow (len >= sizeof guaranteed by
     * the early return above). */
    if (plen > 0 && plen <= SW_NODE_MAX_FRAME &&
        plen <= len - sizeof(sw_remote_msg_t)) {
        payload = malloc(plen);
        if (!payload) return; /* OOM — drop this frame rather than deref NULL */
        memcpy(payload, data + sizeof(sw_remote_msg_t), plen);
    }

    /* Route to local process — try PID first, then name */
    sw_process_t *target = NULL;
    if (hdr->to_pid > 0) {
        target = sw_find_by_pid(hdr->to_pid);
    }
    if (!target && hdr->to_name[0]) {
        target = sw_whereis(hdr->to_name);
    }

    if (target && payload) {
        /* Type-preserving unmarshal — round-trips tuples-as-tuples and
         * atoms-as-atoms, so user-side `receive { {'echo', ...} -> }`
         * patterns actually match. Passes hdr->from_node so PID bytes
         * decode as SW_VAL_REMOTE_PID tagged with the sender — that
         * lets `send(from, reply)` route back through TCP. */
        sw_val_t *msg = sw_unmarshal((const uint8_t *)payload, plen,
                                     hdr->from_node);
        free(payload);
        sw_send_tagged(target, hdr->tag, msg);
    } else if (target) {
        sw_send_tagged(target, hdr->tag, NULL);
    } else {
        if (payload) free(payload);
    }
}

/* Distribution handler process — handles incoming connections and data */
static void dist_handler(void *arg) {
    (void)arg;
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    while (1) {
        uint64_t tag = 0;
        void *msg = sw_receive_any((uint64_t)-1, &tag);

        if (!msg && tag == 0) break; /* Killed */

        if (tag == SW_TAG_PORT_ACCEPT) {
            /* New incoming connection from a remote node */
            sw_port_accept_t *acc = (sw_port_accept_t *)msg;
            /* Transfer conn ownership to this process */
            sw_port_controlling_process(acc->conn, sw_self());
            /* Pre-register a placeholder peer keyed by conn so PORT_DATA
             * has somewhere to buffer bytes. handle_remote_data fills in
             * the peer name on the first complete frame. */
            register_inbound_conn(acc->conn);
            free(msg);

        } else if (tag == SW_TAG_PORT_DATA) {
            sw_port_data_t *data = (sw_port_data_t *)msg;

            pthread_mutex_lock(&g_dist_lock);
            sw_peer_t *peer = find_peer_by_conn_locked(data->port);
            pthread_mutex_unlock(&g_dist_lock);

            if (peer) {
                /* Append to peer rx buffer and drain every complete
                 * length-prefixed frame. TCP is a byte stream so the
                 * kernel can hand us multiple frames in one read, or a
                 * frame split across reads. */
                if (peer_rx_append(peer, data->data, data->len) != 0) {
                    /* Oversized / overflowing / OOM append — this stream
                     * is abusive or we're out of memory. Reset the rx
                     * buffer so we resync on the next frame boundary
                     * rather than corrupting state. */
                    peer->rx_len = 0;
                } else {
                    while (peer->rx_len >= 4) {
                        uint32_t msg_len = ntohl(*(uint32_t *)peer->rx_buf);
                        /* Reject a frame larger than the ceiling before it
                         * can wedge the buffer (we'll never accumulate it).
                         * Drop the stream — a valid peer never sends this. */
                        if (msg_len > SW_NODE_MAX_FRAME) { peer->rx_len = 0; break; }
                        /* `rx_len - 4` can't underflow (loop guard >= 4);
                         * compare via subtraction so `4 + msg_len` never
                         * overflows. */
                        if (msg_len > peer->rx_len - 4) break;
                        handle_remote_data(peer->rx_buf + 4, msg_len, data->port);
                        peer_rx_consume(peer, 4 + msg_len);
                    }
                }
            } else {
                /* No peer slot (max peers reached) — degrade to the
                 * legacy single-frame path so at least the first frame
                 * isn't dropped. */
                if (data->len >= 4) {
                    uint32_t msg_len = ntohl(*(uint32_t *)data->data);
                    if (msg_len <= SW_NODE_MAX_FRAME &&
                        msg_len <= data->len - 4) {
                        handle_remote_data(data->data + 4, msg_len, data->port);
                    }
                }
            }
            free(data->data);
            free(data);

        } else if (tag == SW_TAG_PORT_CLOSED) {
            if (msg) free(msg);

        } else if (tag == SW_TAG_STOP) {
            if (msg) free(msg);
            break;

        } else {
            if (msg) free(msg);
        }
    }
}

/* === Public API === */

int sw_node_start(const char *name, uint16_t port) {
    if (!name) return -1;

    memset(&g_node, 0, sizeof(g_node));
    strncpy(g_node.name, name, SW_NODE_NAME_MAX - 1);
    g_node.port = port;
    strcpy(g_node.host, "0.0.0.0");

    /* Start distribution handler process */
    g_dist_proc = sw_spawn(dist_handler, NULL);
    if (!g_dist_proc) return -1;

    /* Give it a moment to start */
    usleep(10000);

    /* Start TCP listener owned by dist handler */
    /* We need to listen from the dist_handler context. For simplicity,
     * listen from here and transfer ownership. */
    g_node.listener = sw_tcp_listen("0.0.0.0", port);
    if (!g_node.listener) return -1;

    sw_port_controlling_process(g_node.listener, g_dist_proc);
    g_node.active = 1;

    return 0;
}

void sw_node_stop(void) {
    if (!g_node.active) return;

    if (g_dist_proc) {
        sw_send_tagged(g_dist_proc, SW_TAG_STOP, NULL);
    }

    /* Close all peer connections */
    pthread_mutex_lock(&g_dist_lock);
    for (uint32_t i = 0; i < g_peer_count; i++) {
        if (g_peers[i].conn) {
            sw_port_close(g_peers[i].conn);
        }
        if (g_peers[i].rx_buf) {
            free(g_peers[i].rx_buf);
            g_peers[i].rx_buf = NULL;
            g_peers[i].rx_len = 0;
            g_peers[i].rx_cap = 0;
        }
    }
    g_peer_count = 0;
    pthread_mutex_unlock(&g_dist_lock);

    if (g_node.listener) {
        sw_port_close(g_node.listener);
        g_node.listener = NULL;
    }

    g_node.active = 0;
}

const char *sw_node_name(void) {
    return g_node.name;
}

int sw_node_connect(const char *name, const char *host, uint16_t port) {
    if (!name || !host) return -1;

    pthread_mutex_lock(&g_dist_lock);

    if (find_peer(name)) {
        pthread_mutex_unlock(&g_dist_lock);
        return 0; /* Already connected */
    }

    if (g_peer_count >= SW_NODE_MAX_PEERS) {
        pthread_mutex_unlock(&g_dist_lock);
        return -1;
    }

    pthread_mutex_unlock(&g_dist_lock);

    /* Connect (blocking) */
    sw_port_t *conn = sw_tcp_connect(host, port);
    if (!conn) return -1;

    /* Transfer ownership to dist handler so it receives data */
    sw_port_controlling_process(conn, g_dist_proc);

    pthread_mutex_lock(&g_dist_lock);
    sw_peer_t *peer = &g_peers[g_peer_count++];
    memset(peer, 0, sizeof(*peer));
    strncpy(peer->name, name, SW_NODE_NAME_MAX - 1);
    strncpy(peer->host, host, 63);
    peer->port = port;
    peer->conn = conn;
    peer->connected = 1;
    pthread_mutex_unlock(&g_dist_lock);

    return 0;
}

int sw_node_disconnect(const char *name) {
    if (!name) return -1;

    pthread_mutex_lock(&g_dist_lock);
    sw_peer_t *peer = find_peer(name);
    if (!peer) {
        pthread_mutex_unlock(&g_dist_lock);
        return -1;
    }

    if (peer->conn) {
        sw_port_close(peer->conn);
        peer->conn = NULL;
    }
    peer->connected = 0;
    if (peer->rx_buf) {
        free(peer->rx_buf);
        peer->rx_buf = NULL;
        peer->rx_len = 0;
        peer->rx_cap = 0;
    }

    pthread_mutex_unlock(&g_dist_lock);
    return 0;
}

int sw_node_send(const char *node, const char *name, uint64_t tag,
                 const void *data, uint32_t len) {
    if (!node || !name) return -1;

    pthread_mutex_lock(&g_dist_lock);
    sw_peer_t *peer = find_peer(node);
    if (!peer || !peer->connected || !peer->conn) {
        pthread_mutex_unlock(&g_dist_lock);
        return -1;
    }

    sw_remote_msg_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.from_node, g_node.name, SW_NODE_NAME_MAX - 1);
    hdr.from_pid = sw_self() ? sw_self()->pid : 0;
    strncpy(hdr.to_node, node, SW_NODE_NAME_MAX - 1);
    hdr.to_pid = 0;
    strncpy(hdr.to_name, name, SW_REG_NAME_MAX - 1);
    hdr.tag = tag;

    int result = send_remote_msg(peer->conn, &hdr, data, len);
    pthread_mutex_unlock(&g_dist_lock);

    return result;
}

int sw_node_send_pid(const char *node, uint64_t pid, uint64_t tag,
                     const void *data, uint32_t len) {
    if (!node) return -1;

    pthread_mutex_lock(&g_dist_lock);
    sw_peer_t *peer = find_peer(node);
    if (!peer || !peer->connected || !peer->conn) {
        pthread_mutex_unlock(&g_dist_lock);
        return -1;
    }

    sw_remote_msg_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.from_node, g_node.name, SW_NODE_NAME_MAX - 1);
    hdr.from_pid = sw_self() ? sw_self()->pid : 0;
    strncpy(hdr.to_node, node, SW_NODE_NAME_MAX - 1);
    hdr.to_pid = pid;
    hdr.to_name[0] = '\0';
    hdr.tag = tag;

    int result = send_remote_msg(peer->conn, &hdr, data, len);
    pthread_mutex_unlock(&g_dist_lock);

    return result;
}

int sw_node_peers(char names[][SW_NODE_NAME_MAX], int max) {
    pthread_mutex_lock(&g_dist_lock);
    int count = 0;
    for (uint32_t i = 0; i < g_peer_count && count < max; i++) {
        if (g_peers[i].connected) {
            strncpy(names[count], g_peers[i].name, SW_NODE_NAME_MAX - 1);
            names[count][SW_NODE_NAME_MAX - 1] = '\0';
            count++;
        }
    }
    pthread_mutex_unlock(&g_dist_lock);
    return count;
}

int sw_node_is_connected(const char *name) {
    if (!name) return 0;
    pthread_mutex_lock(&g_dist_lock);
    sw_peer_t *peer = find_peer(name);
    int connected = (peer && peer->connected);
    pthread_mutex_unlock(&g_dist_lock);
    return connected;
}
