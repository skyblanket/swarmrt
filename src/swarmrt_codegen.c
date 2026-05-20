/*
 * SwarmRT Compiler: AST → C Code Generation
 *
 * Walks parsed .sw ASTs and emits C source that links against SwarmRT.
 * Handles: spawn trampolines, receive/pattern-match, tail call opt,
 * pipe operators, send/receive, and all expression types.
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "swarmrt_codegen.h"
#include "swarmrt_lang.h"

/* =========================================================================
 * Context
 * ========================================================================= */

typedef struct {
    int id;
    char func_name[128];
    int nargs;
    node_t *call_node;
} spawn_info_t;

typedef struct {
    int id;
    node_t *node;          /* the N_FUN node */
    char gen_name[64];     /* generated C function name */
    /* free variables captured from enclosing scope */
    char captures[32][128];
    int ncaptures;
} lambda_info_t;

/* Max functions tracked per module for is_module_func / did_you_mean.
 * Generous — the largest swarm-code module is ~90 functions. */
#define CG_MAX_FUNCS 512

typedef struct {
    FILE *out;
    char mod_name[128];
    int tmp_counter;

    /* current function context */
    char cur_func[128];
    char cur_params[16][128];
    int cur_nparams;
    int has_tail;

    /* declared local variables in current function */
    char declared[256][128];
    int ndeclared;

    /* all function names in module — sized generously: a single .sw
     * module routinely exceeds 64 functions (agent.sw has ~90), and
     * any name past the cap was invisible to is_module_func, which
     * produced spurious "unknown function" warnings for every
     * forward-referenced helper. */
    char func_names[CG_MAX_FUNCS][128];
    int nfuncs;

    /* spawn sites collected during pre-scan */
    spawn_info_t spawns[64];
    int nspawns;

    /* lambda (anonymous function) sites */
    lambda_info_t lambdas[64];
    int nlambdas;

    /* Last source line we emitted a #line directive for in the current
     * function. Lets emit_block emit one #line per source line (so a C
     * error inside that statement points at the exact .sw line) without
     * spamming the generated file. Reset to 0 at the top of each
     * emit_function. */
    int last_line_emitted;
} cg_ctx_t;

/* Forward decl: emit_expr's N_BLOCK case emits per-statement #line
 * directives, but guess_sw_path is defined further down with the rest
 * of emit_function. */
static const char *guess_sw_path(const char *mod_name);

/* C reserved words. Any sw identifier matching one of these would
 * generate invalid C (e.g. `sw_val_t *inline = ...`) so we mangle by
 * appending `_sw` at every C-emission site. The AST + ctx->declared
 * list keep the ORIGINAL sw name so is_declared still matches by
 * source-level name; only the emitted C-side spelling is rewritten.
 * mangle_for_c is deterministic, so reads and writes of the same sw
 * variable always produce the same C variable name. */
static const char *_c_reserved_words[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while",
    NULL
};

/* Returns either `name` verbatim or a pointer into one of 8 rotating
 * static buffers holding `name_sw`. The rotation lets mangle_for_c be
 * called multiple times in a single fprintf without one call clobbering
 * another's result. Not thread-safe — codegen is single-threaded. */
static const char *mangle_for_c(const char *name) {
    if (!name) return "";
    for (int i = 0; _c_reserved_words[i]; i++) {
        if (strcmp(name, _c_reserved_words[i]) == 0) {
            static char bufs[8][192];
            static int slot = 0;
            char *out = bufs[slot];
            slot = (slot + 1) & 7;
            snprintf(out, sizeof(bufs[0]), "%s_sw", name);
            return out;
        }
    }
    return name;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void fresh_var(cg_ctx_t *ctx, char *buf, int sz) {
    snprintf(buf, sz, "_t%d", ctx->tmp_counter++);
}

/* Escape a string for safe embedding in a C string literal.
 * Caller must free() the returned buffer. */
static char *c_escape_str(const char *s) {
    if (!s) { char *r = malloc(1); r[0] = '\0'; return r; }
    size_t len = strlen(s);
    size_t cap = len * 2 + 1;
    char *buf = (char *)malloc(cap);
    size_t o = 0;
    for (size_t i = 0; i < len && o < cap - 2; i++) {
        switch (s[i]) {
            case '"':  buf[o++] = '\\'; buf[o++] = '"'; break;
            case '\\': buf[o++] = '\\'; buf[o++] = '\\'; break;
            case '\n': buf[o++] = '\\'; buf[o++] = 'n'; break;
            case '\r': buf[o++] = '\\'; buf[o++] = 'r'; break;
            case '\t': buf[o++] = '\\'; buf[o++] = 't'; break;
            default:   buf[o++] = s[i]; break;
        }
    }
    buf[o] = '\0';
    return buf;
}

static int is_declared(cg_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->ndeclared; i++)
        if (strcmp(ctx->declared[i], name) == 0) return 1;
    return 0;
}

static void declare_var(cg_ctx_t *ctx, const char *name) {
    if (ctx->ndeclared < 256 && !is_declared(ctx, name))
        strncpy(ctx->declared[ctx->ndeclared++], name, 127);
}

static int is_builtin(const char *name) {
    return strcmp(name, "print") == 0 || strcmp(name, "length") == 0 ||
           strcmp(name, "hd") == 0 || strcmp(name, "tl") == 0 ||
           strcmp(name, "elem") == 0 || strcmp(name, "abs") == 0 ||
           /* Phase 11: Studio builtins */
           strcmp(name, "register") == 0 || strcmp(name, "whereis") == 0 ||
           strcmp(name, "monitor") == 0 || strcmp(name, "link") == 0 ||
           strcmp(name, "ets_new") == 0 || strcmp(name, "ets_put") == 0 ||
           strcmp(name, "ets_get") == 0 || strcmp(name, "ets_delete") == 0 ||
           strcmp(name, "sleep") == 0 || strcmp(name, "getenv") == 0 ||
           strcmp(name, "to_string") == 0 || strcmp(name, "format") == 0 ||
           strcmp(name, "timestamp") == 0 ||
           strcmp(name, "file_write") == 0 || strcmp(name, "file_read") == 0 ||
           strcmp(name, "file_mkdir") == 0 ||
           strcmp(name, "http_post") == 0 ||
           strcmp(name, "json_get") == 0 || strcmp(name, "json_escape") == 0 ||
           strcmp(name, "string_contains") == 0 || strcmp(name, "string_replace") == 0 ||
           strcmp(name, "string_sub") == 0 || strcmp(name, "string_length") == 0 ||
           strcmp(name, "random_int") == 0 || strcmp(name, "list_append") == 0 ||
           /* Functional primitives */
           strcmp(name, "map") == 0 || strcmp(name, "pmap") == 0 ||
           strcmp(name, "reduce") == 0 || strcmp(name, "filter") == 0 ||
           /* Supervisor */
           strcmp(name, "supervise") == 0 ||
           /* Distributed nodes */
           strcmp(name, "node_start") == 0 || strcmp(name, "node_stop") == 0 ||
           strcmp(name, "node_name") == 0 || strcmp(name, "node_connect") == 0 ||
           strcmp(name, "node_disconnect") == 0 || strcmp(name, "node_send") == 0 ||
           strcmp(name, "node_peers") == 0 || strcmp(name, "node_is_connected") == 0 ||
           /* Map builtins */
           strcmp(name, "map_new") == 0 || strcmp(name, "map_get") == 0 ||
           strcmp(name, "map_put") == 0 || strcmp(name, "map_keys") == 0 ||
           strcmp(name, "map_values") == 0 || strcmp(name, "map_merge") == 0 ||
           strcmp(name, "map_has_key") == 0 || strcmp(name, "map_size") == 0 ||
           strcmp(name, "map_remove") == 0 ||
           /* Error */
           strcmp(name, "error") == 0 ||
           strcmp(name, "panic") == 0 || strcmp(name, "expect") == 0 ||
           strcmp(name, "subprocess_spawn") == 0 ||
           strcmp(name, "subprocess_send_line") == 0 ||
           strcmp(name, "subprocess_recv_line") == 0 ||
           strcmp(name, "subprocess_close") == 0 ||
           strcmp(name, "db_open") == 0 || strcmp(name, "db_close") == 0 ||
           strcmp(name, "db_exec") == 0 || strcmp(name, "db_query") == 0 ||
           strcmp(name, "typeof") == 0 ||
           /* Phase 13: Agent stdlib */
           strcmp(name, "http_get") == 0 || strcmp(name, "shell") == 0 ||
           strcmp(name, "json_encode") == 0 || strcmp(name, "json_decode") == 0 ||
           strcmp(name, "file_exists") == 0 || strcmp(name, "file_list") == 0 ||
           strcmp(name, "file_delete") == 0 || strcmp(name, "file_append") == 0 ||
           strcmp(name, "delay") == 0 || strcmp(name, "interval") == 0 ||
           strcmp(name, "llm_complete") == 0 ||
           strcmp(name, "string_split") == 0 || strcmp(name, "string_trim") == 0 ||
           strcmp(name, "string_upper") == 0 || strcmp(name, "string_lower") == 0 ||
           strcmp(name, "string_starts_with") == 0 ||
           strcmp(name, "string_ends_with") == 0 ||
           strcmp(name, "string_truncate") == 0 ||
           /* Agent utilities */
           strcmp(name, "parse_gemma_calls") == 0 ||
           strcmp(name, "clean_json") == 0 ||
           strcmp(name, "strip_html") == 0 ||
           /* Process introspection */
           strcmp(name, "process_info") == 0 ||
           strcmp(name, "process_list") == 0 ||
           strcmp(name, "registered") == 0 ||
           /* LiveView HTTP/WS */
           strcmp(name, "http_listen") == 0 ||
           strcmp(name, "http_respond") == 0 ||
           strcmp(name, "ws_send") == 0 ||
           strcmp(name, "ws_close") == 0 ||
           strcmp(name, "ws_set_handler") == 0 ||
           strcmp(name, "live_js") == 0 ||
           /* WebSocket CLIENT (CDP / browser control) */
           strcmp(name, "wsc_connect") == 0 ||
           strcmp(name, "wsc_send") == 0 ||
           strcmp(name, "wsc_recv") == 0 ||
           strcmp(name, "wsc_close") == 0 ||
           strcmp(name, "chrome_launch") == 0 ||
           /* String + base64 helpers (2026-05-15) */
           strcmp(name, "string_index_of") == 0 ||
           strcmp(name, "base64_encode") == 0 ||
           strcmp(name, "base64_decode") == 0 ||
           /* Phase 15: Feature Expansion */
           strcmp(name, "query_parse") == 0 ||
           strcmp(name, "http_serve_file") == 0 ||
           strcmp(name, "pubsub_subscribe") == 0 ||
           strcmp(name, "pubsub_broadcast") == 0 ||
           strcmp(name, "pubsub_unsubscribe") == 0 ||
           strcmp(name, "telemetry_emit") == 0 ||
           strcmp(name, "telemetry_subscribe") == 0 ||
           strcmp(name, "telemetry_unsubscribe") == 0 ||
           strcmp(name, "breaker_new") == 0 ||
           strcmp(name, "breaker_call") == 0 ||
           strcmp(name, "breaker_state") == 0 ||
           strcmp(name, "llm_stream") == 0 ||
           strcmp(name, "ets_list") == 0 ||
           strcmp(name, "ets_count") == 0 ||
           /* PDF builtins */
           strcmp(name, "pdf_text") == 0 ||
           strcmp(name, "pdf_pages") == 0 ||
           strcmp(name, "pdf_meta") == 0 ||
           /* Phase 16: Interactive CLI primitives */
           strcmp(name, "read_line") == 0 ||
           strcmp(name, "print_inline") == 0 ||
           strcmp(name, "sys_exit") == 0 ||
           /* Phase 17: LLM streaming */
           strcmp(name, "http_post_stream") == 0 ||
           /* Phase 18: terminal introspection */
           strcmp(name, "term_cols") == 0 ||
           /* Phase 19: interactive picker */
           strcmp(name, "read_choice") == 0;
}

static int is_self_call(cg_ctx_t *ctx, node_t *n) {
    if (n->type != N_CALL) return 0;
    if (n->v.call.func->type != N_IDENT) return 0;
    return strcmp(n->v.call.func->v.sval, ctx->cur_func) == 0;
}

static int is_module_func(cg_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->nfuncs; i++)
        if (strcmp(ctx->func_names[i], name) == 0) return 1;
    return 0;
}

/* =========================================================================
 * Pre-scan: collect spawn sites
 * ========================================================================= */

static void scan_spawns(cg_ctx_t *ctx, node_t *n) {
    if (!n) return;
    if (n->type == N_SPAWN) {
        node_t *inner = n->v.spawn.expr;
        if (inner && inner->type == N_CALL && ctx->nspawns < 64) {
            spawn_info_t *sp = &ctx->spawns[ctx->nspawns];
            sp->id = ctx->nspawns;
            strncpy(sp->func_name, inner->v.call.func->v.sval, 127);
            sp->nargs = inner->v.call.nargs;
            sp->call_node = inner;
            ctx->nspawns++;
        }
        return;
    }
    switch (n->type) {
    case N_MODULE:
        for (int i = 0; i < n->v.mod.nfuns; i++) scan_spawns(ctx, n->v.mod.funs[i]);
        break;
    case N_FUN: scan_spawns(ctx, n->v.fun.body); break;
    case N_BLOCK:
        for (int i = 0; i < n->v.block.nstmts; i++) scan_spawns(ctx, n->v.block.stmts[i]);
        break;
    case N_ASSIGN: scan_spawns(ctx, n->v.assign.value); break;
    case N_CALL:
        for (int i = 0; i < n->v.call.nargs; i++) scan_spawns(ctx, n->v.call.args[i]);
        break;
    case N_SEND:
        scan_spawns(ctx, n->v.send.to);
        scan_spawns(ctx, n->v.send.msg);
        break;
    case N_RECEIVE:
        for (int i = 0; i < n->v.recv.nclauses; i++) scan_spawns(ctx, n->v.recv.clauses[i]);
        if (n->v.recv.after_body) scan_spawns(ctx, n->v.recv.after_body);
        if (n->v.recv.after_expr) scan_spawns(ctx, n->v.recv.after_expr);
        break;
    case N_CLAUSE:
        scan_spawns(ctx, n->v.clause.body);
        break;
    case N_IF:
        scan_spawns(ctx, n->v.iff.cond);
        scan_spawns(ctx, n->v.iff.then_b);
        scan_spawns(ctx, n->v.iff.else_b);
        break;
    case N_BINOP:
        scan_spawns(ctx, n->v.binop.left);
        scan_spawns(ctx, n->v.binop.right);
        break;
    case N_UNARY: scan_spawns(ctx, n->v.unary.operand); break;
    case N_PIPE:
        scan_spawns(ctx, n->v.pipe.val);
        scan_spawns(ctx, n->v.pipe.func);
        break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < n->v.coll.count; i++) scan_spawns(ctx, n->v.coll.items[i]);
        break;
    case N_FOR:
        scan_spawns(ctx, n->v.forloop.iter);
        scan_spawns(ctx, n->v.forloop.body);
        break;
    case N_MAP:
        for (int i = 0; i < n->v.map.count; i++) { scan_spawns(ctx, n->v.map.keys[i]); scan_spawns(ctx, n->v.map.vals[i]); }
        break;
    case N_TRY:
        scan_spawns(ctx, n->v.trycatch.body);
        scan_spawns(ctx, n->v.trycatch.catch_body);
        break;
    case N_CASE:
        scan_spawns(ctx, n->v.casex.subject);
        for (int i = 0; i < n->v.casex.nclauses; i++) {
            node_t *cl = n->v.casex.clauses[i];
            scan_spawns(ctx, cl->v.clause.body);
            if (cl->v.clause.guard) scan_spawns(ctx, cl->v.clause.guard);
        }
        break;
    case N_RANGE:
        scan_spawns(ctx, n->v.range.from);
        scan_spawns(ctx, n->v.range.to);
        break;
    case N_LIST_CONS:
        scan_spawns(ctx, n->v.cons.head);
        scan_spawns(ctx, n->v.cons.tail);
        break;
    default: break;
    }
}

