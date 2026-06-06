/*
 * SwarmRT Phase 10: Language Frontend
 *
 * Tree-walking interpreter for .sw files. Parses source into AST,
 * evaluates expressions, and maps language primitives (spawn, send,
 * receive) to SwarmRT runtime calls.
 *
 * Value types: int, float, string, atom, pid, tuple, list, nil, fun, map.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_LANG_H
#define SWARMRT_LANG_H

#include <stdio.h>

#include "swarmrt_native.h"

/* === Value Representation === */

typedef enum {
    SW_VAL_NIL = 0,
    SW_VAL_INT,
    SW_VAL_FLOAT,
    SW_VAL_STRING,
    SW_VAL_ATOM,
    SW_VAL_PID,
    SW_VAL_TUPLE,
    SW_VAL_LIST,
    SW_VAL_FUN,
    SW_VAL_MAP,
    SW_VAL_REMOTE_PID,   /* pid that lives on another node — carries node
                            name + pid id; `send` routes via TCP. */
    SW_VAL_BYTES,        /* length-carrying, NUL-safe byte vector. MUST stay
                            last in this enum: typeof()'s positional names[]
                            array in swarmrt_lang.c relies on prefix indices. */
} sw_val_type_t;

typedef struct sw_val sw_val_t;
typedef struct sw_env sw_env_t;

struct sw_val {
    sw_val_type_t type;
    union {
        int64_t i;
        double f;
        char *str;                   /* string or atom text (owned) */
        sw_process_t *pid;
        struct {
            sw_val_t **items;
            int count;
        } tuple;                     /* also used for list */
        struct {
            char *name;
            char **params;
            int num_params;
            void *body;              /* ast_node_t* (interpreter) */
            sw_env_t *closure_env;   /* interpreter closure env */
            void *cfunc;             /* compiled function pointer */
            sw_val_t **captures;     /* captured variable values */
            int ncaptures;
        } fun;
        struct {
            sw_val_t **keys;
            sw_val_t **vals;
            int count;
            int cap;
        } map;
        struct {
            char *node;          /* node name string, owned */
            uint64_t id;         /* remote pid id */
        } rpid;
        struct {
            uint8_t *data;       /* owned heap buffer (malloc). May contain
                                    embedded NUL bytes; NOT NUL-terminated.
                                    Always a distinct block owned solely by
                                    this sw_val; len==0 still owns a 1-byte
                                    block so free()/memcmp() stay unconditional. */
            size_t   len;        /* exact byte count */
        } bytes;
    } v;
};

/* === Environment (scope chain) === */

#define SW_ENV_SLOTS 32

typedef struct sw_env_entry {
    char *name;
    sw_val_t *val;
    struct sw_env_entry *next;
} sw_env_entry_t;

struct sw_env {
    sw_env_entry_t *buckets[SW_ENV_SLOTS];
    sw_env_t *parent;
    int refcount;   /* interpreter env lifetime: frame holds 1 ref, each child
                       env and each capturing closure holds 1 ref on this env.
                       Freed only when refcount hits 0 (see env_retain/release). */
};

/* === Interpreter Context === */

typedef struct {
    sw_env_t *global_env;
    void *module_ast;                /* ast_node_t* */
    int error;
    char error_msg[256];
    /* Test assertion state (used by swarmrt_test) */
    int assert_failed;
    char assert_msg[512];
    int assert_line;
    int call_depth;   /* interpreter recursion-depth guard (no TCO on C stack) */
} sw_interp_t;

/* === Public API === */

/* Parse source code, returns module AST (or NULL on error) */
void *sw_lang_parse(const char *source);

/* 1 if the parsed module defines a function named `name` (for validating a
 * parsed module before use, e.g. the tool registry checking for `run`). */
int sw_lang_has_fun(void *module_ast, const char *name);

/* 1 if `name` is a builtin the RUNTIME interpreter actually implements (the
 * authoritative runtime set — codegen's is_builtin is compiler-only). Used by
 * the tool-registry admission lint to tell a real builtin from a typo. */
int interp_is_known_builtin(const char *name);

/* Tool-registry admission lint. Walks `module_ast` read-only; returns non-zero
 * (and writes a human reason into `err`) if any call target is neither a
 * runtime builtin nor a fn in this module, or is a capability-gated builtin
 * (the file_ / db_ / shell families) whose capability is not in `caps` (a
 * list/tuple of atoms; NULL => pure-only). Returns 0 when the tool is admitted. */
