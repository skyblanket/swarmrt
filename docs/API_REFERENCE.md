# SwarmRT API Reference

Complete API reference for the SwarmRT runtime. All functions, types, and constants
organized by module.

---

## Table of Contents

1. [Core Runtime](#1-core-runtime)
2. [GenServer](#2-genserver)
3. [Supervisor](#3-supervisor)
4. [Task](#4-task)
5. [ETS](#5-ets)
6. [Agent](#6-agent)
7. [Application](#7-application)
8. [DynamicSupervisor](#8-dynamicsupervisor)
9. [GenStateMachine](#9-genstatemachine)
10. [Process Groups](#10-process-groups)
11. [IO / Ports](#11-io--ports)
12. [Hot Code Reload](#12-hot-code-reload)
13. [Garbage Collection](#13-garbage-collection)
14. [Distribution](#14-distribution)
15. [HTTP / WebSocket](#15-http--websocket)
16. [Language Frontend](#16-language-frontend)
17. [Codegen](#17-codegen)
18. [Constants & Tags](#18-constants--tags)

---

## 1. Core Runtime

**Header:** `swarmrt_native.h`

The core scheduler, process model, message passing, and memory management.

### Lifecycle

```c
int sw_init(const char *name, uint32_t num_schedulers);
```
Initialize the SwarmRT runtime. Creates `num_schedulers` OS threads for the scheduler pool.
Returns a swarm ID (>= 0) on success, -1 on failure.

- **name** — Name for this runtime instance.
- **num_schedulers** — Number of scheduler threads (typically = CPU cores).

```c
void sw_shutdown(int swarm_id);
```
Shut down a running SwarmRT instance and free all resources.

### Process Management

```c
sw_process_t *sw_spawn(void (*func)(void*), void *arg);
```
Spawn a new lightweight process running `func(arg)`. Returns a process pointer.

```c
sw_process_t *sw_spawn_opts(void (*func)(void*), void *arg, sw_priority_t prio);
```
Spawn with explicit priority (`SW_PRIO_MAX`, `SW_PRIO_HIGH`, `SW_PRIO_NORMAL`, `SW_PRIO_LOW`).

```c
sw_process_t *sw_spawn_link(void (*func)(void*), void *arg);
```
Spawn and atomically link to the calling process.

```c
void sw_yield(void);
```
Voluntarily yield the current time slice to the scheduler.

```c
void sw_exit(sw_process_t *proc);
```
Terminate a process normally.

```c
void sw_process_kill(sw_process_t *proc, int reason);
```
Kill a process with a reason code. Linked processes receive exit signals.

### Process Info

```c
sw_process_t *sw_self(void);
```
Return the current process pointer.

```c
uint64_t sw_getpid(void);
```
Return the current process PID (integer).

```c
sw_proc_state_t sw_get_state(sw_process_t *proc);
```
Get a process's current state. States:

| State | Description |
|-------|-------------|
| `SW_PROC_FREE` | Slot available |
| `SW_PROC_RUNNABLE` | Ready to run |
| `SW_PROC_RUNNING` | Currently executing |
| `SW_PROC_WAITING` | Blocked on receive |
| `SW_PROC_SUSPENDED` | Explicitly suspended |
| `SW_PROC_EXITING` | In exit cleanup |
| `SW_PROC_GARBING` | Being garbage collected |

```c
sw_process_t *sw_find_by_pid(uint64_t pid);
```
Look up a process by its integer PID. Returns NULL if not found.

### Message Passing

```c
void sw_send(sw_process_t *to, void *msg);
```
Send an untagged message to a process.

```c
void sw_send_tagged(sw_process_t *to, uint64_t tag, void *msg);
```
Send a tagged message. Tags enable selective receive.

```c
void *sw_receive(uint64_t timeout_ms);
```
Block until a message arrives or timeout expires. Returns message payload or NULL on timeout.

```c
void *sw_receive_nowait(void);
```
Non-blocking receive. Returns NULL if mailbox is empty.

```c
void *sw_receive_tagged(uint64_t tag, uint64_t timeout_ms);
```
Selectively receive a message matching `tag`.

```c
void *sw_receive_any(uint64_t timeout_ms, uint64_t *out_tag);
```
Receive any message, writing its tag to `out_tag`.

### Selective Receive (Advanced)

```c
void sw_mailbox_drain_signals(void);
```
Move signals from the lock-free signal queue to the private mailbox.

```c
sw_msg_t *sw_mailbox_peek(void);
```
Peek at the first message without consuming it.

```c
void sw_mailbox_remove_msg(sw_msg_t *m);
```
Remove a specific message from the mailbox.

```c
int sw_mailbox_wait_new(uint64_t timeout_ms);
```
Wait for new messages to arrive. Returns 1 if new messages, 0 on timeout.

### Links & Monitors

```c
int sw_link(sw_process_t *other);
```
Create a bidirectional link. When either process exits, the other receives an exit signal.

```c
int sw_unlink(sw_process_t *other);
```
Remove a link.

```c
uint64_t sw_monitor(sw_process_t *target);
```
Monitor a process. Returns a reference. When target exits, caller receives a DOWN message.

```c
int sw_demonitor(uint64_t ref);
```
Cancel a monitor.

```c
void sw_process_flag(uint32_t flag, int value);
```
Set process flags. Use `SW_FLAG_TRAP_EXIT` to convert exit signals to messages.

### Registry

```c
int sw_register(const char *name, sw_process_t *proc);
```
Register a process under a name. Returns 0 on success.

```c
int sw_unregister(const char *name);
```
Remove a name registration.

```c
sw_process_t *sw_whereis(const char *name);
```
Look up a process by registered name. Returns NULL if not found.

```c
int sw_send_named(const char *name, uint64_t tag, void *msg);
```
Send a tagged message to a named process.

### Timers

```c
uint64_t sw_send_after(uint64_t delay_ms, sw_process_t *dest, uint64_t tag, void *msg);
```
Schedule a message delivery after `delay_ms`. Returns a timer reference.

```c
int sw_cancel_timer(uint64_t ref);
```
Cancel a pending timer.

### Statistics

```c
void sw_stats(int swarm_id);
```
Print runtime statistics to stdout.

```c
uint64_t sw_process_count(int swarm_id);
```
Return the number of active processes.

### Arena

```c
int sw_arena_init(sw_arena_t *arena, uint32_t max_procs);
```
Initialize the process arena with capacity for `max_procs` processes.

---

## 2. GenServer

**Header:** `swarmrt_otp.h`

OTP-style generic server behavior with call/cast semantics.

### Callbacks

```c
typedef struct {
    void *(*init)(void *arg);
    sw_call_reply_t (*handle_call)(void *state, sw_process_t *from, void *request);
    void *(*handle_cast)(void *state, void *message);
    void *(*handle_info)(void *state, uint64_t tag, void *info);
    void (*terminate)(void *state, int reason);
} sw_gs_callbacks_t;
```

- **init** — Called on startup. Returns initial state.
- **handle_call** — Synchronous request handler. Returns `{reply, new_state}`.
- **handle_cast** — Asynchronous message handler. Returns new state.
- **handle_info** — Handles non-call/cast messages. Returns new state.
- **terminate** — Cleanup on shutdown.

```c
typedef struct {
    void *reply;
    void *new_state;
} sw_call_reply_t;
```

### Server Lifecycle

```c
sw_process_t *sw_genserver_start(const char *name, sw_gs_callbacks_t *cbs, void *init_arg);
```
Start a named GenServer. Not linked to caller.

```c
sw_process_t *sw_genserver_start_link(const char *name, sw_gs_callbacks_t *cbs, void *init_arg);
```
Start and link to the calling process.

```c
void sw_genserver_stop(const char *name);
```
Stop a running GenServer by name.

### Client API

```c
void *sw_call(const char *name, void *request, uint64_t timeout_ms);
```
Synchronous call by name. Blocks until reply or timeout.

```c
void *sw_call_proc(sw_process_t *server, void *request, uint64_t timeout_ms);
```
Synchronous call by process pointer.

```c
void sw_cast(const char *name, void *message);
```
Asynchronous cast by name. Fire and forget.

```c
void sw_cast_proc(sw_process_t *server, void *message);
```
Asynchronous cast by process pointer.

---

## 3. Supervisor

**Header:** `swarmrt_otp.h`

Fault-tolerant supervision trees.

### Restart Strategies

| Strategy | Behavior |
|----------|----------|
| `SW_ONE_FOR_ONE` | Only restart the failed child |
| `SW_ONE_FOR_ALL` | Restart all children when one fails |
| `SW_REST_FOR_ONE` | Restart the failed child and all children started after it |

### Child Restart Types

| Type | Behavior |
|------|----------|
| `SW_PERMANENT` | Always restart |
| `SW_TEMPORARY` | Never restart |
| `SW_TRANSIENT` | Restart only on abnormal exit |

### Types

```c
typedef struct {
    char name[SW_REG_NAME_MAX];
    void (*start_func)(void*);
    void *start_arg;
    sw_child_restart_t restart;
} sw_child_spec_t;

typedef struct {
    sw_restart_strategy_t strategy;
    uint32_t max_restarts;
    uint32_t max_seconds;
    sw_child_spec_t *children;
    uint32_t num_children;
} sw_sup_spec_t;
```

### Functions

```c
sw_process_t *sw_supervisor_start(const char *name, sw_sup_spec_t *spec);
```
Start a named supervisor. Not linked to caller.

```c
sw_process_t *sw_supervisor_start_link(const char *name, sw_sup_spec_t *spec);
```
Start and link to the calling process.

---

## 4. Task

**Header:** `swarmrt_task.h`

Async/await style parallel computation.

### Types

```c
typedef struct {
    sw_process_t *child;
    uint64_t monitor_ref;
    int completed;
} sw_task_t;

typedef struct {
    sw_task_status_t status;  // SW_TASK_OK, SW_TASK_CRASH, SW_TASK_TIMEOUT
    void *value;
} sw_task_result_t;
```

### Functions

```c
sw_task_t sw_task_async(void *(*func)(void *), void *arg);
```
Spawn a linked task that runs `func(arg)`. Returns a task handle.

```c
sw_task_result_t sw_task_await(sw_task_t *task, uint64_t timeout_ms);
```
Wait for a task to complete. Returns status and result value.

```c
sw_task_result_t sw_task_yield(sw_task_t *task);
```
Non-blocking check. Returns immediately with current status.

```c
void sw_task_shutdown(sw_task_t *task);
```
Kill a running task.

---

## 5. ETS

**Header:** `swarmrt_ets.h`

Concurrent in-memory key-value tables.

### Types

```c
typedef uint32_t sw_ets_tid_t;

typedef struct {
    sw_ets_type_t type;     // SW_ETS_SET, SW_ETS_ORDERED_SET, SW_ETS_BAG
    sw_ets_access_t access; // SW_ETS_PUBLIC, SW_ETS_PROTECTED, SW_ETS_PRIVATE
} sw_ets_opts_t;
```

### Functions

```c
sw_ets_tid_t sw_ets_new(sw_ets_opts_t opts);
```
Create a new table. Use `SW_ETS_DEFAULT` for a public set.

```c
int sw_ets_insert(sw_ets_tid_t tid, void *key, void *value);
```
Insert or update a key-value pair.

```c
void *sw_ets_lookup(sw_ets_tid_t tid, void *key);
```
Look up a value by key. Returns NULL if not found.

```c
int sw_ets_delete(sw_ets_tid_t tid, void *key);
```
Delete a key-value pair.

```c
int sw_ets_drop(sw_ets_tid_t tid);
```
Delete an entire table.

```c
int sw_ets_info_count(sw_ets_tid_t tid);
```
Return the number of entries in a table.

```c
int sw_ets_list_tids(uint32_t *out, int max);
```
List all active table IDs. Returns count written to `out`.

```c
void sw_ets_cleanup_owner(sw_process_t *proc);
```
Internal: clean up tables owned by a dying process.

---

## 6. Agent

**Header:** `swarmrt_phase4.h`

Simple state-holding processes (like Elixir's Agent).

### Types

```c
typedef void *(*sw_agent_get_fn)(void *state, void *arg);
typedef void *(*sw_agent_update_fn)(void *state, void *arg);

typedef struct {
    void *reply;
    void *new_state;
} sw_agent_gau_result_t;

typedef sw_agent_gau_result_t (*sw_agent_gau_fn)(void *state, void *arg);
```

### Functions

```c
sw_process_t *sw_agent_start(const char *name, void *initial_state);
sw_process_t *sw_agent_start_link(const char *name, void *initial_state);
```
Start an agent with initial state.

```c
void sw_agent_stop(const char *name);
void sw_agent_stop_proc(sw_process_t *agent);
```
Stop an agent.

```c
void *sw_agent_get(const char *name, sw_agent_get_fn func, void *arg, uint64_t timeout_ms);
void *sw_agent_get_proc(sw_process_t *agent, sw_agent_get_fn func, void *arg, uint64_t timeout_ms);
```
Read agent state via a callback function. The callback receives current state and arg, returns a derived value.

```c
int sw_agent_update(const char *name, sw_agent_update_fn func, void *arg);
int sw_agent_update_proc(sw_process_t *agent, sw_agent_update_fn func, void *arg);
```
Update agent state. The callback returns the new state.

```c
void *sw_agent_get_and_update(const char *name, sw_agent_gau_fn func, void *arg, uint64_t timeout_ms);
void *sw_agent_get_and_update_proc(sw_process_t *agent, sw_agent_gau_fn func, void *arg, uint64_t timeout_ms);
```
Atomically read and update state. Returns the reply value.

---

## 7. Application

**Header:** `swarmrt_phase4.h`

Application lifecycle management with supervision.

### Types

```c
typedef struct {
    const char *name;
    sw_child_spec_t *children;
    uint32_t num_children;
    sw_restart_strategy_t strategy;
    uint32_t max_restarts;
    uint32_t max_seconds;
} sw_app_spec_t;
```

### Functions

```c
sw_process_t *sw_app_start(sw_app_spec_t *spec);
```
Start an application. Creates a top-level supervisor with the given child specs.

```c
void sw_app_stop(const char *name);
```
Stop a running application and all its children.

```c
sw_process_t *sw_app_get_supervisor(const char *name);
```
Get the application's root supervisor process.

---

## 8. DynamicSupervisor

**Header:** `swarmrt_phase4.h`

Supervisor that starts children dynamically at runtime.

### Types

```c
typedef struct {
    uint32_t max_restarts;
    uint32_t max_seconds;
    uint32_t max_children;  // max SW_DYNSUP_MAX_CHILDREN (4096)
} sw_dynsup_spec_t;
```

### Functions

```c
sw_process_t *sw_dynsup_start(const char *name, sw_dynsup_spec_t *spec);
sw_process_t *sw_dynsup_start_link(const char *name, sw_dynsup_spec_t *spec);
```
Start a dynamic supervisor.

```c
sw_process_t *sw_dynsup_start_child(const char *sup_name, sw_child_spec_t *child_spec);
sw_process_t *sw_dynsup_start_child_proc(sw_process_t *sup, sw_child_spec_t *child_spec);
```
Start a child under the dynamic supervisor.

```c
int sw_dynsup_terminate_child(const char *sup_name, sw_process_t *child);
int sw_dynsup_terminate_child_proc(sw_process_t *sup, sw_process_t *child);
```
Terminate a specific child.

```c
uint32_t sw_dynsup_count_children(const char *sup_name);
uint32_t sw_dynsup_count_children_proc(sw_process_t *sup);
```
Count active children.

---

## 9. GenStateMachine

**Header:** `swarmrt_phase5.h`

Finite state machine behavior.

### Event Types

| Type | Source |
|------|--------|
| `SW_SM_CALL` | Synchronous call |
| `SW_SM_CAST` | Asynchronous cast |
| `SW_SM_INFO` | General message |
| `SW_SM_TIMEOUT` | State timeout |

### Action Results

| Action | Behavior |
|--------|----------|
| `SW_SM_NEXT_STATE` | Transition to a new state |
| `SW_SM_KEEP_STATE` | Stay in current state |
| `SW_SM_STOP` | Stop the state machine |

### Types

```c
typedef struct {
    sw_sm_action_t action;
    int next_state;
    void *new_data;
    void *reply;
    uint64_t timeout_ms;     // 0 = no timeout
} sw_sm_result_t;

typedef struct {
    void *(*init)(void *arg, int *initial_state);
    sw_sm_result_t (*handle_event)(sw_sm_event_type_t type, void *event,
                                   int state, void *data, sw_process_t *from);
    void (*terminate)(int state, void *data, int reason);
} sw_sm_callbacks_t;
```

### Functions

```c
sw_process_t *sw_statemachine_start(const char *name, sw_sm_callbacks_t *cbs, void *arg);
sw_process_t *sw_statemachine_start_link(const char *name, sw_sm_callbacks_t *cbs, void *arg);
```
Start a state machine.

```c
void *sw_sm_call(const char *name, void *request, uint64_t timeout_ms);
void *sw_sm_call_proc(sw_process_t *sm, void *request, uint64_t timeout_ms);
```
Synchronous call to a state machine.

```c
void sw_sm_cast(const char *name, void *event);
void sw_sm_cast_proc(sw_process_t *sm, void *event);
```
Asynchronous event to a state machine.

```c
void sw_sm_stop(const char *name);
```
Stop a state machine.

---

## 10. Process Groups

**Header:** `swarmrt_phase5.h`

Pub/sub style process grouping for broadcast messaging.

### Functions

```c
int sw_pg_join(const char *group, sw_process_t *proc);
```
Join a process group. Creates the group if it doesn't exist.

```c
int sw_pg_leave(const char *group, sw_process_t *proc);
```
Leave a process group.

```c
int sw_pg_dispatch(const char *group, uint64_t tag, void *msg);
```
Send a tagged message to all members of a group.

```c
int sw_pg_members(const char *group, sw_process_t **out, uint32_t max);
```
List group members. Returns count written to `out`.

```c
uint32_t sw_pg_count(const char *group);
```
Count members in a group.

```c
void sw_pg_cleanup_proc(sw_process_t *proc);
```
Internal: auto-remove a dead process from all groups.

---

## 11. IO / Ports

**Header:** `swarmrt_io.h`

kqueue-based async I/O with TCP support.

### Port Types

| Type | Description |
|------|-------------|
| `SW_PORT_TCP_LISTEN` | TCP listener socket |
| `SW_PORT_TCP_CONN` | TCP connection |
| `SW_PORT_PIPE` | Pipe (stdin/stdout) |

### Port Message Tags

| Tag | Description |
|-----|-------------|
| `SW_TAG_PORT_DATA` (20) | Data received |
| `SW_TAG_PORT_ACCEPT` (21) | New connection accepted |
| `SW_TAG_PORT_CLOSED` (22) | Port closed |
| `SW_TAG_PORT_ERROR` (23) | Port error |

### Types

```c
typedef struct sw_port {
    int fd;
    sw_port_type_t type;
    sw_port_state_t state;
    sw_process_t *owner;
    uint32_t id;
    uint8_t *recv_buf;
    uint32_t recv_buf_size;
    struct sw_port *next;
} sw_port_t;

typedef struct {
    sw_port_t *port;
    uint8_t *data;
    uint32_t len;
} sw_port_data_t;

typedef struct {
    sw_port_t *listener;
    sw_port_t *conn;
} sw_port_accept_t;
```

### Functions

```c
int sw_io_init(void);
```
Initialize the I/O subsystem (kqueue). Called once at startup.

```c
void sw_io_shutdown(void);
```
Shut down I/O and close all ports.

```c
sw_port_t *sw_tcp_listen(const char *addr, uint16_t port);
```
Create a TCP listener. The owning process receives `SW_TAG_PORT_ACCEPT` messages.

```c
sw_port_t *sw_tcp_connect(const char *addr, uint16_t port);
```
Connect to a remote TCP endpoint.

```c
int sw_tcp_send(sw_port_t *port, const void *data, uint32_t len);
```
Send data over a TCP connection.

```c
void sw_port_set_active(sw_port_t *port, int active);
```
Toggle active mode. When active, data arrives as messages. When passive, use explicit receive.

```c
void sw_port_close(sw_port_t *port);
```
Close a port.

```c
void sw_port_controlling_process(sw_port_t *port, sw_process_t *new_owner);
```
Transfer port ownership to another process.

```c
void sw_io_cleanup_owner(sw_process_t *proc);
```
Internal: close all ports owned by a dying process.

---

## 12. Hot Code Reload

**Header:** `swarmrt_hotload.h`

Live module upgrade without stopping processes.

### Types

```c
typedef struct {
    char name[SW_MODULE_NAME_MAX];  // max 64 chars
    void (*current_func)(void *);
    void (*previous_func)(void *);
    uint32_t version;
} sw_module_t;

typedef struct {
    uint32_t old_version;
    uint32_t new_version;
    void (*new_func)(void *);
    char module_name[SW_MODULE_NAME_MAX];
} sw_code_change_t;
```

### Functions

```c
sw_module_t *sw_module_register(const char *name, void (*func)(void *));
```
Register a named module with its entry function.

```c
sw_module_t *sw_module_find(const char *name);
```
Look up a registered module.

```c
int sw_module_upgrade(const char *name, void (*new_func)(void *));
```
Upgrade a module to a new version. Old version kept for rollback. Running processes
receive `SW_TAG_CODE_CHANGE` (15) messages.

```c
int sw_module_rollback(const char *name);
```
Roll back to the previous version.

```c
uint32_t sw_module_version(const char *name);
```
Get the current version number of a module.

```c
sw_process_t *sw_module_spawn(const char *module_name, void *arg);
```
Spawn a process running a registered module's entry function.

```c
sw_process_t *sw_module_spawn_link(const char *module_name, void *arg);
```
Spawn-link a module process.

```c
void sw_module_cleanup_proc(sw_process_t *proc);
```
Internal: remove dead process from module tracking.

---

## 13. Garbage Collection

**Header:** `swarmrt_gc.h`

Per-process generational garbage collection.

### Types

```c
typedef struct {
    uint64_t minor_gcs;
    uint64_t major_gcs;
    uint64_t words_reclaimed;
    uint64_t total_gc_time_us;
} sw_gc_stats_t;
```

### Functions

```c
void *sw_heap_alloc(sw_process_t *proc, size_t words);
```
Allocate `words` on a process's heap. May trigger GC if full.

```c
int sw_gc_minor(sw_process_t *proc);
```
Trigger a minor (young generation) GC. Returns words reclaimed.

```c
int sw_gc_major(sw_process_t *proc);
```
Trigger a major (full) GC. Returns words reclaimed.

```c
sw_gc_stats_t sw_gc_stats(sw_process_t *proc);
```
Get GC statistics for a process.

```c
int sw_heap_contains(sw_process_t *proc, void *ptr);
```
Check if a pointer is within a process's heap.

```c
size_t sw_heap_used(sw_process_t *proc);
```
Return heap words currently in use.

```c
size_t sw_heap_size(sw_process_t *proc);
```
Return total heap capacity in words.

---

## 14. Distribution

**Header:** `swarmrt_node.h`

Multi-node clustering and remote message passing.

### Types

```c
typedef struct sw_node {
    char name[SW_NODE_NAME_MAX];  // max 64 chars
    char host[64];
    uint16_t port;
    sw_port_t *listener;
    int active;
} sw_node_t;

typedef struct sw_peer {
    char name[SW_NODE_NAME_MAX];
    char host[64];
    uint16_t port;
    sw_port_t *conn;
    int connected;
} sw_peer_t;

typedef struct {
    char from_node[SW_NODE_NAME_MAX];
    uint64_t from_pid;
    char to_node[SW_NODE_NAME_MAX];
    uint64_t to_pid;
    char to_name[SW_REG_NAME_MAX];
    uint64_t tag;
    uint32_t payload_len;
    /* payload bytes follow header */
} sw_remote_msg_t;
```

### Functions

```c
int sw_node_start(const char *name, uint16_t port);
```
Start the distribution layer. Listens on `port` for peer connections.

```c
void sw_node_stop(void);
```
Stop distribution.

```c
const char *sw_node_name(void);
```
Return this node's name.

```c
int sw_node_connect(const char *name, const char *host, uint16_t port);
```
Connect to a remote node.

```c
int sw_node_disconnect(const char *name);
```
Disconnect from a peer.

```c
int sw_node_send(const char *node, const char *name, uint64_t tag,
                 const void *data, uint32_t len);
```
Send a message to a named process on a remote node.

```c
int sw_node_send_pid(const char *node, uint64_t pid, uint64_t tag,
                     const void *data, uint32_t len);
```
Send a message to a process by PID on a remote node.

```c
int sw_node_peers(char names[][SW_NODE_NAME_MAX], int max);
```
List connected peers. Returns count.

```c
int sw_node_is_connected(const char *name);
```
Check if a peer is connected.

---

## 15. HTTP / WebSocket

**Header:** `swarmrt_http.h`

Built-in HTTP server with WebSocket and LiveView support.

### Types

```c
typedef struct {
    int                 id;
    sw_port_t          *port;
    sw_http_mode_t      mode;     // SW_HTTP_MODE_HTTP or SW_HTTP_MODE_WS
    sw_process_t       *handler;
    uint8_t            *buf;
    uint32_t            buf_len;
    uint32_t            buf_cap;
    uint32_t            content_length;
    int                 body_pending;
    int                 keep_alive;
    char                ws_key[128];
    int                 active;
} sw_http_conn_t;
```

### Functions

```c
sw_process_t *sw_http_listen(uint16_t port, sw_process_t *handler);
```
Start an HTTP server on `port`. The `handler` process receives request messages.

```c
int sw_http_respond(int conn_id, int status, const char *headers, const char *body);
```
Send an HTTP response with string body.

```c
int sw_http_respond_raw(int conn_id, int status, const char *headers,
                        const void *data, uint32_t data_len);
```
Send an HTTP response with raw binary body.

```c
int sw_ws_send_text(int conn_id, const char *data, uint32_t len);
```
Send a WebSocket text frame.

```c
int sw_ws_close(int conn_id);
```
Close a WebSocket connection.

```c
int sw_ws_set_handler(int conn_id, sw_process_t *handler);
```
Change the handler process for a WebSocket connection.

```c
const char *sw_liveview_js(void);
```
Return the embedded LiveView client JavaScript.

---

## 16. Language Frontend

**Header:** `swarmrt_lang.h`

Parser, tree-walking interpreter, and value system for `.sw` files.

### Value Types

| Type | Description |
|------|-------------|
| `SW_VAL_NIL` | Nil/null |
| `SW_VAL_INT` | 64-bit integer |
| `SW_VAL_FLOAT` | 64-bit float |
| `SW_VAL_STRING` | Heap-allocated string |
| `SW_VAL_ATOM` | Atom (interned string) |
| `SW_VAL_PID` | Process pointer |
| `SW_VAL_TUPLE` | Fixed-size tuple |
| `SW_VAL_LIST` | Variable-size list |
| `SW_VAL_FUN` | Function (closure or native) |
| `SW_VAL_MAP` | Key-value map |

### Interpreter

```c
void *sw_lang_parse(const char *source);
```
Parse `.sw` source code into a module AST. Returns NULL on parse error.

```c
sw_interp_t *sw_lang_new(void *module_ast);
```
Create an interpreter from a parsed module AST.

```c
sw_val_t *sw_lang_call(sw_interp_t *interp, const char *func_name,
                        sw_val_t **args, int num_args);
```
Call a named function in the module with the given arguments.

```c
sw_val_t *sw_lang_eval(sw_interp_t *interp, const char *expr_source);
```
Evaluate an expression string. Variables are local to the evaluation.

```c
sw_val_t *sw_lang_eval_repl(sw_interp_t *interp, const char *expr_source);
```
Evaluate in REPL mode. Variable assignments persist in the interpreter's global environment.

```c
void sw_lang_free(sw_interp_t *interp);
```
Free an interpreter and its AST.

### Value Constructors

```c
sw_val_t *sw_val_nil(void);
sw_val_t *sw_val_int(int64_t i);
sw_val_t *sw_val_float(double f);
sw_val_t *sw_val_string(const char *s);
sw_val_t *sw_val_atom(const char *s);
sw_val_t *sw_val_pid(sw_process_t *p);
sw_val_t *sw_val_tuple(sw_val_t **items, int count);
sw_val_t *sw_val_list(sw_val_t **items, int count);
sw_val_t *sw_val_fun_native(void *fn_ptr, int nparams,
                             sw_val_t **captures, int ncaptures);
```

### Map Operations

```c
sw_val_t *sw_val_map_new(sw_val_t **keys, sw_val_t **vals, int count);
sw_val_t *sw_val_map_get(sw_val_t *map, sw_val_t *key);
sw_val_t *sw_val_map_put(sw_val_t *map, sw_val_t *key, sw_val_t *val);
```

### Value Inspection

```c
int sw_val_is_truthy(sw_val_t *v);
```
Returns 0 for `nil` and `:false`, 1 for everything else.

```c
int sw_val_equal(sw_val_t *a, sw_val_t *b);
```
Deep structural equality.

```c
void sw_val_print(sw_val_t *v);
```
Pretty-print a value to stdout.

```c
void sw_val_free(sw_val_t *v);
```
Free a value and its owned memory.

### JSON

```c
sw_val_t *sw_lang_json_decode(const char *s);
```
Decode a JSON string into SwarmRT values.

### Built-in Functions (available in `.sw` files)

| Function | Description |
|----------|-------------|
| `print(args...)` | Print values to stdout |
| `length(v)` | Length of list, tuple, or string |
| `hd(list)` | Head (first element) of a list |
| `tl(list)` | Tail (all but first) of a list |
| `elem(tuple, i)` | Access tuple element by index |
| `abs(n)` | Absolute value |
| `to_string(v)` | Convert to string |
| `typeof(v)` | Type name as string |
| `map_new()` | Create empty map |
| `map_get(m, k)` | Get value from map |
| `map_put(m, k, v)` | Put value in map (returns new map) |
| `map_keys(m)` | List of map keys |
| `map_values(m)` | List of map values |
| `list_append(l, v)` | Append to list |
| `assert(cond)` | Assert condition is truthy |
| `assert_eq(a, b)` | Assert `a == b` |
| `assert_ne(a, b)` | Assert `a != b` |

### AST Node Types

```c
typedef enum {
    N_MODULE, N_FUN, N_BLOCK, N_ASSIGN, N_CALL, N_SPAWN, N_SEND,
    N_RECEIVE, N_CLAUSE, N_IF, N_BINOP, N_UNARY, N_PIPE,
    N_INT, N_FLOAT, N_STRING, N_ATOM, N_IDENT, N_TUPLE, N_LIST,
    N_SELF, N_AFTER, N_MAP, N_FOR, N_RANGE, N_TRY, N_LIST_CONS,
} node_type_t;
```

---

## 17. Codegen

**Header:** `swarmrt_codegen.h`

AST to C code generation pipeline.

### Functions

```c
int sw_codegen(void *ast, FILE *out, int obfuscate);
```
Generate C code from a single module AST.

```c
char *sw_codegen_to_string(void *ast, int obfuscate);
```
Generate C code as a malloc'd string.

```c
int sw_codegen_module(void *ast, FILE *out);
```
Generate a single module (no main wrapper).

```c
int sw_codegen_multi(void **modules, int nmodules, int main_idx, FILE *out);
```
Generate C code for multiple modules. `main_idx` identifies which module contains `main()`.

```c
char *sw_obfuscate(const char *code, const char *mod_name,
                   const char **func_names, int nfuncs);
```
Apply XOR string encryption and symbol mangling to generated C code.

---

## 18. Constants & Tags

### Message Tags

| Constant | Value | Description |
|----------|-------|-------------|
| `SW_TAG_NONE` | 0 | Untagged message |
| `SW_TAG_EXIT` | 1 | Exit signal |
| `SW_TAG_DOWN` | 2 | Monitor down signal |
| `SW_TAG_TIMER` | 3 | Timer expiry |
| `SW_TAG_CALL` | 10 | GenServer call |
| `SW_TAG_CAST` | 11 | GenServer cast |
| `SW_TAG_STOP` | 12 | Stop signal |
| `SW_TAG_TASK_RESULT` | 13 | Task completion |
| `SW_TAG_CODE_CHANGE` | 15 | Hot code reload |
| `SW_TAG_REMOTE_MSG` | 16 | Distribution message |
| `SW_TAG_PORT_DATA` | 20 | Port data received |
| `SW_TAG_PORT_ACCEPT` | 21 | Port connection accepted |
| `SW_TAG_PORT_CLOSED` | 22 | Port closed |
| `SW_TAG_PORT_ERROR` | 23 | Port error |

### Configuration Limits

| Constant | Default | Description |
|----------|---------|-------------|
| `SWARM_MAX_PROCESSES` | 100,000 | Max concurrent processes |
| `SWARM_MAX_SCHEDULERS` | 64 | Max scheduler threads |
| `SWARM_STACK_MIN_SIZE` | 2 KB | Minimum process stack |
| `SWARM_STACK_MAX_SIZE` | 1 MB | Maximum process stack |
| `SWARM_HEAP_MIN_SIZE` | 256 words | Initial process heap |
| `SWARM_CONTEXT_REDS` | 2,000 | Reductions per time slice |
| `SWARM_TIME_SLICE_US` | 1,000 | Time slice in microseconds |
| `SW_ENV_SLOTS` | 32 | Hash buckets per environment |
| `SW_REGISTRY_BUCKETS` | 4,096 | Process registry hash size |
| `SW_REG_NAME_MAX` | 64 | Max registered name length |
| `SW_MODULE_NAME_MAX` | 64 | Max module name length |
| `SW_MODULE_MAX` | 256 | Max registered modules |
| `SW_PG_MAX_MEMBERS` | 1,024 | Max members per process group |
| `SW_PG_BUCKETS` | 256 | Process group hash size |
| `SW_DYNSUP_MAX_CHILDREN` | 4,096 | Max dynamic supervisor children |
| `SW_HTTP_MAX_CONNS` | 256 | Max HTTP connections |
| `SW_NODE_NAME_MAX` | 64 | Max node name length |
| `SW_NODE_MAX_PEERS` | 32 | Max connected peers |
| `SW_ETS_INVALID_TID` | 0 | Invalid table ID sentinel |

### Process Flags

| Flag | Value | Description |
|------|-------|-------------|
| `SW_FLAG_TRAP_EXIT` | 0x01 | Convert exit signals to messages |

### Priority Levels

| Priority | Value | Description |
|----------|-------|-------------|
| `SW_PRIO_MAX` | 0 | Highest priority |
| `SW_PRIO_HIGH` | 1 | High priority |
| `SW_PRIO_NORMAL` | 2 | Default priority |
| `SW_PRIO_LOW` | 3 | Low priority |