/* =========================================================================
 * Pre-scan: collect lambda (anonymous function) sites
 * ========================================================================= */

/* Collect identifiers used in an expression */
static void collect_idents(node_t *n, char ids[][128], int *nids, int max) {
    if (!n) return;
    if (n->type == N_IDENT) {
        for (int i = 0; i < *nids; i++)
            if (strcmp(ids[i], n->v.sval) == 0) return;
        if (*nids < max) {
            strncpy(ids[*nids], n->v.sval, 127);
            (*nids)++;
        }
        return;
    }
    switch (n->type) {
    case N_BLOCK:
        for (int i = 0; i < n->v.block.nstmts; i++)
            collect_idents(n->v.block.stmts[i], ids, nids, max);
        break;
    case N_ASSIGN:
        collect_idents(n->v.assign.value, ids, nids, max);
        break;
    case N_CALL:
        /* Don't collect the function name itself — only args */
        for (int i = 0; i < n->v.call.nargs; i++)
            collect_idents(n->v.call.args[i], ids, nids, max);
        break;
    case N_BINOP:
        collect_idents(n->v.binop.left, ids, nids, max);
        collect_idents(n->v.binop.right, ids, nids, max);
        break;
    case N_UNARY: collect_idents(n->v.unary.operand, ids, nids, max); break;
    case N_SEND:
        collect_idents(n->v.send.to, ids, nids, max);
        collect_idents(n->v.send.msg, ids, nids, max);
        break;
    case N_IF:
        collect_idents(n->v.iff.cond, ids, nids, max);
        collect_idents(n->v.iff.then_b, ids, nids, max);
        collect_idents(n->v.iff.else_b, ids, nids, max);
        break;
    case N_PIPE:
        collect_idents(n->v.pipe.val, ids, nids, max);
        collect_idents(n->v.pipe.func, ids, nids, max);
        break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < n->v.coll.count; i++)
            collect_idents(n->v.coll.items[i], ids, nids, max);
        break;
    case N_FOR:
        collect_idents(n->v.forloop.iter, ids, nids, max);
        collect_idents(n->v.forloop.body, ids, nids, max);
        break;
    case N_MAP:
        for (int i = 0; i < n->v.map.count; i++) { collect_idents(n->v.map.keys[i], ids, nids, max); collect_idents(n->v.map.vals[i], ids, nids, max); }
        break;
    case N_TRY:
        collect_idents(n->v.trycatch.body, ids, nids, max);
        collect_idents(n->v.trycatch.catch_body, ids, nids, max);
        break;
    case N_CASE:
        collect_idents(n->v.casex.subject, ids, nids, max);
        for (int i = 0; i < n->v.casex.nclauses; i++) {
            node_t *cl = n->v.casex.clauses[i];
            collect_idents(cl->v.clause.body, ids, nids, max);
            if (cl->v.clause.guard) collect_idents(cl->v.clause.guard, ids, nids, max);
        }
        break;
    case N_RANGE:
        collect_idents(n->v.range.from, ids, nids, max);
        collect_idents(n->v.range.to, ids, nids, max);
        break;
    case N_LIST_CONS:
        collect_idents(n->v.cons.head, ids, nids, max);
        collect_idents(n->v.cons.tail, ids, nids, max);
        break;
    default: break;
    }
}

static void scan_lambdas(cg_ctx_t *ctx, node_t *n) {
    if (!n) return;
    /* Anonymous function: N_FUN with empty name */
    if (n->type == N_FUN && n->v.fun.name[0] == '\0' && ctx->nlambdas < 64) {
        lambda_info_t *li = &ctx->lambdas[ctx->nlambdas];
        li->id = ctx->nlambdas;
        li->node = n;
        /* Module-prefix lambda names so multi-module builds don't get
         * `_lambda_1` defined twice when two modules each have a lambda.
         * The previous unprefixed form worked only for single-module
         * compiles. */
        snprintf(li->gen_name, sizeof(li->gen_name), "_lambda_%s_%d", ctx->mod_name, li->id);

        /* Find free variables: identifiers in body that aren't parameters */
        char body_idents[64][128];
        int nbody_idents = 0;
        collect_idents(n->v.fun.body, body_idents, &nbody_idents, 64);

        li->ncaptures = 0;
        for (int i = 0; i < nbody_idents; i++) {
            int is_param = 0;
            for (int j = 0; j < n->v.fun.nparams; j++)
                if (strcmp(body_idents[i], n->v.fun.params[j]) == 0) { is_param = 1; break; }
            if (!is_param && li->ncaptures < 32)
                strncpy(li->captures[li->ncaptures++], body_idents[i], 127);
        }

        ctx->nlambdas++;
        /* Continue scanning lambda body for nested lambdas */
        scan_lambdas(ctx, n->v.fun.body);
        return;
    }
    switch (n->type) {
    case N_MODULE:
        for (int i = 0; i < n->v.mod.nfuns; i++) scan_lambdas(ctx, n->v.mod.funs[i]);
        break;
    case N_FUN: scan_lambdas(ctx, n->v.fun.body); break;
    case N_BLOCK:
        for (int i = 0; i < n->v.block.nstmts; i++) scan_lambdas(ctx, n->v.block.stmts[i]);
        break;
    case N_ASSIGN: scan_lambdas(ctx, n->v.assign.value); break;
    case N_CALL:
        scan_lambdas(ctx, n->v.call.func);
        for (int i = 0; i < n->v.call.nargs; i++) scan_lambdas(ctx, n->v.call.args[i]);
        break;
    case N_SEND:
        scan_lambdas(ctx, n->v.send.to);
        scan_lambdas(ctx, n->v.send.msg);
        break;
    case N_RECEIVE:
        for (int i = 0; i < n->v.recv.nclauses; i++) scan_lambdas(ctx, n->v.recv.clauses[i]);
        if (n->v.recv.after_body) scan_lambdas(ctx, n->v.recv.after_body);
        if (n->v.recv.after_expr) scan_lambdas(ctx, n->v.recv.after_expr);
        break;
    case N_CLAUSE: scan_lambdas(ctx, n->v.clause.body); break;
    case N_IF:
        scan_lambdas(ctx, n->v.iff.cond);
        scan_lambdas(ctx, n->v.iff.then_b);
        scan_lambdas(ctx, n->v.iff.else_b);
        break;
    case N_BINOP:
        scan_lambdas(ctx, n->v.binop.left);
        scan_lambdas(ctx, n->v.binop.right);
        break;
    case N_UNARY: scan_lambdas(ctx, n->v.unary.operand); break;
    case N_PIPE:
        scan_lambdas(ctx, n->v.pipe.val);
        scan_lambdas(ctx, n->v.pipe.func);
        break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < n->v.coll.count; i++) scan_lambdas(ctx, n->v.coll.items[i]);
        break;
    case N_FOR:
        scan_lambdas(ctx, n->v.forloop.iter);
        scan_lambdas(ctx, n->v.forloop.body);
        break;
    case N_MAP:
        for (int i = 0; i < n->v.map.count; i++) { scan_lambdas(ctx, n->v.map.keys[i]); scan_lambdas(ctx, n->v.map.vals[i]); }
        break;
    case N_TRY:
        scan_lambdas(ctx, n->v.trycatch.body);
        scan_lambdas(ctx, n->v.trycatch.catch_body);
        break;
    case N_CASE:
        scan_lambdas(ctx, n->v.casex.subject);
        for (int i = 0; i < n->v.casex.nclauses; i++) {
            node_t *cl = n->v.casex.clauses[i];
            scan_lambdas(ctx, cl->v.clause.body);
            if (cl->v.clause.guard) scan_lambdas(ctx, cl->v.clause.guard);
        }
        break;
    case N_RANGE:
        scan_lambdas(ctx, n->v.range.from);
        scan_lambdas(ctx, n->v.range.to);
        break;
    case N_LIST_CONS:
        scan_lambdas(ctx, n->v.cons.head);
        scan_lambdas(ctx, n->v.cons.tail);
        break;
    case N_SPAWN: scan_lambdas(ctx, n->v.spawn.expr); break;
    default: break;
    }
}

static int find_lambda_id(cg_ctx_t *ctx, node_t *node) {
    for (int i = 0; i < ctx->nlambdas; i++)
        if (ctx->lambdas[i].node == node) return i;
    return -1;
}

static int find_spawn_id(cg_ctx_t *ctx, node_t *call_node) {
    for (int i = 0; i < ctx->nspawns; i++)
        if (ctx->spawns[i].call_node == call_node) return i;
    return -1;
}

/* Check if function body contains self-recursive tail calls */
static int has_tail_calls(cg_ctx_t *ctx, node_t *n) {
    if (!n) return 0;
    switch (n->type) {
    case N_CALL:
        return is_self_call(ctx, n);
    case N_BLOCK:
        if (n->v.block.nstmts == 0) return 0;
        return has_tail_calls(ctx, n->v.block.stmts[n->v.block.nstmts - 1]);
    case N_RECEIVE:
        for (int i = 0; i < n->v.recv.nclauses; i++)
            if (has_tail_calls(ctx, n->v.recv.clauses[i])) return 1;
        return 0;
    case N_CLAUSE:
        return has_tail_calls(ctx, n->v.clause.body);
    case N_IF:
        return has_tail_calls(ctx, n->v.iff.then_b) ||
               has_tail_calls(ctx, n->v.iff.else_b);
    case N_CASE:
        /* A tail call inside any case arm body still produces a
         * `goto _tail` from emit_call when tail flag propagates in,
         * so the function header MUST emit the `_tail:` label. */
        for (int i = 0; i < n->v.casex.nclauses; i++)
            if (has_tail_calls(ctx, n->v.casex.clauses[i]->v.clause.body)) return 1;
        return 0;
    case N_TRY:
        return has_tail_calls(ctx, n->v.trycatch.body) ||
               has_tail_calls(ctx, n->v.trycatch.catch_body);
    default: return 0;
    }
}

/* =========================================================================
 * Preamble: includes, runtime helpers, builtins
 * ========================================================================= */

