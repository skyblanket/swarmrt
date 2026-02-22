/*
 * SwarmRT Phase 6: IO System (kqueue + TCP)
 *
 * Ports:     OS resources (sockets, pipes) as process-like handles.
 * IO Loop:   kqueue-driven async IO on a dedicated thread.
 * TCP:       Listen, accept, send, recv — all async via ports.
 *
 * Every port is owned by a process. Events (data ready, connection accepted,
 * closed) are delivered as tagged messages to the owner.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_IO_H
#define SWARMRT_IO_H

#include "swarmrt_native.h"

/* Port types */
typedef enum {
    SW_PORT_TCP_LISTEN,
    SW_PORT_TCP_CONN,
    SW_PORT_PIPE,
} sw_port_type_t;

/* Port state */
typedef enum {
    SW_PORT_OPEN,
    SW_PORT_CLOSING,
    SW_PORT_CLOSED,
} sw_port_state_t;

/* Tags for port messages */
#define SW_TAG_PORT_DATA     20
#define SW_TAG_PORT_ACCEPT   21
#define SW_TAG_PORT_CLOSED   22
#define SW_TAG_PORT_ERROR    23

/* Port handle */
typedef struct sw_port {
    int fd;
    sw_port_type_t type;
    sw_port_state_t state;
    sw_process_t *owner;
    uint32_t id;

    /* Read buffer */
    uint8_t *recv_buf;
    uint32_t recv_buf_size;

    struct sw_port *next;   /* Global port list */
} sw_port_t;

/* Data message sent to owner */
typedef struct {
    sw_port_t *port;
    uint8_t *data;
    uint32_t len;
} sw_port_data_t;

/* Accept message sent to listener owner */
typedef struct {
    sw_port_t *listener;
    sw_port_t *conn;       /* New connection port */
} sw_port_accept_t;

/* Error/close message */
typedef struct {
    sw_port_t *port;
    int error;
} sw_port_event_t;

/* === IO System Lifecycle === */
int  sw_io_init(void);
void sw_io_shutdown(void);

/* === TCP API === */

/* Listen on addr:port. Returns a listener port owned by caller.
 * Accepted connections are sent as SW_TAG_PORT_ACCEPT messages. */
sw_port_t *sw_tcp_listen(const char *addr, uint16_t port);

/* Connect to addr:port. Returns connection port owned by caller. */
sw_port_t *sw_tcp_connect(const char *addr, uint16_t port);

/* Send data on a connection port. Non-blocking, copies data. */
int sw_tcp_send(sw_port_t *port, const void *data, uint32_t len);

/* Set active mode: port sends SW_TAG_PORT_DATA on incoming data.
 * active=1: continuous (default), active=0: manual sw_tcp_recv needed. */
void sw_port_set_active(sw_port_t *port, int active);

/* Close a port. Sends SW_TAG_PORT_CLOSED to owner. */
void sw_port_close(sw_port_t *port);

/* Transfer port ownership to another process. */
void sw_port_controlling_process(sw_port_t *port, sw_process_t *new_owner);

/* Called by process_exit to close owned ports */
void sw_io_cleanup_owner(sw_process_t *proc);

#endif /* SWARMRT_IO_H */
