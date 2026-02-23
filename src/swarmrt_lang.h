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
};

/* === Interpreter Context === */

typedef struct {
    sw_env_t *global_env;
    void *module_ast;                /* ast_node_t* */
    int error;
    char error_msg[256];
} sw_interp_t;

/* === Public API === */

/* Parse source code, returns module AST (or NULL on error) */
void *sw_lang_parse(const char *source);

/* Create interpreter from parsed module AST */
sw_interp_t *sw_lang_new(void *module_ast);

/* Evaluate a function by name in the module */
sw_val_t *sw_lang_call(sw_interp_t *interp, const char *func_name,
                        sw_val_t **args, int num_args);

/* Evaluate an expression string (for testing) */
sw_val_t *sw_lang_eval(sw_interp_t *interp, const char *expr_source);

/* Free interpreter */
void sw_lang_free(sw_interp_t *interp);

/* Value constructors */
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
sw_val_t *sw_val_apply(sw_val_t *fun, sw_val_t **args, int nargs);

/* Map constructors */
sw_val_t *sw_val_map_new(sw_val_t **keys, sw_val_t **vals, int count);
sw_val_t *sw_val_map_get(sw_val_t *map, sw_val_t *key);
sw_val_t *sw_val_map_put(sw_val_t *map, sw_val_t *key, sw_val_t *val);

/* Value inspection */
int sw_val_is_truthy(sw_val_t *v);
int sw_val_equal(sw_val_t *a, sw_val_t *b);
void sw_val_print(sw_val_t *v);

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
} node_type_t;

typedef struct node {
    node_type_t type;
    int line;
    union {
        /* N_MODULE */
        struct { char name[128]; struct node **funs; int nfuns;
                 char imports[16][128]; int nimports; } mod;
        /* N_FUN */
        struct { char name[128]; char params[16][128]; int nparams;
                 struct node *body; struct node *defaults[16]; } fun;
        /* N_BLOCK */
        struct { struct node **stmts; int nstmts; } block;
        /* N_ASSIGN */
        struct { char name[128]; struct node *value; } assign;
        /* N_CALL */
        struct { struct node *func; struct node **args; int nargs; } call;
        /* N_SPAWN */
        struct { struct node *expr; } spawn;
        /* N_SEND */
        struct { struct node *to; struct node *msg; } send;
        /* N_RECEIVE */
        struct { struct node **clauses; int nclauses; struct node *after_body; int after_ms; } recv;
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
        char sval[2048];
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
    } v;
} node_t;

/* JSON decode for distribution layer */
sw_val_t *sw_lang_json_decode(const char *s);

#endif /* SWARMRT_LANG_H */