static void emit_preamble(cg_ctx_t *ctx) {
    FILE *f = ctx->out;
    fprintf(f, "/* Generated by swc */\n");
    fprintf(f, "#include \"swarmrt_native.h\"\n");
    fprintf(f, "#include \"swarmrt_lang.h\"\n");
    fprintf(f, "#include \"swarmrt_ets.h\"\n");
    fprintf(f, "#include \"swarmrt_builtins_studio.h\"\n");
    fprintf(f, "#include <stdio.h>\n");
    fprintf(f, "#include <stdlib.h>\n");
    fprintf(f, "#include <string.h>\n");
    fprintf(f, "#include <unistd.h>\n");
    fprintf(f, "#include <sys/stat.h>\n");
    fprintf(f, "#include <sys/time.h>\n");
    fprintf(f, "#include <errno.h>\n\n");

    fprintf(f, "__thread sw_val_t *_sw_error = NULL;\n");
    /* Runtime line / file tracking — codegen emits
     *   `_sw_current_line = N; _sw_current_file = \"src/Mod.sw\";`
     * once per source line so panic() / expect() / failing builtins
     * can report where in the .sw source the program was running.
     * Cheap (two stores per source line, only when the line changes). */
    fprintf(f, "__thread int _sw_current_line = 0;\n");
    fprintf(f, "__thread const char *_sw_current_file = \"<unknown>\";\n");

    /* Call-stack ring buffer for stack traces. The typedef `_sw_frame_t`
     * comes from builtins_studio.h (included above); we just define
     * the thread-locals + push/pop helpers here. Capacity 64 frames —
     * deeper recursion sets the overflow flag rather than blowing up. */
    fprintf(f, "__thread _sw_frame_t _sw_trace[64];\n");
    fprintf(f, "__thread int _sw_trace_top = 0;\n");
    fprintf(f, "__thread int _sw_trace_overflowed = 0;\n");
    fprintf(f, "static inline void _sw_trace_push(const char *m, const char *fn, int line) {\n"
               "    if (_sw_trace_top < 64) {\n"
               "        _sw_trace[_sw_trace_top].module_name = m;\n"
               "        _sw_trace[_sw_trace_top].fn_name = fn;\n"
               "        _sw_trace[_sw_trace_top].line = line;\n"
               "    } else { _sw_trace_overflowed = 1; }\n"
               "    _sw_trace_top++;\n"
               "}\n");
    fprintf(f, "static inline void _sw_trace_pop(void) { if (_sw_trace_top > 0) _sw_trace_top--; }\n\n");

    /* Binary operation helpers */
    fprintf(f,
        "static sw_val_t *_op_add(sw_val_t *a, sw_val_t *b) {\n"
        "    if (a->type == SW_VAL_INT && b->type == SW_VAL_INT)\n"
        "        return sw_val_int(a->v.i + b->v.i);\n"
        "    double fa = a->type == SW_VAL_INT ? (double)a->v.i : a->v.f;\n"
        "    double fb = b->type == SW_VAL_INT ? (double)b->v.i : b->v.f;\n"
        "    return sw_val_float(fa + fb);\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_op_sub(sw_val_t *a, sw_val_t *b) {\n"
        "    if (a->type == SW_VAL_INT && b->type == SW_VAL_INT)\n"
        "        return sw_val_int(a->v.i - b->v.i);\n"
        "    double fa = a->type == SW_VAL_INT ? (double)a->v.i : a->v.f;\n"
        "    double fb = b->type == SW_VAL_INT ? (double)b->v.i : b->v.f;\n"
        "    return sw_val_float(fa - fb);\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_op_mul(sw_val_t *a, sw_val_t *b) {\n"
        "    if (a->type == SW_VAL_INT && b->type == SW_VAL_INT)\n"
        "        return sw_val_int(a->v.i * b->v.i);\n"
        "    double fa = a->type == SW_VAL_INT ? (double)a->v.i : a->v.f;\n"
        "    double fb = b->type == SW_VAL_INT ? (double)b->v.i : b->v.f;\n"
        "    return sw_val_float(fa * fb);\n"
        "}\n\n");

    /* Division panics on zero divisor. Was silently returning nil,
     * which led to spurious "use of nil" cascades downstream. Now you
     * get the exact line where the bad division happened. */
    fprintf(f,
        "static sw_val_t *_op_div(sw_val_t *a, sw_val_t *b) {\n"
        "    if (a->type == SW_VAL_INT && b->type == SW_VAL_INT) {\n"
        "        if (b->v.i == 0) _sw_runtime_panic(\"division by zero\");\n"
        "        return sw_val_int(a->v.i / b->v.i);\n"
        "    }\n"
        "    double fa = a->type == SW_VAL_INT ? (double)a->v.i : a->v.f;\n"
        "    double fb = b->type == SW_VAL_INT ? (double)b->v.i : b->v.f;\n"
        "    if (fb == 0.0) _sw_runtime_panic(\"division by zero\");\n"
        "    return sw_val_float(fa / fb);\n"
        "}\n\n");

    /* Modulo (`%`). Same divide-by-zero discipline as `/`. */
    fprintf(f,
        "#include <math.h>\n"
        "static sw_val_t *_op_mod(sw_val_t *a, sw_val_t *b) {\n"
        "    if (a->type == SW_VAL_INT && b->type == SW_VAL_INT) {\n"
        "        if (b->v.i == 0) _sw_runtime_panic(\"modulo by zero\");\n"
        "        return sw_val_int(a->v.i %% b->v.i);\n"
        "    }\n"
        "    double fa = a->type == SW_VAL_INT ? (double)a->v.i : a->v.f;\n"
        "    double fb = b->type == SW_VAL_INT ? (double)b->v.i : b->v.f;\n"
        "    if (fb == 0.0) _sw_runtime_panic(\"modulo by zero\");\n"
        "    return sw_val_float(fmod(fa, fb));\n"
        "}\n\n");

    /* `++` is polymorphic: list ++ list → concatenated list (Erlang-style),
     * otherwise string-concat with auto-coercion of ints/floats/atoms.
     * Without the list path, Std.sw helpers like `reverse`/`flatten`/
     * `intersperse` that build with `[x] ++ rest` silently produced
     * empty strings instead of the expected lists. */
    fprintf(f,
        "static sw_val_t *_op_concat(sw_val_t *a, sw_val_t *b) {\n"
        "    /* List ++ list — preserve list semantics. */\n"
        "    if (a && b && a->type == SW_VAL_LIST && b->type == SW_VAL_LIST) {\n"
        "        int la = a->v.tuple.count, lb = b->v.tuple.count;\n"
        "        if (la == 0) return b;\n"
        "        if (lb == 0) return a;\n"
        "        sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * (la + lb));\n"
        "        for (int i = 0; i < la; i++) items[i] = a->v.tuple.items[i];\n"
        "        for (int i = 0; i < lb; i++) items[la + i] = b->v.tuple.items[i];\n"
        "        sw_val_t *r = sw_val_list(items, la + lb);\n"
        "        free(items);\n"
        "        return r;\n"
        "    }\n"
        "    /* String concat with int/float/atom auto-coercion. */\n"
        "    size_t alen = 0, blen = 0;\n"
        "    char ta[64] = \"\", tb[64] = \"\";\n"
        "    const char *sa = ta, *sb = tb;\n"
        "    if (a->type == SW_VAL_STRING) { sa = a->v.str; alen = strlen(sa); }\n"
        "    else if (a->type == SW_VAL_ATOM) { sa = a->v.str; alen = strlen(sa); }\n"
        "    else if (a->type == SW_VAL_INT) { snprintf(ta,64,\"%%lld\",(long long)a->v.i); alen = strlen(ta); }\n"
        "    else if (a->type == SW_VAL_FLOAT) { snprintf(ta,64,\"%%g\",a->v.f); alen = strlen(ta); }\n"
        "    if (b->type == SW_VAL_STRING) { sb = b->v.str; blen = strlen(sb); }\n"
        "    else if (b->type == SW_VAL_ATOM) { sb = b->v.str; blen = strlen(sb); }\n"
        "    else if (b->type == SW_VAL_INT) { snprintf(tb,64,\"%%lld\",(long long)b->v.i); blen = strlen(tb); }\n"
        "    else if (b->type == SW_VAL_FLOAT) { snprintf(tb,64,\"%%g\",b->v.f); blen = strlen(tb); }\n"
        "    char *buf = (char *)malloc(alen + blen + 1);\n"
        "    memcpy(buf, sa, alen);\n"
        "    memcpy(buf + alen, sb, blen);\n"
        "    buf[alen + blen] = '\\0';\n"
        "    sw_val_t *r = sw_val_string(buf);\n"
        "    free(buf);\n"
        "    return r;\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_op_cmp(sw_val_t *a, sw_val_t *b, const char *op) {\n"
        "    if (a->type == SW_VAL_INT && b->type == SW_VAL_INT) {\n"
        "        int64_t x = a->v.i, y = b->v.i;\n"
        "        int r = 0;\n"
        "        if (op[0]=='=' && op[1]=='=') r = (x == y);\n"
        "        else if (op[0]=='!' && op[1]=='=') r = (x != y);\n"
        "        else if (op[0]=='<' && op[1]=='=') r = (x <= y);\n"
        "        else if (op[0]=='>' && op[1]=='=') r = (x >= y);\n"
        "        else if (op[0]=='<') r = (x < y);\n"
        "        else if (op[0]=='>') r = (x > y);\n"
        "        return sw_val_atom(r ? \"true\" : \"false\");\n"
        "    }\n"
        "    if (op[0]=='=' && op[1]=='=') return sw_val_atom(sw_val_equal(a, b) ? \"true\" : \"false\");\n"
        "    if (op[0]=='!' && op[1]=='=') return sw_val_atom(!sw_val_equal(a, b) ? \"true\" : \"false\");\n"
        "    /* Ordered comparisons on float/int mix and float/float —\n"
        "     * coerce both to double and compare. Without this, `0.5 > 0.1`\n"
        "     * fell through to 'false' silently (only int<int worked). */\n"
        "    if ((a->type == SW_VAL_INT || a->type == SW_VAL_FLOAT) &&\n"
        "        (b->type == SW_VAL_INT || b->type == SW_VAL_FLOAT)) {\n"
        "        double x = (a->type == SW_VAL_INT) ? (double)a->v.i : a->v.f;\n"
        "        double y = (b->type == SW_VAL_INT) ? (double)b->v.i : b->v.f;\n"
        "        int r = 0;\n"
        "        if (op[0]=='<' && op[1]=='=') r = (x <= y);\n"
        "        else if (op[0]=='>' && op[1]=='=') r = (x >= y);\n"
        "        else if (op[0]=='<') r = (x < y);\n"
        "        else if (op[0]=='>') r = (x > y);\n"
        "        return sw_val_atom(r ? \"true\" : \"false\");\n"
        "    }\n"
        "    /* String ordered compare — lexicographic. */\n"
        "    if (a->type == SW_VAL_STRING && b->type == SW_VAL_STRING) {\n"
        "        int c = strcmp(a->v.str, b->v.str);\n"
        "        int r = 0;\n"
        "        if (op[0]=='<' && op[1]=='=') r = (c <= 0);\n"
        "        else if (op[0]=='>' && op[1]=='=') r = (c >= 0);\n"
        "        else if (op[0]=='<') r = (c < 0);\n"
        "        else if (op[0]=='>') r = (c > 0);\n"
        "        return sw_val_atom(r ? \"true\" : \"false\");\n"
        "    }\n"
        "    return sw_val_atom(\"false\");\n"
        "}\n\n");

    /* Builtins */
    fprintf(f,
        "static sw_val_t *_builtin_print(sw_val_t **a, int n) {\n"
        "    for (int i = 0; i < n; i++) { if (i) printf(\" \"); sw_val_print(a[i]); }\n"
        "    printf(\"\\n\"); fflush(stdout); return sw_val_atom(\"ok\");\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_length(sw_val_t **a, int n) {\n"
        "    if (n < 1) return sw_val_int(0);\n"
        "    if (a[0]->type == SW_VAL_LIST || a[0]->type == SW_VAL_TUPLE)\n"
        "        return sw_val_int(a[0]->v.tuple.count);\n"
        "    if (a[0]->type == SW_VAL_STRING) return sw_val_int((int64_t)strlen(a[0]->v.str));\n"
        "    if (a[0]->type == SW_VAL_MAP) return sw_val_int(a[0]->v.map.count);\n"
        "    return sw_val_int(0);\n"
        "}\n\n");

    /* hd / tl / elem fail loudly via runtime panic. These three are
     * unambiguously bugs: nobody MEANS to call hd on an empty list or
     * index past the end of a tuple. Silent nil returns were burying
     * mistakes deep in subsequent operations. (map_get / ets_get stay
     * lenient — "optional lookup" is a genuine use case there.) */
    fprintf(f,
        "static sw_val_t *_builtin_hd(sw_val_t **a, int n) {\n"
        "    if (n < 1 || !a[0]) _sw_runtime_panic(\"hd: no list given\");\n"
        "    if (a[0]->type != SW_VAL_LIST) _sw_runtime_panic(\"hd: not a list (got type %%d)\", a[0]->type);\n"
        "    if (a[0]->v.tuple.count == 0) _sw_runtime_panic(\"hd: list is empty\");\n"
        "    return a[0]->v.tuple.items[0];\n"
        "}\n\n");

    /* tl of a 1-element list returning [] is intentional (matches Erlang). */
    fprintf(f,
        "static sw_val_t *_builtin_tl(sw_val_t **a, int n) {\n"
        "    if (n < 1 || !a[0]) _sw_runtime_panic(\"tl: no list given\");\n"
        "    if (a[0]->type != SW_VAL_LIST) _sw_runtime_panic(\"tl: not a list (got type %%d)\", a[0]->type);\n"
        "    if (a[0]->v.tuple.count == 0) _sw_runtime_panic(\"tl: list is empty\");\n"
        "    if (a[0]->v.tuple.count == 1) return sw_val_list(NULL, 0);\n"
        "    return sw_val_list(a[0]->v.tuple.items + 1, a[0]->v.tuple.count - 1);\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_elem(sw_val_t **a, int n) {\n"
        "    if (n < 2 || !a[0] || !a[1]) _sw_runtime_panic(\"elem: needs (tuple, index)\");\n"
        "    if (a[0]->type != SW_VAL_TUPLE) _sw_runtime_panic(\"elem: not a tuple (got type %%d)\", a[0]->type);\n"
        "    if (a[1]->type != SW_VAL_INT) _sw_runtime_panic(\"elem: index must be int\");\n"
        "    int idx = (int)a[1]->v.i;\n"
        "    if (idx < 0 || idx >= a[0]->v.tuple.count)\n"
        "        _sw_runtime_panic(\"elem: index %%d out of range for %%d-tuple\", idx, a[0]->v.tuple.count);\n"
        "    return a[0]->v.tuple.items[idx];\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_abs(sw_val_t **a, int n) {\n"
        "    if (n < 1 || a[0]->type != SW_VAL_INT) return sw_val_nil();\n"
        "    return sw_val_int(a[0]->v.i < 0 ? -a[0]->v.i : a[0]->v.i);\n"
        "}\n\n");

    /* Functional primitives: map, pmap, reduce, filter */
    fprintf(f,
        "static sw_val_t *_builtin_map(sw_val_t **a, int n) {\n"
        "    if (n < 2) return sw_val_list(NULL, 0);\n"
        "    sw_val_t *fn = a[0];\n"
        "    sw_val_t *lst = a[1];\n"
        "    if (!lst || lst->type != SW_VAL_LIST) return sw_val_list(NULL, 0);\n"
        "    int cnt = lst->v.tuple.count;\n"
        "    if (cnt == 0) return sw_val_list(NULL, 0);\n"
        "    sw_val_t **res = malloc(sizeof(sw_val_t*) * cnt);\n"
        "    for (int i = 0; i < cnt; i++) {\n"
        "        sw_val_t *arg[] = {lst->v.tuple.items[i]};\n"
        "        res[i] = sw_val_apply(fn, arg, 1);\n"
        "    }\n"
        "    sw_val_t *r = sw_val_list(res, cnt);\n"
        "    free(res);\n"
        "    return r;\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_reduce(sw_val_t **a, int n) {\n"
        "    if (n < 3) return sw_val_nil();\n"
        "    sw_val_t *fn = a[0];\n"
        "    sw_val_t *lst = a[1];\n"
        "    sw_val_t *acc = a[2];\n"
        "    if (!lst || lst->type != SW_VAL_LIST) return acc;\n"
        "    for (int i = 0; i < lst->v.tuple.count; i++) {\n"
        "        sw_val_t *args[] = {acc, lst->v.tuple.items[i]};\n"
        "        acc = sw_val_apply(fn, args, 2);\n"
        "    }\n"
        "    return acc;\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_filter(sw_val_t **a, int n) {\n"
        "    if (n < 2) return sw_val_list(NULL, 0);\n"
        "    sw_val_t *fn = a[0];\n"
        "    sw_val_t *lst = a[1];\n"
        "    if (!lst || lst->type != SW_VAL_LIST) return sw_val_list(NULL, 0);\n"
        "    int cnt = lst->v.tuple.count;\n"
        "    sw_val_t **res = malloc(sizeof(sw_val_t*) * (cnt > 0 ? cnt : 1));\n"
        "    int nr = 0;\n"
        "    for (int i = 0; i < cnt; i++) {\n"
        "        sw_val_t *arg[] = {lst->v.tuple.items[i]};\n"
        "        sw_val_t *keep = sw_val_apply(fn, arg, 1);\n"
        "        if (sw_val_is_truthy(keep)) res[nr++] = lst->v.tuple.items[i];\n"
        "    }\n"
        "    sw_val_t *r = sw_val_list(res, nr);\n"
        "    free(res);\n"
        "    return r;\n"
        "}\n\n");

    /* pmap: parallel map — spawns a process per list item */
    fprintf(f,
        "typedef struct { sw_val_t *fn; sw_val_t *item; sw_process_t *caller; int idx; } _pmap_w_t;\n"
        "static void _pmap_entry(void *raw) {\n"
        "    _pmap_w_t *w = (_pmap_w_t *)raw;\n"
        "    sw_val_t *arg[] = {w->item};\n"
        "    sw_val_t *result = sw_val_apply(w->fn, arg, 1);\n"
        "    sw_val_t *ti[] = {sw_val_int(w->idx), result};\n"
        "    sw_val_t *msg = sw_val_tuple(ti, 2);\n"
        "    sw_send_tagged(w->caller, 11, msg);\n"
        "    free(w);\n"
        "}\n"
        "static sw_val_t *_builtin_pmap(sw_val_t **a, int n) {\n"
        "    if (n < 2) return sw_val_list(NULL, 0);\n"
        "    sw_val_t *fn = a[0];\n"
        "    sw_val_t *lst = a[1];\n"
        "    if (!lst || lst->type != SW_VAL_LIST) return sw_val_list(NULL, 0);\n"
        "    int cnt = lst->v.tuple.count;\n"
        "    if (cnt == 0) return sw_val_list(NULL, 0);\n"
        "    sw_process_t *self = sw_self();\n"
        "    sw_val_t **res = calloc(cnt, sizeof(sw_val_t*));\n"
        "    for (int i = 0; i < cnt; i++) {\n"
        "        _pmap_w_t *w = malloc(sizeof(_pmap_w_t));\n"
        "        w->fn = fn; w->item = lst->v.tuple.items[i];\n"
        "        w->caller = self; w->idx = i;\n"
        "        sw_spawn(_pmap_entry, w);\n"
        "    }\n"
        "    for (int i = 0; i < cnt; i++) {\n"
        "        uint64_t tag;\n"
        "        void *raw = sw_receive_any(5000, &tag);\n"
        "        if (raw) {\n"
        "            sw_val_t *m = (sw_val_t *)raw;\n"
        "            if (m->type == SW_VAL_TUPLE && m->v.tuple.count == 2\n"
        "                && m->v.tuple.items[0]->type == SW_VAL_INT) {\n"
        "                int ix = (int)m->v.tuple.items[0]->v.i;\n"
        "                if (ix >= 0 && ix < cnt) res[ix] = m->v.tuple.items[1];\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    for (int i = 0; i < cnt; i++)\n"
        "        if (!res[i]) res[i] = sw_val_nil();\n"
        "    sw_val_t *r = sw_val_list(res, cnt);\n"
        "    free(res);\n"
        "    return r;\n"
        "}\n\n");
}

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