int sw_lang_lint_tool(void *module_ast, sw_val_t *caps, char *err, unsigned long errlen);

/* Call `func_name` in `module_ast` with a FRESH, per-call interpreter (its own
 * global_env + error/call_depth) off the (immutable, read-only) AST. Safe to run
 * concurrently from multiple threads on the SAME ast, and a fault never persists
 * past the call. Used by the tool registry (each tool_call is isolated). The
 * returned value is heap-owned and outlives the call; the AST is NOT freed. */
sw_val_t *sw_lang_call_fresh(void *module_ast, const char *func_name,
                             sw_val_t **args, int num_args);

/* Create interpreter from parsed module AST */
sw_interp_t *sw_lang_new(void *module_ast);

/* Evaluate a function by name in the module */
sw_val_t *sw_lang_call(sw_interp_t *interp, const char *func_name,
                        sw_val_t **args, int num_args);

/* Evaluate an expression string (for testing) */
sw_val_t *sw_lang_eval(sw_interp_t *interp, const char *expr_source);

/* Evaluate in REPL mode: assignments persist in global_env */
sw_val_t *sw_lang_eval_repl(sw_interp_t *interp, const char *expr_source);

/* Free interpreter */
void sw_lang_free(sw_interp_t *interp);

/* Value constructors */
sw_val_t *sw_val_nil(void);
sw_val_t *sw_val_int(int64_t i);
sw_val_t *sw_val_float(double f);
sw_val_t *sw_val_string(const char *s);
sw_val_t *sw_val_atom(const char *s);
/* Length-carrying byte vector. Copies `len` bytes from `data` into a fresh
 * owned block (data may be NULL iff len==0). NUL-safe — never uses strlen. */
sw_val_t *sw_val_bytes(const uint8_t *data, size_t len);
sw_val_t *sw_val_pid(sw_process_t *p);
sw_val_t *sw_val_remote_pid(const char *node, uint64_t id);
sw_val_t *sw_val_tuple(sw_val_t **items, int count);
sw_val_t *sw_val_list(sw_val_t **items, int count);
sw_val_t *sw_val_fun_native(void *fn_ptr, int nparams,
                             sw_val_t **captures, int ncaptures);
sw_val_t *sw_val_apply(sw_val_t *fun, sw_val_t **args, int nargs);

/* GC v1: deep-copy a value to the GLOBAL heap (copy-on-escape) so it
 * outlives the source process's arena. See [[swarmrt-gc-design]]. */
sw_val_t *sw_val_deep_copy_global(sw_val_t *v);

/* GC v1: type-safe value-send choke point — deep-copies the payload to the
 * global heap, then enqueues via sw_send_tagged. Route every sw_val_t* send
 * through this (struct sends keep using sw_send_tagged directly). */
void sw_send_value(sw_process_t *to, uint64_t tag, sw_val_t *v);

/* Shared builtin helpers — one impl, called by both backends (see swarmrt_lang.c). */
sw_val_t *sw_coerce_int(sw_val_t *v);    /* to_int:   string/float/int -> int  | nil */
sw_val_t *sw_coerce_float(sw_val_t *v);  /* to_float: string/int/float -> float | nil */
sw_val_t *sw_make_uuid(void);            /* uuid():    random RFC-4122 v4 string */
sw_val_t *sw_now_iso(void);              /* now_iso(): current UTC time as ISO-8601 */

/* Process command-line arguments. A compiled program's generated
 * main() wires argc/argv straight into these (see swarmrt_codegen.c);
 * the os_args() builtin exposes them to sw as a list of strings.
 * Zero/NULL when unset — e.g. under the interpreter. */
extern int    sw_prog_argc;
extern char **sw_prog_argv;

/* Map constructors */
sw_val_t *sw_val_map_new(sw_val_t **keys, sw_val_t **vals, int count);
sw_val_t *sw_val_map_get(sw_val_t *map, sw_val_t *key);
sw_val_t *sw_val_map_put(sw_val_t *map, sw_val_t *key, sw_val_t *val);

/* Value inspection */
int sw_val_is_truthy(sw_val_t *v);
int sw_val_equal(sw_val_t *a, sw_val_t *b);
void sw_val_print(sw_val_t *v);
/* Render a value to a file stream — same shape as sw_val_print but
 * routable to a memstream so to_string() can produce a real string for
 * tuples / lists / maps / pids. */
