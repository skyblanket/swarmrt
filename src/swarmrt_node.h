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

#define SW_NODE_NAME_MAX 64
#define SW_NODE_MAX_PEERS 32
#define SW_TAG_REMOTE_MSG 16

/* Node handle */
typedef struct sw_node {
    char name[SW_NODE_NAME_MAX];
    char host[64];
    uint16_t port;
    sw_port_t *listener;        /* TCP listener for incoming connections */
    int active;
} sw_node_t;

/* Peer (remote node) connection */
typedef struct sw_peer {
    char name[SW_NODE_NAME_MAX];
    char host[64];
    uint16_t port;
    sw_port_t *conn;            /* TCP connection to peer */
    int connected;
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