static void emit_forward_decls(cg_ctx_t *ctx, node_t *mod) {
    FILE *f = ctx->out;
    fprintf(f, "/* === Forward declarations === */\n");
    for (int i = 0; i < mod->v.mod.nfuns; i++) {
        fprintf(f, "static sw_val_t *%s_%s(sw_val_t **_args, int _nargs);\n",
                ctx->mod_name, mod->v.mod.funs[i]->v.fun.name);
    }
    fprintf(f, "\n");
}

/* =========================================================================
 * Spawn trampolines
 * ========================================================================= */

static void emit_spawn_trampolines(cg_ctx_t *ctx) {
    FILE *f = ctx->out;
    if (ctx->nspawns == 0) return;
    fprintf(f, "/* === Spawn trampolines === */\n");
    for (int i = 0; i < ctx->nspawns; i++) {
        spawn_info_t *sp = &ctx->spawns[i];
        /* Struct for captured args. Names are module-prefixed so multiple
         * modules in a multi-module build don't collide on _sp0_t etc. */
        fprintf(f, "typedef struct {");
        for (int a = 0; a < sp->nargs; a++)
            fprintf(f, " sw_val_t *a%d;", a);
        fprintf(f, " } _%s_sp%d_t;\n", ctx->mod_name, sp->id);

        /* Entry function */
        fprintf(f, "static void _%s_sp%d_entry(void *_raw) {\n", ctx->mod_name, sp->id);
        fprintf(f, "    _%s_sp%d_t *_s = (_%s_sp%d_t *)_raw;\n",
                ctx->mod_name, sp->id, ctx->mod_name, sp->id);
        if (sp->nargs > 0) {
            fprintf(f, "    sw_val_t *_a[] = {");
            for (int a = 0; a < sp->nargs; a++) {
                if (a) fprintf(f, ", ");
                fprintf(f, "_s->a%d", a);
            }
            fprintf(f, "};\n");
            fprintf(f, "    %s_%s(_a, %d);\n", ctx->mod_name, sp->func_name, sp->nargs);
        } else {
            fprintf(f, "    %s_%s(NULL, 0);\n", ctx->mod_name, sp->func_name);
        }
        fprintf(f, "    free(_s);\n");
        fprintf(f, "}\n\n");
    }
}

/* =========================================================================
 * Lambda (anonymous function) emission
 * ========================================================================= */

/* Forward declaration for emit_expr */
static void emit_expr(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz);

static void emit_lambda_functions(cg_ctx_t *ctx) {
    FILE *f = ctx->out;
    if (ctx->nlambdas == 0) return;
    fprintf(f, "/* === Lambda functions === */\n");
    for (int i = 0; i < ctx->nlambdas; i++)
        fprintf(f, "static sw_val_t *%s(sw_val_t **_args, int _nargs);\n", ctx->lambdas[i].gen_name);
    fprintf(f, "\n");
    for (int i = 0; i < ctx->nlambdas; i++) {
        lambda_info_t *li = &ctx->lambdas[i];
        node_t *fn = li->node;

        /* Lambda function signature: same as regular functions */
        fprintf(f, "static sw_val_t *%s(sw_val_t **_args, int _nargs) {\n", li->gen_name);

        /* Save/restore codegen context */
        char save_func[128];
        char save_params[16][128];
        int save_nparams = ctx->cur_nparams;
        int save_ndeclared = ctx->ndeclared;
        int save_has_tail = ctx->has_tail;
        strncpy(save_func, ctx->cur_func, 127);
        for (int j = 0; j < save_nparams; j++)
            strncpy(save_params[j], ctx->cur_params[j], 127);

        strncpy(ctx->cur_func, li->gen_name, 127);
        ctx->cur_nparams = fn->v.fun.nparams;
        ctx->ndeclared = 0;
        ctx->has_tail = 0;

        /* Extract explicit parameters */
        for (int j = 0; j < fn->v.fun.nparams; j++) {
            strncpy(ctx->cur_params[j], fn->v.fun.params[j], 127);
            fprintf(f, "    sw_val_t *%s = _nargs > %d ? _args[%d] : sw_val_nil();\n",
                    mangle_for_c(fn->v.fun.params[j]), j, j);
            declare_var(ctx, fn->v.fun.params[j]);
        }

        /* Extract captured variables (passed as extra args after explicit params) */
        for (int j = 0; j < li->ncaptures; j++) {
            fprintf(f, "    sw_val_t *%s = _nargs > %d ? _args[%d] : sw_val_nil();\n",
                    mangle_for_c(li->captures[j]), fn->v.fun.nparams + j, fn->v.fun.nparams + j);
            declare_var(ctx, li->captures[j]);
        }

        if (fn->v.fun.nparams == 0 && li->ncaptures == 0)
            fprintf(f, "    (void)_args; (void)_nargs;\n");

        /* Push trace frame for the lambda — uses the lambda's gen_name
         * since anonymous functions have no source name. */
        fprintf(f, "    _sw_trace_push(\"%s\", \"%s\", %d);\n",
                ctx->mod_name, li->gen_name, fn->line);

        /* Body */
        char result[32];
        emit_expr(ctx, fn->v.fun.body, 1, result, sizeof(result));
        fprintf(f, "    _sw_trace_pop();\n");
        if (result[0])
            fprintf(f, "    return %s;\n", result);
        else
            fprintf(f, "    return sw_val_nil();\n");
        fprintf(f, "}\n\n");

        /* Restore context */
        strncpy(ctx->cur_func, save_func, 127);
        ctx->cur_nparams = save_nparams;
        ctx->ndeclared = save_ndeclared;
        ctx->has_tail = save_has_tail;
        for (int j = 0; j < save_nparams; j++)
            strncpy(ctx->cur_params[j], save_params[j], 127);
    }
}

/* =========================================================================
 * Pattern matching
 * ========================================================================= */

static void emit_pattern_cond(cg_ctx_t *ctx, node_t *pat, const char *val) {
    FILE *f = ctx->out;
    (void)ctx;
    switch (pat->type) {
    case N_ATOM:
        /* Special case: the pattern `nil` should match the runtime
         * nil value (SW_VAL_NIL) AND the literal atom `'nil'`, which
         * sw_val_equal() already treats as equivalent at the value
         * level. Without this, `case x { nil -> ... }` silently misses
         * when x is a real nil (e.g. a map_get on a missing key). */
        if (strcmp(pat->v.sval, "nil") == 0) {
            fprintf(f, "(%s->type == SW_VAL_NIL || (%s->type == SW_VAL_ATOM && strcmp(%s->v.str, \"nil\") == 0))",
                    val, val, val);
        } else {
            fprintf(f, "%s->type == SW_VAL_ATOM && strcmp(%s->v.str, \"%s\") == 0",
                    val, val, pat->v.sval);
        }
        break;
    case N_INT:
        fprintf(f, "%s->type == SW_VAL_INT && %s->v.i == %lldLL",
                val, val, (long long)pat->v.ival);
        break;
    case N_FLOAT:
        fprintf(f, "%s->type == SW_VAL_FLOAT && %s->v.f == %.17g",
                val, val, pat->v.fval);
        break;
    case N_STRING: {
        char *esc = c_escape_str(pat->v.sval);
        fprintf(f, "%s->type == SW_VAL_STRING && strcmp(%s->v.str, \"%s\") == 0",
                val, val, esc);
        free(esc);
        break;
    }
    case N_IDENT:
        /* Variable: always matches */
        fprintf(f, "1");
        break;
    case N_TUPLE:
        fprintf(f, "(%s->type == SW_VAL_TUPLE && %s->v.tuple.count == %d",
                val, val, pat->v.coll.count);
        for (int i = 0; i < pat->v.coll.count; i++) {
            char item[128];
            snprintf(item, sizeof(item), "%s->v.tuple.items[%d]", val, i);
            fprintf(f, " && ");
            emit_pattern_cond(ctx, pat->v.coll.items[i], item);
        }
        fprintf(f, ")");
        break;
    case N_LIST:
        fprintf(f, "(%s->type == SW_VAL_LIST && %s->v.tuple.count == %d",
                val, val, pat->v.coll.count);
        for (int i = 0; i < pat->v.coll.count; i++) {
            char item[128];
            snprintf(item, sizeof(item), "%s->v.tuple.items[%d]", val, i);
            fprintf(f, " && ");
            emit_pattern_cond(ctx, pat->v.coll.items[i], item);
        }
        fprintf(f, ")");
        break;
    case N_LIST_CONS:
        /* [h | t] pattern: list with at least 1 element */
        fprintf(f, "(%s->type == SW_VAL_LIST && %s->v.tuple.count >= 1", val, val);
        /* Check head pattern */
        {
            char head_item[128];
            snprintf(head_item, sizeof(head_item), "%s->v.tuple.items[0]", val);
            fprintf(f, " && ");
            emit_pattern_cond(ctx, pat->v.cons.head, head_item);
        }
        fprintf(f, ")");
        break;
    default:
        fprintf(f, "1");
        break;
    }
}

static void emit_pattern_bind(cg_ctx_t *ctx, node_t *pat, const char *val) {
    FILE *f = ctx->out;
    switch (pat->type) {
    case N_IDENT:
        /* Always emit a fresh declaration (scoped to the if-block).
         * Also track it so the clause body can reference it. */
        fprintf(f, "        sw_val_t *%s = %s;\n", mangle_for_c(pat->v.sval), val);
        declare_var(ctx, pat->v.sval);
        break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < pat->v.coll.count; i++) {
            char item[128];
            snprintf(item, sizeof(item), "%s->v.tuple.items[%d]", val, i);
            emit_pattern_bind(ctx, pat->v.coll.items[i], item);
        }
        break;
    case N_LIST_CONS: {
        char head_item[128];
        snprintf(head_item, sizeof(head_item), "%s->v.tuple.items[0]", val);
        emit_pattern_bind(ctx, pat->v.cons.head, head_item);
        /* Bind tail: rest of list */
        if (pat->v.cons.tail->type == N_IDENT) {
            fprintf(f, "        sw_val_t *%s = sw_val_list(%s->v.tuple.items + 1, %s->v.tuple.count - 1);\n",
                    mangle_for_c(pat->v.cons.tail->v.sval), val, val);
            declare_var(ctx, pat->v.cons.tail->v.sval);
        }
        break;
    }
    default: break;
    }
}

/* =========================================================================
 * Expression emission
 * ========================================================================= */

