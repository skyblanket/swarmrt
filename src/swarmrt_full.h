/*
 * SwarmRT Full - Erlang Parity for the AI Age
 * Complete BEAM-compatible runtime with AI-native extensions
 */

#ifndef SWARMRT_FULL_H
#define SWARMRT_FULL_H

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <pthread.h>
#include <ucontext.h>

/* === AI-Native Extensions === */
#define SWARM_AI_EXTENSIONS     1
#define SWARM_MAX_SWARMS        256
#define SWARM_MAX_PROCESSES     (1 << 22)   /* 4M processes */
#define SWARM_STACK_SIZE        1024        /* 1KB initial */
#define SWARM_REDUCTIONS        2000        /* BEAM default */

/* === Term Types (Full Erlang) === */
typedef enum {
    SW_IMMEDIATE = 0,       /* Small ints, atoms, pids, ports */
    SW_CONS = 1,            /* List [Head|Tail] */
    SW_TUPLE = 2,           /* {A, B, C} */
    SW_BOXED = 3,           /* Floats, big ints, binaries, refs */
    SW_FUN = 4,             /* Fun, closures */
} sw_term_tag_t;

/* Erlang term representation (tagged pointers) */
typedef uintptr_t sw_term_t;

#define SW_TAG_MASK     0x3
#define SW_TAG_SHIFT    2

/* === Process Structure (Full BEAM parity) === */
typedef struct sw_process {
    /* Frequently accessed (cache line 1) */
    sw_term_t *htop;                /* Heap top */
    sw_term_t *stop;                /* Stack top */
    sw_term_t *heap;                /* Heap start */
    sw_term_t *hend;                /* Heap end */
    
    /* Reductions and scheduling */
    int32_t fcalls;                 /* Reductions remaining */
    uint32_t reds;                  /* Total reductions executed */
    uint32_t priority;              /* 0=max, 3=low */
    
    /* Program counter and state */
    uint8_t *pc;                    /* Bytecode pointer */
    uint32_t state;                 /* RUNNING, WAITING, etc */
    uint64_t pid;                   /* Process ID */
    
    /* Context for switching */
    ucontext_t ctx;
    void *stack_base;
    size_t stack_size;
    
    /* Message queue (double-ended) */
    struct sw_msg *msg_first;
    struct sw_msg *msg_last;
    uint32_t msg_count;
    pthread_mutex_t msg_lock;
    pthread_cond_t msg_avail;
    
    /* Process dictionary (ETS-like for single process) */
    struct sw_dict_entry *pdict;
    
    /* Links and monitors */
    struct sw_link *links;
    struct sw_monitor *monitors;
    
    /* Group leader and parent */
    uint64_t group_leader;
    uint64_t parent;
    
    /* Exception handling */
    sw_term_t fvalue;               /* Last failure value */
    uint32_t freason;               /* Failure reason */
    int catches;                    /* Catch depth */
    
    /* GC info */
    sw_term_t *old_heap;
    sw_term_t *old_htop;
    uint32_t gen_gcs;               /* Minor GC count */
    uint32_t max_gen_gcs;           /* Max before fullsweep */
    
    /* AI extensions */
    void *ai_context;               /* LLM context window */
    uint32_t ai_capabilities;       /* Tool permissions */
    
    /* Scheduler linkage */
    struct sw_scheduler *scheduler;
    struct sw_process *rq_next;
    struct sw_process *rq_prev;
} sw_process_t;

/* === Message Structure === */
typedef struct sw_msg {
    sw_term_t data;                 /* Message payload (copied to heap) */
    uint64_t from;                  /* Sender PID */
    uint64_t timestamp;
    struct sw_msg *next;
} sw_msg_t;

/* === Scheduler (Per OS Thread) === */
typedef struct sw_scheduler {
    uint32_t id;
    pthread_t thread;
    
    /* Run queues by priority */
    sw_process_t *runq[4];          /* 0=max, 1=high, 2=normal, 3=low */
    uint32_t runq_count[4];
    pthread_mutex_t runq_lock;
    
    /* Current process */
    sw_process_t *current;
    
    /* Stats */
    uint64_t context_switches;
    uint64_t reductions;
    uint64_t spawns;
    
    /* For work stealing */
    struct sw_swarm *swarm;
    int steal_count;
} sw_scheduler_t;

