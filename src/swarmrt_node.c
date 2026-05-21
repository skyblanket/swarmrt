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
 *   0x08  pid    (8 bytes pid id — only meaningful on local node; cross-
 *                 node pid routing isn't wired yet — see KNOWN_ISSUES)
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
        /* Local pid id only — cross-node pid routing not implemented.
         * Reply paths should embed node_name() in the message instead. */
        marsh_grow(buf, cap, *pos + 1 + 8);
        (*buf)[(*pos)++] = SW_MARSHAL_PID;
        uint64_t pid = v->v.pid ? v->v.pid->pid : 0;
        memcpy(*buf + *pos, &pid, 8); *pos += 8;
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
        /* Drop the pid id — no cross-node pid routing yet. Return nil. */
        *pos += 8;
        return sw_val_nil();
    }
    default:
        return sw_val_nil();
    }
}

sw_val_t *sw_unmarshal(const uint8_t *buf, uint32_t len) {
    uint32_t pos = 0;
    return unmarsh_val(buf, len, &pos);
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
static void handle_remote_data(uint8_t *data, uint32_t len) {
    if (len < sizeof(sw_remote_msg_t)) return;

    sw_remote_msg_t *hdr = (sw_remote_msg_t *)data;
    void *payload = NULL;
    uint32_t plen = hdr->payload_len;

    if (plen > 0 && len >= sizeof(sw_remote_msg_t) + plen) {
        payload = malloc(plen);
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
         * patterns actually match. The previous JSON path lost both
         * distinctions. */
        sw_val_t *msg = sw_unmarshal((const uint8_t *)payload, plen);
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
            free(msg);

        } else if (tag == SW_TAG_PORT_DATA) {
            sw_port_data_t *data = (sw_port_data_t *)msg;

            /* Check if this looks like a framed message (length-prefixed) */
            if (data->len >= 4) {
                uint32_t msg_len = ntohl(*(uint32_t *)data->data);
                if (data->len >= 4 + msg_len) {
                    handle_remote_data(data->data + 4, msg_len);
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