static void emit_binop(cg_ctx_t *ctx, node_t *n, char *out, int osz) {
    FILE *f = ctx->out;
    char left[32], right[32], res[32];
    const char *op = n->v.binop.op;

    /* Short-circuit && / || — the right operand must NOT be evaluated
     * when the left already decides the result. The naive emitter
     * flattened both operands into statements before combining, so a
     * guard like `x != nil && elem(x, 0) == 'p'` always ran elem(x,0)
     * and panicked when x was nil. We special-case the logical ops:
     * emit the left, declare the result temp seeded with the
     * short-circuit value, then emit the right operand's statements
     * INSIDE a conditional block so they only run when needed.
     * sw's && / || yield a boolean atom ('true' / 'false'). */
    if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
        int is_and = (op[0] == '&');
        emit_expr(ctx, n->v.binop.left, 0, left, sizeof(left));
        fresh_var(ctx, res, sizeof(res));
        /* Seed with the value the short-circuit path yields:
         *   &&  → left falsy  ⇒ "false"
         *   ||  → left truthy ⇒ "true"  */
        fprintf(f, "    sw_val_t *%s = sw_val_atom(%s);\n",
                res, is_and ? "\"false\"" : "\"true\"");
        fprintf(f, "    if (%ssw_val_is_truthy(%s)) {\n",
                is_and ? "" : "!", left);
        emit_expr(ctx, n->v.binop.right, 0, right, sizeof(right));
        fprintf(f, "    %s = sw_val_atom(sw_val_is_truthy(%s) ? \"true\" : \"false\");\n",
                res, right);
        fprintf(f, "    }\n");
        strncpy(out, res, osz - 1);
        return;
    }

    emit_expr(ctx, n->v.binop.left, 0, left, sizeof(left));
    emit_expr(ctx, n->v.binop.right, 0, right, sizeof(right));
    fresh_var(ctx, res, sizeof(res));

    if (strcmp(op, "+") == 0)
        fprintf(f, "    sw_val_t *%s = _op_add(%s, %s);\n", res, left, right);
    else if (strcmp(op, "-") == 0)
        fprintf(f, "    sw_val_t *%s = _op_sub(%s, %s);\n", res, left, right);
    else if (strcmp(op, "*") == 0)
        fprintf(f, "    sw_val_t *%s = _op_mul(%s, %s);\n", res, left, right);
    else if (strcmp(op, "/") == 0)
        fprintf(f, "    sw_val_t *%s = _op_div(%s, %s);\n", res, left, right);
    else if (strcmp(op, "%") == 0)
        fprintf(f, "    sw_val_t *%s = _op_mod(%s, %s);\n", res, left, right);
    else if (strcmp(op, "++") == 0)
        fprintf(f, "    sw_val_t *%s = _op_concat(%s, %s);\n", res, left, right);
    else if (strcmp(op, "&&") == 0)
        fprintf(f, "    sw_val_t *%s = sw_val_atom(sw_val_is_truthy(%s) && sw_val_is_truthy(%s) ? \"true\" : \"false\");\n",
                res, left, right);
    else if (strcmp(op, "||") == 0)
        fprintf(f, "    sw_val_t *%s = sw_val_atom(sw_val_is_truthy(%s) || sw_val_is_truthy(%s) ? \"true\" : \"false\");\n",
                res, left, right);
    else
        fprintf(f, "    sw_val_t *%s = _op_cmp(%s, %s, \"%s\");\n", res, left, right, op);

    strncpy(out, res, osz - 1);
}

/* Curated list of common builtin names — used by did_you_mean.
 * Doesn't need to be exhaustive (it's only consulted on already-failed
 * lookups for hint generation), just covers the names a confused user
 * is most likely to reach for. New builtins can be added here at
 * leisure — missing one means the suggestion is less specific, not
 * that the compile breaks. */
static const char *_common_builtins[] = {
    "print", "print_inline", "length", "hd", "tl", "elem", "abs",
    "to_string", "format", "panic", "expect", "error", "typeof",
    "map", "pmap", "reduce", "filter", "list_append",
    "map_get", "map_put", "map_new", "map_keys", "map_values",
    "map_merge", "map_has_key", "map_size", "map_remove",
    "string_length", "string_sub", "string_split", "string_replace",
    "string_contains", "string_starts_with", "string_ends_with",
    "string_index_of", "string_trim", "string_upper", "string_lower",
    "string_truncate", "strip_html", "clean_json",
    "base64_encode", "base64_decode",
    "json_encode", "json_decode", "json_get", "json_escape",
    "spawn", "self", "send", "register", "whereis",
    "ets_new", "ets_put", "ets_get", "ets_delete", "ets_list", "ets_count",
    "file_read", "file_write", "file_exists", "file_delete", "file_list",
    "file_append", "file_mkdir",
    "http_get", "http_post", "http_post_stream", "http_listen", "http_respond",
    "ws_send", "ws_close", "ws_set_handler",
    "wsc_connect", "wsc_send", "wsc_recv", "wsc_close",
    "chrome_launch", "term_cols", "read_line", "read_char", "read_choice",
    "sleep", "timestamp", "getenv", "sys_exit", "shell",
    "random_int", "supervise",
    NULL
};

/* Tiny Levenshtein distance (capped at len ~64 since identifiers are
 * short). Standard DP, no allocation past two rows. */
static int _lev_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 63) la = 63;
    if (lb > 63) lb = 63;
    int prev[65], curr[65];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j-1] + 1;
            int sub = prev[j-1] + cost;
            int m = del < ins ? del : ins;
            if (sub < m) m = sub;
            curr[j] = m;
        }
        for (int j = 0; j <= lb; j++) prev[j] = curr[j];
    }
    return prev[lb];
}

/* Print "src/Mod.sw:N: unknown function 'foo' — did you mean 'foa'?"
 * to stderr. Threshold = lower of (3, len/2): keeps "string_xx" from
 * matching "abs". Up to 3 suggestions, sorted by distance. */
static void did_you_mean(cg_ctx_t *ctx, const char *fname, int line) {
    int flen = (int)strlen(fname);
    int threshold = flen / 2;
    if (threshold > 3) threshold = 3;
    if (threshold < 1) threshold = 1;

    /* Collect (name, distance) pairs from builtins + module funcs. */
    const char *cands[8] = { NULL };
    int dists[8];
    int ncands = 0;

    for (int i = 0; _common_builtins[i]; i++) {
        int d = _lev_distance(fname, _common_builtins[i]);
        if (d > threshold) continue;
        /* Insert into top-3 sorted list. */
        int slot = ncands;
        while (slot > 0 && dists[slot-1] > d) slot--;
        if (slot < 3) {
            for (int k = (ncands < 3 ? ncands : 2); k > slot; k--) {
                cands[k] = cands[k-1]; dists[k] = dists[k-1];
            }
            cands[slot] = _common_builtins[i];
            dists[slot] = d;
            if (ncands < 3) ncands++;
        }
    }
    for (int i = 0; i < ctx->nfuncs; i++) {
        int d = _lev_distance(fname, ctx->func_names[i]);
        if (d > threshold || d == 0) continue;
        int slot = ncands;
        while (slot > 0 && dists[slot-1] > d) slot--;
        if (slot < 3) {
            for (int k = (ncands < 3 ? ncands : 2); k > slot; k--) {
                cands[k] = cands[k-1]; dists[k] = dists[k-1];
            }
            cands[slot] = ctx->func_names[i];
            dists[slot] = d;
            if (ncands < 3) ncands++;
        }
    }

    fprintf(stderr, "swc: %s:%d: unknown function '%s'",
            guess_sw_path(ctx->mod_name), line > 0 ? line : 0, fname);
    if (ncands > 0) {
        fprintf(stderr, " — did you mean ");
        for (int i = 0; i < ncands; i++) {
            fprintf(stderr, "%s'%s'", i > 0 ? (i == ncands - 1 ? " or " : ", ") : "", cands[i]);
        }
        fprintf(stderr, "?");
    }
    fprintf(stderr, "\n");
}

static void emit_call(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz) {
    FILE *f = ctx->out;
    const char *fname = n->v.call.func->v.sval;
    int nargs = n->v.call.nargs;

    /* Evaluate arguments */
    char arg_vars[16][32];
    for (int i = 0; i < nargs && i < 16; i++)
        emit_expr(ctx, n->v.call.args[i], 0, arg_vars[i], sizeof(arg_vars[i]));

    /* Tail call optimization: self-recursive call in tail position */
    if (tail && is_self_call(ctx, n)) {
        for (int i = 0; i < nargs && i < ctx->cur_nparams; i++)
            fprintf(f, "    %s = %s;\n", ctx->cur_params[i], arg_vars[i]);
        fprintf(f, "    goto _tail;\n");
        out[0] = '\0';
        return;
    }

    /* Build argument array */
    char arr[32];
    fresh_var(ctx, arr, sizeof(arr));
    if (nargs > 0) {
        fprintf(f, "    sw_val_t *%s[] = {", arr);
        for (int i = 0; i < nargs; i++) {
            if (i) fprintf(f, ", ");
            fprintf(f, "%s", arg_vars[i]);
        }
        fprintf(f, "};\n");
    }

    char res[32];
    fresh_var(ctx, res, sizeof(res));

    /* Cross-module call: Module.function(args) → Module_function(args, n) */
    const char *dot = strchr(fname, '.');
    if (dot) {
        char mod[128], func[128];
        int mlen = (int)(dot - fname);
        strncpy(mod, fname, mlen); mod[mlen] = '\0';
        strncpy(func, dot + 1, sizeof(func) - 1);
        if (nargs > 0)
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, %d);\n", res, mod, func, arr, nargs);
        else
            fprintf(f, "    sw_val_t *%s = %s_%s(NULL, 0);\n", res, mod, func);
        strncpy(out, res, osz - 1);
        return;
    }

    /* Dispatch to builtin or user function */
    if (strcmp(fname, "print") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_print(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "length") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_length(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "hd") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_hd(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "tl") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_tl(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "elem") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_elem(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "abs") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_abs(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "map") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_map(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "pmap") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_pmap(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "reduce") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_reduce(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "filter") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_filter(%s, %d);\n", res, arr, nargs);
    else if (strcmp(fname, "supervise") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_supervise(%s, %d);\n", res, arr, nargs);
    /* Phase 11: Studio builtins (from swarmrt_builtins_studio.h) */
    else if (strcmp(fname, "register") == 0 || strcmp(fname, "whereis") == 0 ||
             strcmp(fname, "monitor") == 0 || strcmp(fname, "link") == 0 ||
             strcmp(fname, "ets_new") == 0 || strcmp(fname, "ets_put") == 0 ||
             strcmp(fname, "ets_get") == 0 || strcmp(fname, "ets_delete") == 0 ||
             strcmp(fname, "sleep") == 0 || strcmp(fname, "getenv") == 0 ||
             strcmp(fname, "to_string") == 0 || strcmp(fname, "format") == 0 ||
             strcmp(fname, "timestamp") == 0 ||
             strcmp(fname, "file_write") == 0 || strcmp(fname, "file_read") == 0 ||
             strcmp(fname, "file_mkdir") == 0 || strcmp(fname, "http_post") == 0 ||
             strcmp(fname, "json_get") == 0 || strcmp(fname, "json_escape") == 0 ||
             strcmp(fname, "string_contains") == 0 || strcmp(fname, "string_replace") == 0 ||
             strcmp(fname, "string_sub") == 0 || strcmp(fname, "string_length") == 0 ||
             strcmp(fname, "random_int") == 0 || strcmp(fname, "list_append") == 0 ||
             /* Distributed nodes */
             strcmp(fname, "node_start") == 0 || strcmp(fname, "node_stop") == 0 ||
             strcmp(fname, "node_name") == 0 || strcmp(fname, "node_connect") == 0 ||
             strcmp(fname, "node_disconnect") == 0 || strcmp(fname, "node_send") == 0 ||
             strcmp(fname, "node_peers") == 0 || strcmp(fname, "node_is_connected") == 0 ||
             strcmp(fname, "map_new") == 0 || strcmp(fname, "map_get") == 0 ||
             strcmp(fname, "map_put") == 0 || strcmp(fname, "map_keys") == 0 ||
             strcmp(fname, "map_values") == 0 || strcmp(fname, "map_merge") == 0 ||
             strcmp(fname, "map_has_key") == 0 || strcmp(fname, "map_size") == 0 ||
             strcmp(fname, "map_remove") == 0 || strcmp(fname, "error") == 0 ||
             strcmp(fname, "panic") == 0 || strcmp(fname, "expect") == 0 ||
             strcmp(fname, "subprocess_spawn") == 0 ||
             strcmp(fname, "subprocess_send_line") == 0 ||
             strcmp(fname, "subprocess_recv_line") == 0 ||
             strcmp(fname, "subprocess_close") == 0 ||
             strcmp(fname, "db_open") == 0 || strcmp(fname, "db_close") == 0 ||
             strcmp(fname, "db_exec") == 0 || strcmp(fname, "db_query") == 0 ||
             strcmp(fname, "typeof") == 0 ||
             /* Phase 13: Agent stdlib */
             strcmp(fname, "http_get") == 0 || strcmp(fname, "shell") == 0 ||
             strcmp(fname, "json_encode") == 0 || strcmp(fname, "json_decode") == 0 ||
             strcmp(fname, "file_exists") == 0 || strcmp(fname, "file_list") == 0 ||
             strcmp(fname, "file_delete") == 0 || strcmp(fname, "file_append") == 0 ||
             strcmp(fname, "delay") == 0 || strcmp(fname, "interval") == 0 ||
             strcmp(fname, "llm_complete") == 0 ||
             strcmp(fname, "string_split") == 0 || strcmp(fname, "string_trim") == 0 ||
             strcmp(fname, "string_upper") == 0 || strcmp(fname, "string_lower") == 0 ||
             strcmp(fname, "string_starts_with") == 0 ||
             strcmp(fname, "string_ends_with") == 0 ||
             strcmp(fname, "string_truncate") == 0 ||
             /* Agent utilities */
             strcmp(fname, "parse_gemma_calls") == 0 ||
             strcmp(fname, "clean_json") == 0 ||
             strcmp(fname, "strip_html") == 0 ||
             /* Process introspection */
             strcmp(fname, "process_info") == 0 ||
             strcmp(fname, "process_list") == 0 ||
             strcmp(fname, "registered") == 0 ||
             /* LiveView HTTP/WS */
             strcmp(fname, "http_listen") == 0 ||
             strcmp(fname, "http_respond") == 0 ||
             strcmp(fname, "ws_send") == 0 ||
             strcmp(fname, "ws_close") == 0 ||
             strcmp(fname, "ws_set_handler") == 0 ||
             strcmp(fname, "live_js") == 0 ||
             /* WebSocket CLIENT (CDP / browser control) */
             strcmp(fname, "wsc_connect") == 0 ||
             strcmp(fname, "wsc_send") == 0 ||
             strcmp(fname, "wsc_recv") == 0 ||
             strcmp(fname, "wsc_close") == 0 ||
             strcmp(fname, "chrome_launch") == 0 ||
             /* String + base64 helpers */
             strcmp(fname, "string_index_of") == 0 ||
             strcmp(fname, "base64_encode") == 0 ||
             strcmp(fname, "base64_decode") == 0 ||
             /* Phase 15: Feature Expansion */
             strcmp(fname, "query_parse") == 0 ||
             strcmp(fname, "http_serve_file") == 0 ||
             strcmp(fname, "pubsub_subscribe") == 0 ||
             strcmp(fname, "pubsub_broadcast") == 0 ||
             strcmp(fname, "pubsub_unsubscribe") == 0 ||
             strcmp(fname, "telemetry_emit") == 0 ||
             strcmp(fname, "telemetry_subscribe") == 0 ||
             strcmp(fname, "telemetry_unsubscribe") == 0 ||
             strcmp(fname, "breaker_new") == 0 ||
             strcmp(fname, "breaker_call") == 0 ||
             strcmp(fname, "breaker_state") == 0 ||
             strcmp(fname, "llm_stream") == 0 ||
             strcmp(fname, "ets_list") == 0 ||
             strcmp(fname, "ets_count") == 0 ||
             /* PDF builtins */
             strcmp(fname, "pdf_text") == 0 ||
             strcmp(fname, "pdf_pages") == 0 ||
             strcmp(fname, "pdf_meta") == 0 ||
             /* Phase 16: Interactive CLI primitives */
             strcmp(fname, "read_line") == 0 ||
             strcmp(fname, "print_inline") == 0 ||
             strcmp(fname, "sys_exit") == 0 ||
             /* Phase 17: LLM streaming */
             strcmp(fname, "http_post_stream") == 0 ||
             /* Phase 18: terminal introspection */
             strcmp(fname, "term_cols") == 0 ||
             /* Phase 19: interactive picker */
             strcmp(fname, "read_choice") == 0)
        fprintf(f, "    sw_val_t *%s = _builtin_%s(%s, %d);\n", res, fname, nargs > 0 ? arr : "NULL", nargs);
    else if (is_module_func(ctx, fname)) {
        if (nargs > 0)
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, %d);\n", res, ctx->mod_name, fname, arr, nargs);
        else
            fprintf(f, "    sw_val_t *%s = %s_%s(NULL, 0);\n", res, ctx->mod_name, fname);
    } else if (is_declared(ctx, fname) && !is_builtin(fname)) {
        /* Dynamic dispatch: variable holds a closure value */
        if (nargs > 0)
            fprintf(f, "    sw_val_t *%s = sw_val_apply(%s, %s, %d);\n", res, fname, arr, nargs);
        else
            fprintf(f, "    sw_val_t *%s = sw_val_apply(%s, NULL, 0);\n", res, fname);
    } else {
        /* Unknown function — print did-you-mean to stderr so the user
         * gets actionable suggestions BEFORE the C compiler trips. We
         * still emit the call so the C-level error fires (with our
         * per-statement #line directive it'll point at the right sw
         * line); the stderr hint just makes the cause obvious. */
        did_you_mean(ctx, fname, n->line);
        if (nargs > 0)
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, %d);\n", res, ctx->mod_name, fname, arr, nargs);
        else
            fprintf(f, "    sw_val_t *%s = %s_%s(NULL, 0);\n", res, ctx->mod_name, fname);
    }

    strncpy(out, res, osz - 1);
}

