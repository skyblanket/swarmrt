/*
 * SwarmRT Phase 9: Node & Distribution
 *
 * Multi-node messaging over TCP. Location-transparent process addressing.
 *
 * Node:       Named runtime instance with a TCP listener for inter-node traffic.
 * Remote PID: {node_name, pid} pairs for cross-node messaging.
 * Distribution: Automatic connection + message routing between nodes.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_NODE_H
#define SWARMRT_NODE_H

#include "swarmrt_native.h"
#include "swarmrt_io.h"
#include "swarmrt_lang.h"

/* Type-preserving marshal/unmarshal — used by node_send/handle_remote_data
 * and exposed for any code that needs to serialize an sw_val_t over the
 * wire without losing the tuple-vs-list / atom-vs-string distinction.
 * See marsh_val in swarmrt_node.c for the on-wire layout.
 * `default_remote_node` is the node name used to reconstruct SW_VAL_PID
 * payloads as SW_VAL_REMOTE_PID values on the receiving side (so they
 * route back via TCP when used as a `send()` target). Pass NULL when
 * decoding a payload that was generated locally. */
int sw_marshal(sw_val_t *v, uint8_t **out, uint32_t *out_len);
sw_val_t *sw_unmarshal(const uint8_t *buf, uint32_t len,
                       const char *default_remote_node);

/* Dispatch a send() — local SW_VAL_PID goes through sw_send_tagged;
 * SW_VAL_REMOTE_PID goes through sw_node_send_pid (marshals the message
 * and writes to the peer's TCP connection). Anything else is a no-op.
 * Used by the codegen emit_send so user code doesn't have to know
 * whether the target is local or remote. */
void sw_send_dispatch(sw_val_t *target, sw_val_t *msg);

/* The name passed to sw_node_start (e.g. "alpha@127.0.0.1"). Returns an
 * empty string if no node has been started. Used by sw_marshal so the
 * receiver can reconstruct local pids as remote pids tagged with the
 * sending node's name. */
const char *sw_node_local_name(void);

#define SW_NODE_NAME_MAX 64
#define SW_NODE_MAX_PEERS 32
#define SW_TAG_REMOTE_MSG 16

/* Hard ceiling on a single inbound distribution frame (length prefix +
 * header + payload). A peer is untrusted network input: without this an
 * adversary can send a 4-byte length prefix claiming ~4 GB and force the
 * rx buffer to grow without bound (memory-exhaustion DoS), or send a
 * length near UINT32_MAX to overflow `4 + msg_len` / `sizeof(hdr)+plen`.
 * 64 MB is far above any legitimate marshalled message. */
#define SW_NODE_MAX_FRAME (64u * 1024u * 1024u)

/* Node handle */
typedef struct sw_node {
    char name[SW_NODE_NAME_MAX];
    char host[64];
    uint16_t port;
    sw_port_t *listener;        /* TCP listener for incoming connections */
    int active;
} sw_node_t;

/* Peer (remote node) connection.
 *
 * `rx_buf` accumulates bytes from the TCP byte stream so dist_handler
 * can drain whatever framed messages are complete on each PORT_DATA
 * event. TCP coalesces small writes — without this buffer the handler
 * would consume one frame per read and silently drop the rest. See
 * dist_handler in swarmrt_node.c. */
typedef struct sw_peer {
    char name[SW_NODE_NAME_MAX];
    char host[64];
    uint16_t port;
    sw_port_t *conn;            /* TCP connection to peer */
    int connected;
    uint8_t *rx_buf;
    uint32_t rx_len;
    uint32_t rx_cap;
} sw_peer_t;

/* Remote message envelope (serialized over TCP) */
typedef struct {
    char from_node[SW_NODE_NAME_MAX];
    uint64_t from_pid;
    char to_node[SW_NODE_NAME_MAX];
    uint64_t to_pid;            /* 0 = look up by name */
    char to_name[SW_REG_NAME_MAX]; /* Registry name (if to_pid == 0) */
    uint64_t tag;
    uint32_t payload_len;
    /* payload bytes follow */
} sw_remote_msg_t;

/* === Node Lifecycle === */

/* Start this node with a name and listen port */
int sw_node_start(const char *name, uint16_t port);
void sw_node_stop(void);

/* Get this node's name */
const char *sw_node_name(void);

/* === Distribution === */

/* Connect to a remote node */
int sw_node_connect(const char *name, const char *host, uint16_t port);

/* Disconnect from a remote node */
int sw_node_disconnect(const char *name);

/* Send a message to a process on a remote node (by name) */
int sw_node_send(const char *node, const char *name, uint64_t tag,
                 const void *data, uint32_t len);

/* Send a message to a process on a remote node (by PID) */
int sw_node_send_pid(const char *node, uint64_t pid, uint64_t tag,
                     const void *data, uint32_t len);

/* List connected peers */
int sw_node_peers(char names[][SW_NODE_NAME_MAX], int max);

/* Check if a node is connected */
int sw_node_is_connected(const char *name);

#endif /* SWARMRT_NODE_H */