/* === Swarm (Cluster of Schedulers) === */
typedef struct sw_swarm {
    char name[32];
    uint32_t num_scheds;
    sw_scheduler_t *scheds[64];
    
    /* Process table (hash table for O(1) lookup) */
    sw_process_t **proc_table;
    uint64_t proc_count;
    uint64_t next_pid;
    pthread_mutex_t pid_lock;
    
    /* Global stats */
    uint64_t total_spawns;
    uint64_t total_reductions;
    uint64_t total_msgs;
    
    /* Distribution */
    uint32_t node_id;
    struct sw_dist *distribution;
} sw_swarm_t;

/* === Bytecode Instructions === */
typedef enum {
    SW_OP_LABEL,        /* label Lbl */
    SW_OP_FUNC_INFO,    /* func_info M F A */
    SW_OP_INT_CODE_END, /* int_code_end */
    SW_OP_CALL,         /* call Arity Label */
    SW_OP_CALL_LAST,    /* call_last Arity Label Deallocate */
    SW_OP_CALL_ONLY,    /* call_only Arity Label */
    SW_OP_CALL_EXT,     /* call_ext Arity ExtFunc */
    SW_OP_BIF,          /* bif Bif Reg */
    SW_OP_ALLOC,        /* allocate N */
    SW_OP_ALLOC_HEAP,   /* allocate_heap N Live */
    SW_OP_TEST_HEAP,    /* test_heap N Live */
    SW_OP_INIT,         /* init N */
    SW_OP_DEALLOCATE,   /* deallocate N */
    SW_OP_RETURN,       /* return */
    SW_OP_RECV_MARK,    /* recv_mark Label */
    SW_OP_RECV_SET,     /* recv_set Label */
    SW_OP_LOOP_REC,     /* loop_rec Label Reg */
    SW_OP_LOOP_REC_END, /* loop_rec_end Label */
    SW_OP_WAIT,         /* wait Label */
    SW_OP_WAIT_TIMEOUT, /* wait_timeout Label Time */
    SW_OP_RECV_CLEAR,   /* recv_clear */
    SW_OP_RECV_SUSP,    /* recv_susp Label */
    SW_OP_TIMEOUT,      /* timeout */
    SW_OP_SEND,         /* send */
    SW_OP_REMOVE_MSG,   /* remove_msg */
    SW_OP_CMP,          /* cmp Op Reg1 Reg2 */
    SW_OP_IS_EQ,        /* is_eq Label Reg Val */
    SW_OP_IS_NE,        /* is_ne Label Reg Val */
    SW_OP_IS_LT,        /* is_lt Label Reg Val */
    SW_OP_IS_GE,        /* is_ge Label Reg Val */
    SW_OP_IS_INTEGER,   /* is_integer Label Reg */
    SW_OP_IS_ATOM,      /* is_atom Label Reg */
    SW_OP_IS_TUPLE,     /* is_tuple Label Reg */
    SW_OP_IS_LIST,      /* is_list Label Reg */
    SW_OP_IS_BINARY,    /* is_binary Label Reg */
    SW_OP_MOVE,         /* move Src Dst */
    SW_OP_GET_TUPLE_ELEMENT, /* get_tuple_element Src Pos Dst */
    SW_OP_SET_TUPLE_ELEMENT, /* set_tuple_element Val Src Pos */
    SW_OP_PUT_LIST,     /* put_list Head Tail Dst */
    SW_OP_PUT_TUPLE,    /* put_tuple Arity Dst */
    SW_OP_PUT,          /* put Val */
    SW_OP_GET_LIST,     /* get_list Src Hd Tl */
    SW_OP_TRIM,         /* trim N */
    SW_OP_PUT_STR,      /* put_str Lit Dst */
    SW_OP_MAKE_FUN,     /* make_fun Label NumFree Dst */
    SW_OP_CALL_FUN,     /* call_fun Arity */
    SW_OP_IS_FUNCTION,  /* is_function Label Reg */
    SW_OP_ARITH,        /* arith Op Reg1 Reg2 Dst */
    SW_OP_GC_BIF,       /* gc_bif Bif Live Regs Dst */
    SW_OP_LINE,         /* line LineNum */
    SW_OP_BADMATCH,     /* badmatch Reg */
    SW_OP_CASE_END,     /* case_end Reg */
    SW_OP_TRY,          /* try Label Reg */
    SW_OP_TRY_END,      /* try_end Reg */
    SW_OP_TRY_CASE,     /* try_case Reg */
    SW_OP_TRY_CASE_END, /* try_case_end */
    SW_OP_CATCH,        /* catch Label Reg */
    SW_OP_CATCH_END,    /* catch_end Reg */
    SW_OP_JUMP,         /* jump Label */
    SW_OP_SELECT_VAL,   /* select_val Reg Label Cases */
    SW_OP_SELECT_TUPLE, /* select_tuple_reg Reg Dst */
    SW_OP_BS_PUT,       /* Binary construction */
    SW_OP_BS_GET,       /* Binary matching */
    SW_OP_PUT_CONSTANT, /* put_constant Val Dst */
    SW_OP_IS_CONST,     /* is_const Label Reg Val */
    /* AI extensions */
    SW_OP_AI_PROMPT,    /* ai_prompt Template Dst */
    SW_OP_AI_TOOL,      /* ai_tool ToolName Args Dst */
    SW_OP_AI_EMBED,     /* ai_embed Text Dst */
    SW_NUM_OPS
} sw_opcode_t;