static void emit_send(cg_ctx_t *ctx, node_t *n, char *out, int osz) {
    FILE *f = ctx->out;
    char to_var[32], msg_var[32];
    emit_expr(ctx, n->v.send.to, 0, to_var, sizeof(to_var));
    emit_expr(ctx, n->v.send.msg, 0, msg_var, sizeof(msg_var));
    fprintf(f, "    sw_send_tagged(%s->v.pid, SW_TAG_NONE, %s);\n", to_var, msg_var);
    strncpy(out, msg_var, osz - 1);
}

static void emit_spawn(cg_ctx_t *ctx, node_t *n, char *out, int osz) {
    FILE *f = ctx->out;
    node_t *inner = n->v.spawn.expr;
    int sp_id = find_spawn_id(ctx, inner);
    if (sp_id < 0) {
        /* Fallback: can't find trampoline */
        char res[32];
        fresh_var(ctx, res, sizeof(res));
        fprintf(f, "    sw_val_t *%s = sw_val_pid(NULL); /* spawn failed */\n", res);
        strncpy(out, res, osz - 1);
        return;
    }

    spawn_info_t *sp = &ctx->spawns[sp_id];
    char sa[32];
    fresh_var(ctx, sa, sizeof(sa));
    fprintf(f, "    _%s_sp%d_t *%s = malloc(sizeof(_%s_sp%d_t));\n",
            ctx->mod_name, sp_id, sa, ctx->mod_name, sp_id);

    /* Evaluate and assign spawn arguments */
    for (int i = 0; i < sp->nargs; i++) {
        char arg[32];
        emit_expr(ctx, inner->v.call.args[i], 0, arg, sizeof(arg));
        fprintf(f, "    %s->a%d = %s;\n", sa, i, arg);
    }

    char res[32];
    fresh_var(ctx, res, sizeof(res));
    fprintf(f, "    sw_val_t *%s = sw_val_pid(sw_spawn(_%s_sp%d_entry, %s));\n",
            res, ctx->mod_name, sp_id, sa);
    strncpy(out, res, osz - 1);
}

static void emit_receive(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz) {
    FILE *f = ctx->out;
    char res[32], cur[32], msg[32];
    fresh_var(ctx, res, sizeof(res));
    fresh_var(ctx, cur, sizeof(cur));
    fresh_var(ctx, msg, sizeof(msg));

    uint64_t timeout = (n->v.recv.after_ms >= 0)
        ? (uint64_t)n->v.recv.after_ms : (uint64_t)-1;

    fprintf(f, "    sw_val_t *%s = sw_val_nil();\n", res);
    fprintf(f, "    { /* selective receive */\n");

    /* Dynamic timeout: `after some_var { ... }`. Evaluate the
     * expression once before the wait loop and use that. (Re-eval
     * each iteration would be nice but rare in practice.) */
    char to_ms[32] = "";
    if (n->v.recv.after_expr) {
        char to_val[32];
        emit_expr(ctx, n->v.recv.after_expr, 0, to_val, sizeof(to_val));
        fresh_var(ctx, to_ms, sizeof(to_ms));
        fprintf(f, "      uint64_t %s = (%s && %s->type == SW_VAL_INT) ? (uint64_t)%s->v.i : (uint64_t)-1;\n",
                to_ms, to_val, to_val, to_val);
    }

    fprintf(f, "      int _matched = 0;\n");
    /* Track elapsed time so `after N` actually times out. wait_new
     * returns 1 (got message) on EVERY wake, including its own
     * internal timer wake — we can't rely on its return code alone.
     * Pre-record start_ms and call wait_new with remaining budget;
     * when budget hits 0 we break out and fall through to after_body. */
    int has_timeout = n->v.recv.after_body && (to_ms[0] || n->v.recv.after_ms >= 0);
    if (has_timeout) {
        fprintf(f, "      uint64_t _recv_start_ms = _sw_now_ms();\n");
    }
    fprintf(f, "      while (!_matched) {\n");
    fprintf(f, "        sw_mailbox_drain_signals();\n");
    fprintf(f, "        sw_msg_t *%s = sw_mailbox_peek();\n", cur);
    fprintf(f, "        while (%s && !_matched) {\n", cur);
    fprintf(f, "          sw_msg_t *_next = %s->next;\n", cur);
    fprintf(f, "          sw_val_t *%s = %s->payload ? (sw_val_t *)%s->payload : sw_val_nil();\n",
            msg, cur, cur);

    /* Pattern matching clauses */
    for (int i = 0; i < n->v.recv.nclauses; i++) {
        node_t *cl = n->v.recv.clauses[i];
        fprintf(f, "          %sif (", i == 0 ? "" : "} else ");
        emit_pattern_cond(ctx, cl->v.clause.pattern, msg);
        fprintf(f, ") {\n");

        int saved_ndeclared = ctx->ndeclared;

        /* Guard check — if guard fails, don't match */
        if (cl->v.clause.guard) {
            emit_pattern_bind(ctx, cl->v.clause.pattern, msg);
            char guard_res[32];
            emit_expr(ctx, cl->v.clause.guard, 0, guard_res, sizeof(guard_res));
            fprintf(f, "            if (sw_val_is_truthy(%s)) {\n", guard_res);
            fprintf(f, "              sw_mailbox_remove_msg(%s);\n", cur);
            fprintf(f, "              _matched = 1;\n");
            char body_res[32];
            emit_expr(ctx, cl->v.clause.body, tail, body_res, sizeof(body_res));
            if (body_res[0])
                fprintf(f, "              %s = %s;\n", res, body_res);
            fprintf(f, "            }\n");
        } else {
            fprintf(f, "            sw_mailbox_remove_msg(%s);\n", cur);
            fprintf(f, "            _matched = 1;\n");
            emit_pattern_bind(ctx, cl->v.clause.pattern, msg);
            char body_res[32];
            emit_expr(ctx, cl->v.clause.body, tail, body_res, sizeof(body_res));
            if (body_res[0])
                fprintf(f, "            %s = %s;\n", res, body_res);
        }

        ctx->ndeclared = saved_ndeclared;
    }

    if (n->v.recv.nclauses > 0)
        fprintf(f, "          }\n");

    fprintf(f, "          if (!_matched) %s = _next;\n", cur);
    fprintf(f, "        }\n");  /* end inner while (cursor) */

    /* No match in current queue — wait for new messages or timeout */
    fprintf(f, "        if (!_matched) {\n");
    if (has_timeout) {
        /* Compute remaining budget. If exhausted, break and fall
         * through to after_body. Otherwise wait_new with the
         * remaining time. */
        const char *budget_src = to_ms[0] ? to_ms : NULL;
        if (budget_src) {
            fprintf(f, "          uint64_t _elapsed = _sw_now_ms() - _recv_start_ms;\n");
            fprintf(f, "          if (_elapsed >= %s) break;\n", budget_src);
            fprintf(f, "          sw_mailbox_wait_new(%s - _elapsed);\n", budget_src);
        } else {
            fprintf(f, "          uint64_t _elapsed = _sw_now_ms() - _recv_start_ms;\n");
            fprintf(f, "          if (_elapsed >= %lluULL) break;\n", (unsigned long long)timeout);
            fprintf(f, "          sw_mailbox_wait_new(%lluULL - _elapsed);\n", (unsigned long long)timeout);
        }
    } else {
        /* No after-clause — wait forever (or use the literal-int path
         * if some other code path set after_ms but no after_body). */
        if (to_ms[0]) {
            fprintf(f, "          if (!sw_mailbox_wait_new(%s)) break;\n", to_ms);
        } else {
            fprintf(f, "          if (!sw_mailbox_wait_new(%lluULL)) break;\n", (unsigned long long)timeout);
        }
    }
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");  /* end outer while (!matched) */

    /* After clause (timeout handler) */
    if (n->v.recv.after_body) {
        fprintf(f, "      if (!_matched) {\n");
        char after_res[32];
        emit_expr(ctx, n->v.recv.after_body, 0, after_res, sizeof(after_res));
        if (after_res[0])
            fprintf(f, "        %s = %s;\n", res, after_res);
        fprintf(f, "      }\n");
    }

    fprintf(f, "    }\n");  /* end selective receive block */

    strncpy(out, res, osz - 1);
}

/* emit_case — top-level pattern matching expression.
 *
 *   case <subject> {
 *       pat1 -> body1
 *       pat2 when guard -> body2
 *       _other -> default
 *   }
 *
 * Compiles to: evaluate subject once, then a chained if/else over the
 * patterns (reusing emit_pattern_cond + emit_pattern_bind that the
 * receive emitter uses). No mailbox involvement — this is a value
 * dispatch, not a message dispatch. Each arm is its own scope, so we
 * snapshot ndeclared around the body to keep parallel binding names
 * isolated (e.g. `case x { {'a', n} -> n ; {'b', n} -> n + 1 }` —
 * both `n`s are separate). */
static void emit_case(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz) {
    FILE *f = ctx->out;
    char subj[32], res[32];
    emit_expr(ctx, n->v.casex.subject, 0, subj, sizeof(subj));
    fresh_var(ctx, res, sizeof(res));
    fprintf(f, "    sw_val_t *%s = sw_val_nil();\n", res);

    /* Wrap in a do-while(0) so each arm can `break` to skip the rest
     * once it matches AND its guard (if any) fires. Without this, a
     * pattern that matches but whose guard fails would consume the
     * dispatch even though no body ran — falling through is exactly
     * what we want when the guard rejects. */
    fprintf(f, "    do {\n");
    int nclauses = n->v.casex.nclauses;
    for (int i = 0; i < nclauses; i++) {
        node_t *cl = n->v.casex.clauses[i];
        int saved_ndeclared = ctx->ndeclared;
        fprintf(f, "        if (");
        emit_pattern_cond(ctx, cl->v.clause.pattern, subj);
        fprintf(f, ") {\n");

        emit_pattern_bind(ctx, cl->v.clause.pattern, subj);

        if (cl->v.clause.guard) {
            char guard_res[32];
            emit_expr(ctx, cl->v.clause.guard, 0, guard_res, sizeof(guard_res));
            fprintf(f, "            if (sw_val_is_truthy(%s)) {\n", guard_res);
            char body_res[32];
            emit_expr(ctx, cl->v.clause.body, tail, body_res, sizeof(body_res));
            if (body_res[0])
                fprintf(f, "                %s = %s;\n", res, body_res);
            fprintf(f, "                break;\n");
            fprintf(f, "            }\n");
            /* Guard failed — close the pattern-match block and let the
             * loop continue to the next clause. */
        } else {
            char body_res[32];
            emit_expr(ctx, cl->v.clause.body, tail, body_res, sizeof(body_res));
            if (body_res[0])
                fprintf(f, "            %s = %s;\n", res, body_res);
            fprintf(f, "            break;\n");
        }

        fprintf(f, "        }\n");
        ctx->ndeclared = saved_ndeclared;
    }
    fprintf(f, "    } while (0);\n");

    strncpy(out, res, osz - 1);
}