void sw_val_format(FILE *f, sw_val_t *v);

/* Free a value */
void sw_val_free(sw_val_t *v);

/* === AST Node Types (used by codegen) === */

typedef enum {
    N_MODULE, N_FUN, N_BLOCK, N_ASSIGN, N_CALL, N_SPAWN, N_SEND,
    N_RECEIVE, N_CLAUSE, N_IF, N_BINOP, N_UNARY, N_PIPE,
    N_INT, N_FLOAT, N_STRING, N_ATOM, N_IDENT, N_TUPLE, N_LIST,
    N_SELF, N_AFTER,
    /* Phase 12: Language polish */
    N_MAP, N_FOR, N_RANGE, N_TRY, N_LIST_CONS,
    /* Phase 13: case/match expression — top-level pattern matching */
    N_CASE,
    /* Phase 14: list comprehension — [expr for x in list] or [...when guard] */
    N_LIST_COMP,
} node_type_t;

typedef struct node {
    node_type_t type;
    int line;
    union {
        /* N_MODULE */
        struct { char name[128]; struct node **funs; int nfuns;
                 char imports[32][128]; int nimports;
                 /* Module-level immutable globals: let x = literal */
                 struct { char name[128]; struct node *val; } globals[16];
                 int nglobals; } mod;
        /* N_FUN */
        struct { char name[128]; char params[16][128]; int nparams;
                 struct node *body; struct node *defaults[16]; } fun;
        /* N_BLOCK */
        struct { struct node **stmts; int nstmts; } block;
        /* N_ASSIGN */
        struct { char name[128]; struct node *value; } assign;
        /* N_CALL */
        struct { struct node *func; struct node **args; int nargs; } call;
        /* N_SPAWN — `monitor` set for spawn_monitor(...) which atomically
         * spawns + monitors and returns {pid, ref} instead of a bare pid. */
        struct { struct node *expr; int monitor; } spawn;
        /* N_SEND */
        struct { struct node *to; struct node *msg; } send;
        /* N_RECEIVE */
        /* after_ms is the literal-int fast path (set to -1 when no
         * `after` clause exists). after_expr is the dynamic case:
         * any expression that evaluates to a millisecond timeout,
         * used when the user writes `after some_var { ... }`. */
        struct { struct node **clauses; int nclauses; struct node *after_body; int after_ms; struct node *after_expr; } recv;
        /* N_CLAUSE */
        struct { struct node *pattern; struct node *body; struct node *guard; } clause;
        /* N_IF */
        struct { struct node *cond; struct node *then_b; struct node *else_b; } iff;
        /* N_BINOP */
        struct { struct node *left; struct node *right; char op[4]; } binop;
        /* N_UNARY */
        struct { struct node *operand; char op; } unary;
        /* N_PIPE */
        struct { struct node *val; struct node *func; } pipe;
        /* N_INT */
        int64_t ival;
        /* N_FLOAT */
        double fval;
        /* N_STRING, N_ATOM, N_IDENT */
        /* 8192 to match tok_t.text[] in swarmrt_lang.c so multi-KB string
         * literals (system prompts) aren't truncated when copied here. */
        char sval[8192];
        /* N_TUPLE, N_LIST */
        struct { struct node **items; int count; } coll;
        /* N_MAP */
        struct { struct node **keys; struct node **vals; int count; } map;
        /* N_FOR */
        struct { char var[128]; struct node *iter; struct node *body; } forloop;
        /* N_RANGE */
        struct { struct node *from; struct node *to; } range;
        /* N_TRY */
        struct { struct node *body; char err_var[128]; struct node *catch_body; } trycatch;
        /* N_LIST_CONS */
        struct { struct node *head; struct node *tail; } cons;
        /* N_CASE — top-level pattern match: case subject { pat -> body ; ... }.
         * Reuses N_CLAUSE for arms (pattern + optional guard + body). */
        struct { struct node *subject; struct node **clauses; int nclauses; } casex;
        /* N_LIST_COMP — [body for var in iter] or [body for var in iter when guard].
         * guard is NULL when no `when` clause is present. */
        struct { char var[128]; struct node *iter; struct node *body; struct node *guard; } lcomp;
    } v;
} node_t;

/* JSON decode for distribution layer */
sw_val_t *sw_lang_json_decode(const char *s);

#endif /* SWARMRT_LANG_H */
