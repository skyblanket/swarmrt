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

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
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
        /* Deserialize JSON payload back to sw_val_t */
        extern sw_val_t *sw_val_nil(void);
        extern sw_val_t *sw_val_string(const char *s);
        const char *p = (const char *)payload;
        /* Try JSON parse — if it looks like JSON, parse it */
        sw_val_t *msg = NULL;
        if (p[0] == '{' || p[0] == '[' || p[0] == '"' ||
            (p[0] >= '0' && p[0] <= '9') || p[0] == '-' ||
            p[0] == 't' || p[0] == 'f' || p[0] == 'n') {
            /* Forward-declare json_decode from lang */
            extern sw_val_t *sw_lang_json_decode(const char *s);
            msg = sw_lang_json_decode(p);
        }
        if (!msg) msg = sw_val_string(p);
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