static void emit_if(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz) {
    FILE *f = ctx->out;
    char cond[32], res[32];
    emit_expr(ctx, n->v.iff.cond, 0, cond, sizeof(cond));
    fresh_var(ctx, res, sizeof(res));
    fprintf(f, "    sw_val_t *%s = sw_val_nil();\n", res);
    fprintf(f, "    if (sw_val_is_truthy(%s)) {\n", cond);

    /* Snapshot declared-variable count so anything declared inside
     * the then-branch is forgotten when we exit it. Without this,
     * a name reused in the else-branch (or any later sibling block)
     * is emitted as a re-assignment instead of a fresh declaration —
     * but C scope rules don't carry the first declaration across
     * sibling braces, so the second use becomes "undeclared
     * identifier 'X'" at the C compile step. Restoring ndeclared on
     * scope exit fixes the canonical "rename _w / _h" footgun. */
    int saved_then = ctx->ndeclared;
    char then_res[32];
    emit_expr(ctx, n->v.iff.then_b, tail, then_res, sizeof(then_res));
    if (then_res[0])
        fprintf(f, "        %s = %s;\n", res, then_res);
    fprintf(f, "    }");
    ctx->ndeclared = saved_then;

    if (n->v.iff.else_b) {
        fprintf(f, " else {\n");
        int saved_else = ctx->ndeclared;
        char else_res[32];
        emit_expr(ctx, n->v.iff.else_b, tail, else_res, sizeof(else_res));
        if (else_res[0])
            fprintf(f, "        %s = %s;\n", res, else_res);
        fprintf(f, "    }");
        ctx->ndeclared = saved_else;
    }
    fprintf(f, "\n");
    strncpy(out, res, osz - 1);
}

static void emit_pipe(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz) {
    FILE *f = ctx->out;
    char val[32];
    emit_expr(ctx, n->v.pipe.val, 0, val, sizeof(val));

    node_t *func = n->v.pipe.func;
    if (func->type == N_CALL) {
        /* val |> func(extra_args) → func(val, extra_args) */
        const char *fname = func->v.call.func->v.sval;
        int nargs = func->v.call.nargs + 1;

        char arg_vars[16][32];
        strncpy(arg_vars[0], val, 31);
        for (int i = 0; i < func->v.call.nargs && i < 15; i++)
            emit_expr(ctx, func->v.call.args[i], 0, arg_vars[i + 1], sizeof(arg_vars[i + 1]));

        char arr[32];
        fresh_var(ctx, arr, sizeof(arr));
        fprintf(f, "    sw_val_t *%s[] = {", arr);
        for (int i = 0; i < nargs; i++) {
            if (i) fprintf(f, ", ");
            fprintf(f, "%s", arg_vars[i]);
        }
        fprintf(f, "};\n");

        char res[32];
        fresh_var(ctx, res, sizeof(res));

        if (is_builtin(fname))
            fprintf(f, "    sw_val_t *%s = _builtin_%s(%s, %d);\n", res, fname, arr, nargs);
        else
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, %d);\n", res, ctx->mod_name, fname, arr, nargs);

        strncpy(out, res, osz - 1);
    } else if (func->type == N_IDENT) {
        /* val |> func → func(val) */
        const char *fname = func->v.sval;
        char arr[32];
        fresh_var(ctx, arr, sizeof(arr));
        fprintf(f, "    sw_val_t *%s[] = {%s};\n", arr, val);
        char res[32];
        fresh_var(ctx, res, sizeof(res));

        if (is_builtin(fname))
            fprintf(f, "    sw_val_t *%s = _builtin_%s(%s, 1);\n", res, fname, arr);
        else
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, 1);\n", res, ctx->mod_name, fname, arr);

        strncpy(out, res, osz - 1);
    } else {
        /* Can't pipe to this expression type */
        strncpy(out, val, osz - 1);
    }
    (void)tail;
}