/* === Public API === */

/* System */
int sw_init(const char *name, uint32_t scheds);
void sw_shutdown(void);

/* Process lifecycle */
sw_process_t *sw_spawn(sw_term_t mod, sw_term_t fun, sw_term_t args);
sw_process_t *sw_spawn_link(sw_term_t mod, sw_term_t fun, sw_term_t args);
void sw_exit(sw_process_t *p, sw_term_t reason);
void sw_link(sw_process_t *p1, sw_process_t *p2);
void sw_unlink(sw_process_t *p1, sw_process_t *p2);

/* Scheduling */
void sw_schedule(void);
void sw_yield(void);
void sw_wait(sw_process_t *p);
void sw_wakeup(sw_process_t *p);

/* Message passing */
void sw_send(uint64_t to, sw_term_t msg);
sw_term_t sw_receive(void);
sw_term_t sw_receive_timeout(uint64_t ms);

/* Term construction */
sw_term_t sw_mk_atom(const char *name);
sw_term_t sw_mk_int(int64_t i);
sw_term_t sw_mk_float(double f);
sw_term_t sw_mk_tuple(uint32_t size);
sw_term_t sw_mk_list(sw_term_t head, sw_term_t tail);
sw_term_t sw_mk_binary(const uint8_t *data, size_t len);
sw_term_t sw_mk_pid(uint64_t pid);
sw_term_t sw_mk_ref(void);

/* Pattern matching */
int sw_match(sw_term_t pattern, sw_term_t value, sw_term_t *bindings);

/* GC */
void sw_gc_minor(sw_process_t *p);
void sw_gc_major(sw_process_t *p);

/* Bytecode */
int sw_load_module(const char *name, uint8_t *code, size_t size);
sw_term_t sw_call(sw_term_t mod, sw_term_t fun, sw_term_t args);

/* AI Extensions */
sw_term_t sw_ai_prompt(const char *template, sw_term_t context);
sw_term_t sw_ai_tool(const char *name, sw_term_t args);
sw_term_t sw_ai_embed(const char *text);

/* Supervision */
int sw_supervise(sw_process_t *parent, sw_process_t *child, uint32_t strategy);

/* Distribution */
int sw_connect_node(const char *node_name);
void sw_send_dist(uint64_t node_id, uint64_t to, sw_term_t msg);

/* ETS-like storage */
typedef struct sw_table sw_table_t;
sw_table_t *sw_table_new(const char *name, uint32_t type);
void sw_table_insert(sw_table_t *tab, sw_term_t key, sw_term_t value);
sw_term_t sw_table_lookup(sw_table_t *tab, sw_term_t key);

#endif