static void emit_expr(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz) {
    FILE *f = ctx->out;
    out[0] = '\0';
    if (!n) return;

    switch (n->type) {
    case N_INT: {
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_int(%lldLL);\n", v, (long long)n->v.ival);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_FLOAT: {
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_float(%.17g);\n", v, n->v.fval);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_STRING: {
        char v[32]; fresh_var(ctx, v, sizeof(v));
        char *esc = c_escape_str(n->v.sval);
        fprintf(f, "    sw_val_t *%s = sw_val_string(\"%s\");\n", v, esc);
        free(esc);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_ATOM: {
        char v[32]; fresh_var(ctx, v, sizeof(v));
        char *esc = c_escape_str(n->v.sval);
        fprintf(f, "    sw_val_t *%s = sw_val_atom(\"%s\");\n", v, esc);
        free(esc);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_SELF: {
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_pid(sw_self());\n", v);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_IDENT: {
        if (is_declared(ctx, n->v.sval)) {
            /* Known variable — return mangled C-side name. */
            strncpy(out, mangle_for_c(n->v.sval), osz - 1);
        } else if (is_module_func(ctx, n->v.sval)) {
            /* Module function used by-name as a value (e.g. passed in
             * `%{handler: handle_echo}` or stored in a var to call
             * later). Wrap it as a callable SW_VAL_FUN so sw_val_apply
             * dispatches correctly. Without this we silently produced
             * an atom and any later call(args) returned nil. */
            char v[32]; fresh_var(ctx, v, sizeof(v));
            /* nparams = -1 means "variadic at the C ABI level"; the
             * generated module functions all take (sw_val_t **, int). */
            fprintf(f, "    sw_val_t *%s = sw_val_fun_native((void*)%s_%s, -1, NULL, 0);\n",
                    v, ctx->mod_name, n->v.sval);
            strncpy(out, v, osz - 1);
        } else {
            /* Undeclared identifier → treat as atom (message tag) */
            char v[32]; fresh_var(ctx, v, sizeof(v));
            fprintf(f, "    sw_val_t *%s = sw_val_atom(\"%s\");\n", v, n->v.sval);
            strncpy(out, v, osz - 1);
        }
        break;
    }
    case N_ASSIGN: {
        char val[32];
        emit_expr(ctx, n->v.assign.value, 0, val, sizeof(val));
        const char *cname = mangle_for_c(n->v.assign.name);
        if (is_declared(ctx, n->v.assign.name)) {
            fprintf(f, "    %s = %s;\n", cname, val);
        } else {
            fprintf(f, "    sw_val_t *%s = %s;\n", cname, val);
            declare_var(ctx, n->v.assign.name);
        }
        strncpy(out, cname, osz - 1);
        break;
    }
    case N_BLOCK: {
        char last[32] = "";
        for (int i = 0; i < n->v.block.nstmts; i++) {
            int is_last = (i == n->v.block.nstmts - 1);
            /* Per-statement #line so a C error inside a multi-statement
             * function points at the exact .sw line, not the function's
             * start line. Skip if this stmt has no line info or shares
             * a line with the previous emit (avoid duplicate directives
             * for several expressions on one source line). */
            node_t *stmt = n->v.block.stmts[i];
            if (stmt && stmt->line > 0 && stmt->line != ctx->last_line_emitted) {
                const char *path = guess_sw_path(ctx->mod_name);
                fprintf(f, "#line %d \"%s\"\n", stmt->line, path);
                /* Runtime tracker — panic() reads these for "at FILE:LINE". */
                fprintf(f, "_sw_current_line = %d; _sw_current_file = \"%s\";\n",
                        stmt->line, path);
                ctx->last_line_emitted = stmt->line;
            }
            emit_expr(ctx, stmt, is_last ? tail : 0, last, sizeof(last));
        }
        strncpy(out, last, osz - 1);
        break;
    }
    case N_BINOP:
        emit_binop(ctx, n, out, osz);
        break;
    case N_UNARY: {
        char operand[32];
        emit_expr(ctx, n->v.unary.operand, 0, operand, sizeof(operand));
        char v[32]; fresh_var(ctx, v, sizeof(v));
        if (n->v.unary.op == '-')
            fprintf(f, "    sw_val_t *%s = (%s->type == SW_VAL_INT) ? sw_val_int(-%s->v.i) : sw_val_float(-%s->v.f);\n",
                    v, operand, operand, operand);
        else {
            fprintf(f, "    sw_val_t *%s = %s;\n", v, operand);
        }
        strncpy(out, v, osz - 1);
        break;
    }
    case N_TUPLE: {
        char items[64][32];
        for (int i = 0; i < n->v.coll.count && i < 64; i++)
            emit_expr(ctx, n->v.coll.items[i], 0, items[i], sizeof(items[i]));
        char arr[32]; fresh_var(ctx, arr, sizeof(arr));
        fprintf(f, "    sw_val_t *%s[] = {", arr);
        for (int i = 0; i < n->v.coll.count; i++) {
            if (i) fprintf(f, ", ");
            fprintf(f, "%s", items[i]);
        }
        fprintf(f, "};\n");
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_tuple(%s, %d);\n", v, arr, n->v.coll.count);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_LIST: {
        char items[256][32];
        int count = n->v.coll.count < 256 ? n->v.coll.count : 256;
        for (int i = 0; i < count; i++)
            emit_expr(ctx, n->v.coll.items[i], 0, items[i], sizeof(items[i]));
        char arr[32]; fresh_var(ctx, arr, sizeof(arr));
        fprintf(f, "    sw_val_t *%s[] = {", arr);
        for (int i = 0; i < count; i++) {
            if (i) fprintf(f, ", ");
            fprintf(f, "%s", items[i]);
        }
        fprintf(f, "};\n");
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_list(%s, %d);\n", v, arr, count);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_CALL:
        emit_call(ctx, n, tail, out, osz);
        break;
    case N_SEND:
        emit_send(ctx, n, out, osz);
        break;
    case N_SPAWN:
        emit_spawn(ctx, n, out, osz);
        break;
    case N_RECEIVE:
        emit_receive(ctx, n, tail, out, osz);
        break;
    case N_CASE:
        emit_case(ctx, n, tail, out, osz);
        break;
    case N_IF:
        emit_if(ctx, n, tail, out, osz);
        break;
    case N_PIPE:
        emit_pipe(ctx, n, tail, out, osz);
        break;
    case N_FUN: {
        /* Anonymous function expression — create a closure value */
        int lid = find_lambda_id(ctx, n);
        if (lid < 0) break;
        lambda_info_t *li = &ctx->lambdas[lid];
        char v[32]; fresh_var(ctx, v, sizeof(v));
        if (li->ncaptures > 0) {
            char cap_arr[32]; fresh_var(ctx, cap_arr, sizeof(cap_arr));
            fprintf(f, "    sw_val_t *%s[] = {", cap_arr);
            for (int i = 0; i < li->ncaptures; i++) {
                if (i) fprintf(f, ", ");
                fprintf(f, "%s", mangle_for_c(li->captures[i]));
            }
            fprintf(f, "};\n");
            fprintf(f, "    sw_val_t *%s = sw_val_fun_native((void*)%s, %d, %s, %d);\n",
                    v, li->gen_name, n->v.fun.nparams, cap_arr, li->ncaptures);
        } else {
            fprintf(f, "    sw_val_t *%s = sw_val_fun_native((void*)%s, %d, NULL, 0);\n",
                    v, li->gen_name, n->v.fun.nparams);
        }
        strncpy(out, v, osz - 1);
        break;
    }
    case N_MAP: {
        /* Map literal: %{k1: v1, k2: v2} -> sw_val_map_new(keys, vals, count) */
        int count = n->v.map.count;
        char key_vars[64][32], val_vars[64][32];
        for (int i = 0; i < count && i < 64; i++) {
            emit_expr(ctx, n->v.map.keys[i], 0, key_vars[i], sizeof(key_vars[i]));
            emit_expr(ctx, n->v.map.vals[i], 0, val_vars[i], sizeof(val_vars[i]));
        }
        char karr[32], varr[32];
        fresh_var(ctx, karr, sizeof(karr));
        fresh_var(ctx, varr, sizeof(varr));
        fprintf(f, "    sw_val_t *%s[] = {", karr);
        for (int i = 0; i < count; i++) { if (i) fprintf(f, ", "); fprintf(f, "%s", key_vars[i]); }
        fprintf(f, "};\n");
        fprintf(f, "    sw_val_t *%s[] = {", varr);
        for (int i = 0; i < count; i++) { if (i) fprintf(f, ", "); fprintf(f, "%s", val_vars[i]); }
        fprintf(f, "};\n");
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_map_new(%s, %s, %d);\n", v, karr, varr, count);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_FOR: {
        /* for x in iter { body } */
        char iter_var[32];
        emit_expr(ctx, n->v.forloop.iter, 0, iter_var, sizeof(iter_var));
        char idx[32]; fresh_var(ctx, idx, sizeof(idx));
        /* Check if iter is a range (N_RANGE produces a special struct) */
        if (n->v.forloop.iter->type == N_RANGE) {
            /* Range for: for i in start..end */
            char start_v[32], end_v[32];
            emit_expr(ctx, n->v.forloop.iter->v.range.from, 0, start_v, sizeof(start_v));
            emit_expr(ctx, n->v.forloop.iter->v.range.to, 0, end_v, sizeof(end_v));
            fprintf(f, "    for (int64_t %s = %s->v.i; %s <= %s->v.i; %s++) {\n",
                    idx, start_v, idx, end_v, idx);
            if (!is_declared(ctx, n->v.forloop.var)) {
                fprintf(f, "    sw_val_t *%s = sw_val_int(%s);\n", n->v.forloop.var, idx);
                declare_var(ctx, n->v.forloop.var);
            } else {
                fprintf(f, "    %s = sw_val_int(%s);\n", n->v.forloop.var, idx);
            }
        } else {
            /* List for: for x in list */
            fprintf(f, "    for (int %s = 0; %s < %s->v.tuple.count; %s++) {\n",
                    idx, idx, iter_var, idx);
            if (!is_declared(ctx, n->v.forloop.var)) {
                fprintf(f, "    sw_val_t *%s = %s->v.tuple.items[%s];\n", n->v.forloop.var, iter_var, idx);
                declare_var(ctx, n->v.forloop.var);
            } else {
                fprintf(f, "    %s = %s->v.tuple.items[%s];\n", n->v.forloop.var, iter_var, idx);
            }
        }
        char body_res[32];
        emit_expr(ctx, n->v.forloop.body, 0, body_res, sizeof(body_res));
        fprintf(f, "    }\n");
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_nil();\n", v);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_RANGE: {
        /* Range expression 1..10 -- build a list */
        char start_v[32], end_v[32];
        emit_expr(ctx, n->v.range.from, 0, start_v, sizeof(start_v));
        emit_expr(ctx, n->v.range.to, 0, end_v, sizeof(end_v));
        char v[32]; fresh_var(ctx, v, sizeof(v));
        char cnt[32]; fresh_var(ctx, cnt, sizeof(cnt));
        char arr[32]; fresh_var(ctx, arr, sizeof(arr));
        fprintf(f, "    int %s = (int)(%s->v.i - %s->v.i + 1);\n", cnt, end_v, start_v);
        fprintf(f, "    if (%s < 0) %s = 0;\n", cnt, cnt);
        fprintf(f, "    sw_val_t **%s = malloc(sizeof(sw_val_t*) * (%s > 0 ? %s : 1));\n", arr, cnt, cnt);
        char ri[32]; fresh_var(ctx, ri, sizeof(ri));
        fprintf(f, "    for (int %s = 0; %s < %s; %s++) %s[%s] = sw_val_int(%s->v.i + %s);\n",
                ri, ri, cnt, ri, arr, ri, start_v, ri);
        fprintf(f, "    sw_val_t *%s = sw_val_list(%s, %s);\n", v, arr, cnt);
        fprintf(f, "    free(%s);\n", arr);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_TRY: {
        /* try { body } catch e { handler }
         *
         * The C `if (_sw_error) { ... }` block is its own lexical
         * scope, so the err_var declaration belongs only to it.
         * Snapshot ndeclared before the catch and restore after, the
         * same pattern emit_if uses for branch scopes — without this,
         * a second try/catch with the same err_var name would emit
         * `e = _sw_error;` instead of `sw_val_t *e = _sw_error;` and
         * fail with "use of undeclared identifier 'e'". */
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    _sw_error = NULL;\n");
        char body_res[32];
        emit_expr(ctx, n->v.trycatch.body, 0, body_res, sizeof(body_res));
        fprintf(f, "    sw_val_t *%s = %s;\n", v, body_res[0] ? body_res : "sw_val_nil()");
        fprintf(f, "    if (_sw_error) {\n");
        int saved_ndeclared = ctx->ndeclared;
        fprintf(f, "        sw_val_t *%s = _sw_error;\n", mangle_for_c(n->v.trycatch.err_var));
        declare_var(ctx, n->v.trycatch.err_var);
        fprintf(f, "        _sw_error = NULL;\n");
        char catch_res[32];
        emit_expr(ctx, n->v.trycatch.catch_body, 0, catch_res, sizeof(catch_res));
        if (catch_res[0])
            fprintf(f, "        %s = %s;\n", v, catch_res);
        fprintf(f, "    }\n");
        ctx->ndeclared = saved_ndeclared;
        strncpy(out, v, osz - 1);
        break;
    }
    case N_LIST_CONS: {
        /* [h | t] as expression -- cons h onto list t */
        char head_v[32], tail_v[32];
        emit_expr(ctx, n->v.cons.head, 0, head_v, sizeof(head_v));
        emit_expr(ctx, n->v.cons.tail, 0, tail_v, sizeof(tail_v));
        char v[32]; fresh_var(ctx, v, sizeof(v));
        char arr_name[32]; fresh_var(ctx, arr_name, sizeof(arr_name));
        char cnt_name[32]; fresh_var(ctx, cnt_name, sizeof(cnt_name));
        fprintf(f, "    int %s = %s->type == SW_VAL_LIST ? %s->v.tuple.count + 1 : 1;\n",
                cnt_name, tail_v, tail_v);
        fprintf(f, "    sw_val_t **%s = malloc(sizeof(sw_val_t*) * %s);\n", arr_name, cnt_name);
        fprintf(f, "    %s[0] = %s;\n", arr_name, head_v);
        fprintf(f, "    if (%s->type == SW_VAL_LIST) for (int _ci = 0; _ci < %s->v.tuple.count; _ci++) %s[_ci+1] = %s->v.tuple.items[_ci];\n",
                tail_v, tail_v, arr_name, tail_v);
        fprintf(f, "    sw_val_t *%s = sw_val_list(%s, %s);\n", v, arr_name, cnt_name);
        fprintf(f, "    free(%s);\n", arr_name);
        strncpy(out, v, osz - 1);
        break;
    }
    default:
        break;
    }
}

/* =========================================================================
 * Function emission
 * ========================================================================= */

/* Map a module name to its likely source path. Convention: most
 * modules live at src/<ModName>.sw (case preserved), with the
 * exception of `Main` → `src/main.sw` (lowercase). swc.c uses the
 * same fallback when resolving imports. Returned in a static buffer
 * — caller copies if needed. */
static const char *guess_sw_path(const char *mod_name) {
    static char path[256];
    /* Special-case Main → main.sw (legacy convention). */
    if (strcmp(mod_name, "Main") == 0) {
        snprintf(path, sizeof(path), "src/main.sw");
    } else {
        snprintf(path, sizeof(path), "src/%s.sw", mod_name);
    }
    return path;
}

static void emit_function(cg_ctx_t *ctx, node_t *fn) {
    FILE *f = ctx->out;

    /* Set up function context */
    strncpy(ctx->cur_func, fn->v.fun.name, 127);
    ctx->cur_nparams = fn->v.fun.nparams;
    for (int i = 0; i < fn->v.fun.nparams; i++)
        strncpy(ctx->cur_params[i], fn->v.fun.params[i], 127);
    ctx->ndeclared = 0;
    ctx->has_tail = has_tail_calls(ctx, fn->v.fun.body);

    /* Emit a #line directive so any C compiler error inside this
     * function points back to the .sw source instead of the
     * generated /tmp/swc_*.c file. Pure UX: enables editors to
     * jump to the right sw line on compile failure. */
    if (fn->line > 0) {
        const char *path = guess_sw_path(ctx->mod_name);
        fprintf(f, "#line %d \"%s\"\n", fn->line, path);
        ctx->last_line_emitted = fn->line;
    } else {
        ctx->last_line_emitted = 0;
    }

    fprintf(f, "static sw_val_t *%s_%s(sw_val_t **_args, int _nargs) {\n",
            ctx->mod_name, fn->v.fun.name);

    /* Parameter extraction */
    if (fn->v.fun.nparams > 0) {
        for (int i = 0; i < fn->v.fun.nparams; i++) {
            if (fn->v.fun.defaults[i]) {
                char def_val[32];
                emit_expr(ctx, fn->v.fun.defaults[i], 0, def_val, sizeof(def_val));
                fprintf(f, "    sw_val_t *%s = _nargs > %d ? _args[%d] : %s;\n",
                        mangle_for_c(fn->v.fun.params[i]), i, i, def_val);
            } else {
                fprintf(f, "    sw_val_t *%s = _nargs > %d ? _args[%d] : sw_val_nil();\n",
                        mangle_for_c(fn->v.fun.params[i]), i, i);
            }
            declare_var(ctx, fn->v.fun.params[i]);
        }
    } else {
        fprintf(f, "    (void)_args; (void)_nargs;\n");
    }

    /* Push stack frame for tracing. _tail goto reuses the SAME
     * frame — tail-recursion doesn't grow the call stack visually. */
    fprintf(f, "    _sw_trace_push(\"%s\", \"%s\", %d);\n",
            ctx->mod_name, fn->v.fun.name, fn->line);

    /* Tail call label */
    if (ctx->has_tail)
        fprintf(f, "_tail:;\n");

    /* Body */
    char result[32];
    emit_expr(ctx, fn->v.fun.body, 1, result, sizeof(result));

    /* Pop frame before returning. */
    fprintf(f, "    _sw_trace_pop();\n");

    /* Return */
    if (result[0])
        fprintf(f, "    return %s;\n", result);
    else
        fprintf(f, "    return sw_val_nil();\n");

    fprintf(f, "}\n\n");
}

/* =========================================================================
 * Entry point and main()
 * ========================================================================= */

static void emit_entry_and_main(cg_ctx_t *ctx) {
    FILE *f = ctx->out;

    /* Entry: when the user's main() returns, signal the OS-thread main
     * (below) to tear down the runtime and exit cleanly. Without this,
     * every program would hang in the run-forever loop after main
     * finished — Erlang VM behaviour ("never exit unless told"), but
     * unhelpful for the 90% case where main is a script that does its
     * thing and ends. Programs that want to keep running put a
     * permanent receive / sleep loop at the end of main themselves. */
    fprintf(f, "/* === Entry point === */\n");
    fprintf(f, "static pthread_mutex_t _sw_done_lock = PTHREAD_MUTEX_INITIALIZER;\n");
    fprintf(f, "static pthread_cond_t  _sw_done_cond = PTHREAD_COND_INITIALIZER;\n");
    fprintf(f, "static volatile int    _sw_done_flag = 0;\n\n");

    fprintf(f, "static void _main_entry(void *_arg) {\n");
    fprintf(f, "    (void)_arg;\n");
    fprintf(f, "    %s_main(NULL, 0);\n", ctx->mod_name);
    fprintf(f, "    pthread_mutex_lock(&_sw_done_lock);\n");
    fprintf(f, "    _sw_done_flag = 1;\n");
    fprintf(f, "    pthread_cond_signal(&_sw_done_cond);\n");
    fprintf(f, "    pthread_mutex_unlock(&_sw_done_lock);\n");
    fprintf(f, "}\n\n");

    fprintf(f, "int main(int argc, char **argv) {\n");
    fprintf(f, "    (void)argc; (void)argv;\n");
    fprintf(f, "    setvbuf(stdout, NULL, _IONBF, 0);\n");
    fprintf(f, "    sw_init(\"%s\", 2);\n", ctx->mod_name);
    fprintf(f, "    sw_io_init();\n");
    fprintf(f, "    sw_spawn(_main_entry, NULL);\n");
    fprintf(f, "    /* Wait for the user's main() to return (set by _main_entry). */\n");
    fprintf(f, "    pthread_mutex_lock(&_sw_done_lock);\n");
    fprintf(f, "    while (!_sw_done_flag) pthread_cond_wait(&_sw_done_cond, &_sw_done_lock);\n");
    fprintf(f, "    pthread_mutex_unlock(&_sw_done_lock);\n");
    fprintf(f, "    sw_shutdown(0);\n");
    fprintf(f, "    return 0;\n");
    fprintf(f, "}\n");
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int sw_codegen(void *ast, FILE *out, int obfuscate) {
    if (!ast || !out) return -1;
    node_t *mod = (node_t *)ast;
    if (mod->type != N_MODULE) return -1;

    cg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    strncpy(ctx.mod_name, mod->v.mod.name, 127);

    /* Collect function names */
    for (int i = 0; i < mod->v.mod.nfuns && i < CG_MAX_FUNCS; i++) {
        strncpy(ctx.func_names[i], mod->v.mod.funs[i]->v.fun.name, 127);
        ctx.nfuncs++;
    }

    /* Pre-scan for spawn sites and lambdas */
    scan_spawns(&ctx, mod);
    scan_lambdas(&ctx, mod);

    /* Emit everything */
    emit_preamble(&ctx);
    emit_forward_decls(&ctx, mod);
    emit_spawn_trampolines(&ctx);
    emit_lambda_functions(&ctx);

    fprintf(out, "/* === Functions === */\n");
    for (int i = 0; i < mod->v.mod.nfuns; i++)
        emit_function(&ctx, mod->v.mod.funs[i]);

    emit_entry_and_main(&ctx);

    (void)obfuscate; /* handled externally by sw_obfuscate() */
    return 0;
}

char *sw_codegen_to_string(void *ast, int obfuscate) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return NULL;
    int rc = sw_codegen(ast, f, obfuscate);
    fclose(f);
    if (rc != 0) { free(buf); return NULL; }
    return buf;
}

int sw_codegen_module(void *ast, FILE *out) {
    if (!ast || !out) return -1;
    node_t *mod = (node_t *)ast;
    if (mod->type != N_MODULE) return -1;

    cg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    strncpy(ctx.mod_name, mod->v.mod.name, 127);

    for (int i = 0; i < mod->v.mod.nfuns && i < CG_MAX_FUNCS; i++) {
        strncpy(ctx.func_names[i], mod->v.mod.funs[i]->v.fun.name, 127);
        ctx.nfuncs++;
    }

    scan_spawns(&ctx, mod);
    scan_lambdas(&ctx, mod);

    /* Module-only: forward decls, trampolines, lambdas, functions — no preamble, no main */
    fprintf(out, "/* === Module: %s === */\n", ctx.mod_name);
    emit_forward_decls(&ctx, mod);
    emit_spawn_trampolines(&ctx);
    emit_lambda_functions(&ctx);

    for (int i = 0; i < mod->v.mod.nfuns; i++)
        emit_function(&ctx, mod->v.mod.funs[i]);

    return 0;
}

int sw_codegen_multi(void **modules, int nmodules, int main_idx, FILE *out) {
    if (!modules || nmodules <= 0 || !out) return -1;
    if (main_idx < 0 || main_idx >= nmodules) return -1;

    /* Emit preamble once (from the main module's context) */
    cg_ctx_t preamble_ctx;
    memset(&preamble_ctx, 0, sizeof(preamble_ctx));
    preamble_ctx.out = out;
    emit_preamble(&preamble_ctx);

    /* Forward declarations for ALL modules first */
    for (int m = 0; m < nmodules; m++) {
        node_t *mod = (node_t *)modules[m];
        if (!mod || mod->type != N_MODULE) continue;
        cg_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.out = out;
        strncpy(ctx.mod_name, mod->v.mod.name, 127);
        emit_forward_decls(&ctx, mod);
    }

    /* Emit each module's functions */
    for (int m = 0; m < nmodules; m++) {
        node_t *mod = (node_t *)modules[m];
        if (!mod || mod->type != N_MODULE) continue;

        cg_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.out = out;
        strncpy(ctx.mod_name, mod->v.mod.name, 127);

        for (int i = 0; i < mod->v.mod.nfuns && i < CG_MAX_FUNCS; i++) {
            strncpy(ctx.func_names[i], mod->v.mod.funs[i]->v.fun.name, 127);
            ctx.nfuncs++;
        }

        scan_spawns(&ctx, mod);
        scan_lambdas(&ctx, mod);

        fprintf(out, "\n/* === Module: %s === */\n", ctx.mod_name);
        emit_spawn_trampolines(&ctx);
        emit_lambda_functions(&ctx);

        for (int i = 0; i < mod->v.mod.nfuns; i++)
            emit_function(&ctx, mod->v.mod.funs[i]);
    }

    /* Entry point from main module */
    node_t *main_mod = (node_t *)modules[main_idx];
    cg_ctx_t main_ctx;
    memset(&main_ctx, 0, sizeof(main_ctx));
    main_ctx.out = out;
    strncpy(main_ctx.mod_name, main_mod->v.mod.name, 127);
    emit_entry_and_main(&main_ctx);

    return 0;
}
