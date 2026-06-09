/*
 * SwarmRT Compiler: AST → C Code Generation
 *
 * Walks parsed .sw ASTs and emits C source that links against SwarmRT.
 * Handles: spawn trampolines, receive/pattern-match, tail call opt,
 * pipe operators, send/receive, and all expression types.
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

    /* module-level globals (let x = expr) */
    char global_names[16][128];
    int nglobals;
    int has_globals; /* 1 if nglobals > 0 — used by emit_entry_and_main */

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
    int func_nparams[CG_MAX_FUNCS];
    int nfuncs;

    /* Sticky flag: set by emit_call when it detects an arity mismatch
     * on a user-defined function. sw_codegen() checks this after the
     * whole module has been walked and fails the codegen so swc aborts
     * before invoking cc. Walking the whole module first means we can
     * report ALL mismatches in one shot rather than bailing on the
     * first one. */
    int had_arity_error;

    /* Sticky flag: set by did_you_mean when it reports an unknown
     * function call. Same pattern as had_arity_error — emit_call still
     * walks the rest of the module so we surface ALL unknown calls in
     * one pass, then sw_codegen() / sw_codegen_multi() return -1 so
     * swc aborts before invoking cc. Without this flag, swc emitted
     * the hint but still synthesised `Main_helper(...)` calls into the
     * generated C, and the user got a second wave of cascading
     * "undeclared function" errors out of clang. */
    int had_unknown_fn;

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

static int is_global(cg_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->nglobals; i++)
        if (strcmp(ctx->global_names[i], name) == 0) return 1;
    return 0;
}

static int is_builtin(const char *name) {
    return strcmp(name, "print") == 0 || strcmp(name, "length") == 0 ||
           strcmp(name, "hd") == 0 || strcmp(name, "tl") == 0 ||
           strcmp(name, "elem") == 0 || strcmp(name, "abs") == 0 ||
           /* Phase 11: Studio builtins */
           strcmp(name, "register") == 0 || strcmp(name, "whereis") == 0 ||
           strcmp(name, "monitor") == 0 || strcmp(name, "link") == 0 ||
           strcmp(name, "unlink") == 0 || strcmp(name, "demonitor") == 0 ||
           strcmp(name, "exit_proc") == 0 || strcmp(name, "trap_exit") == 0 ||
           strcmp(name, "ets_new") == 0 || strcmp(name, "ets_put") == 0 ||
           strcmp(name, "ets_get") == 0 || strcmp(name, "ets_delete") == 0 ||
           strcmp(name, "ets_update_counter") == 0 || strcmp(name, "ets_cas") == 0 ||
           strcmp(name, "ets_take") == 0 || strcmp(name, "ets_update") == 0 ||
           strcmp(name, "sleep") == 0 || strcmp(name, "getenv") == 0 ||
           strcmp(name, "os_args") == 0 ||
           strcmp(name, "print_above") == 0 ||
           strcmp(name, "to_string") == 0 || strcmp(name, "format") == 0 ||
           strcmp(name, "timestamp") == 0 ||
           strcmp(name, "file_write") == 0 || strcmp(name, "file_read") == 0 ||
           strcmp(name, "file_mkdir") == 0 ||
           strcmp(name, "http_post") == 0 ||
           strcmp(name, "http_request") == 0 ||
           strcmp(name, "json_get") == 0 || strcmp(name, "json_escape") == 0 ||
           strcmp(name, "string_contains") == 0 || strcmp(name, "string_replace") == 0 ||
           strcmp(name, "string_sub") == 0 || strcmp(name, "string_length") == 0 ||
           strcmp(name, "random_int") == 0 || strcmp(name, "list_append") == 0 ||
           /* Functional primitives */
           strcmp(name, "map") == 0 || strcmp(name, "pmap") == 0 ||
           strcmp(name, "reduce") == 0 || strcmp(name, "filter") == 0 ||
           /* Supervisor */
           strcmp(name, "supervise") == 0 ||
           /* DynamicSupervisor (runtime start_child) */
           strcmp(name, "dyn_supervisor") == 0 ||
           strcmp(name, "sup_start_child") == 0 ||
           strcmp(name, "sup_terminate_child") == 0 ||
           strcmp(name, "sup_count_children") == 0 ||
           /* Tool registry (self-defined hot-loaded sw tools) */
           strcmp(name, "tool_define") == 0 ||
           strcmp(name, "tool_call") == 0 ||
           strcmp(name, "tool_list") == 0 ||
           strcmp(name, "tool_rollback") == 0 ||
           strcmp(name, "tool_history") == 0 ||
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
           strcmp(name, "shell_sandboxed") == 0 ||
           strcmp(name, "json_encode") == 0 || strcmp(name, "json_decode") == 0 ||
           strcmp(name, "file_exists") == 0 || strcmp(name, "file_list") == 0 ||
           strcmp(name, "file_delete") == 0 || strcmp(name, "file_append") == 0 ||
           strcmp(name, "file_rename") == 0 || strcmp(name, "file_stat") == 0 ||
           strcmp(name, "file_atomic_write") == 0 || strcmp(name, "file_temp") == 0 ||
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
           strcmp(name, "is_list") == 0 ||
           strcmp(name, "is_map") == 0 ||
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
           strcmp(name, "ws_request_headers") == 0 ||
           strcmp(name, "ws_request_path") == 0 ||
           strcmp(name, "live_js") == 0 ||
           /* WebSocket CLIENT (CDP / browser control) */
           strcmp(name, "wsc_connect") == 0 ||
           strcmp(name, "wsc_connect_tls") == 0 ||
           strcmp(name, "wsc_send") == 0 ||
           strcmp(name, "wsc_recv") == 0 ||
           strcmp(name, "wsc_set_handler") == 0 ||
           strcmp(name, "wsc_close") == 0 ||
           strcmp(name, "chrome_launch") == 0 ||
           /* WS binary frames + audio codecs (native voice agents) */
           strcmp(name, "ws_send_binary") == 0 ||
           strcmp(name, "audio_ulaw_to_pcm16") == 0 ||
           strcmp(name, "audio_pcm16_to_ulaw") == 0 ||
           strcmp(name, "audio_resample") == 0 ||
           /* String + base64 helpers (2026-05-15) */
           strcmp(name, "string_index_of") == 0 ||
           strcmp(name, "base64_encode") == 0 ||
           strcmp(name, "base64_decode") == 0 ||
           /* Ed25519 signature verify (TLS-gated, openssl EVP) */
           strcmp(name, "ed25519_verify") == 0 ||
           /* Bytes value type (length-carrying, NUL-safe) */
           strcmp(name, "bytes_from_base64") == 0 ||
           strcmp(name, "bytes_to_base64") == 0 ||
           strcmp(name, "byte_size") == 0 ||
           strcmp(name, "byte_at") == 0 ||
           strcmp(name, "byte_slice") == 0 ||
           strcmp(name, "bytes_concat") == 0 ||
           strcmp(name, "string_to_bytes") == 0 ||
           strcmp(name, "bytes_to_string") == 0 ||
           strcmp(name, "bytes_from_ints") == 0 ||
           strcmp(name, "byte") == 0 ||
           strcmp(name, "audio_ulaw_to_pcm16_b") == 0 ||
           strcmp(name, "audio_pcm16_to_ulaw_b") == 0 ||
           strcmp(name, "audio_resample_b") == 0 ||
           /* Binary-safe file I/O + char codes + math (dogfood primitives) */
           strcmp(name, "file_read_bytes") == 0 ||
           strcmp(name, "file_write_bytes") == 0 ||
           strcmp(name, "ord") == 0 ||
           strcmp(name, "codepoint_at") == 0 ||
           strcmp(name, "to_float") == 0 ||
           strcmp(name, "to_int") == 0 ||
           strcmp(name, "uuid") == 0 ||
           strcmp(name, "now_iso") == 0 ||
           strcmp(name, "math_sqrt") == 0 || strcmp(name, "math_sin") == 0 ||
           strcmp(name, "math_cos") == 0 || strcmp(name, "math_pow") == 0 ||
           strcmp(name, "math_exp") == 0 || strcmp(name, "math_log") == 0 ||
           strcmp(name, "math_floor") == 0 || strcmp(name, "math_ceil") == 0 ||
           strcmp(name, "math_round") == 0 ||
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
           strcmp(name, "pid_alive") == 0 ||
           /* Phase 17: LLM streaming */
           strcmp(name, "http_post_stream") == 0 ||
           /* Phase 18: terminal introspection */
           strcmp(name, "term_cols") == 0 ||
           strcmp(name, "term_rows") == 0 ||
           strcmp(name, "stream_content_rows") == 0 ||
           /* Phase 19: interactive picker */
           strcmp(name, "read_choice") == 0 ||
           /* argv-style exec (no shell) */
           strcmp(name, "exec_argv") == 0;
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

/* Returns the declared arity of a user-defined module function, or -1
 * if `name` is not one of the module's functions. Used by emit_call
 * for compile-time arity checking. */
static int module_func_nparams(cg_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->nfuncs; i++)
        if (strcmp(ctx->func_names[i], name) == 0) return ctx->func_nparams[i];
    return -1;
}

/* Compile-time arity check for calls to user-defined module functions.
 * Builtins are skipped (they take an array+count at the C ABI level and
 * each builtin enforces its own arity at runtime); cross-module calls
 * (Mod.fn) are also skipped because the called module isn't in our
 * func_names table. Reports the mismatch to stderr at the original .sw
 * source location and sets had_arity_error so sw_codegen() can fail
 * the whole compile before cc runs. */
static void check_user_call_arity(cg_ctx_t *ctx, const char *fname, int nargs, int line) {
    int expected = module_func_nparams(ctx, fname);
    if (expected < 0) return;            /* not a user-defined module fun */
    if (expected == nargs) return;
    fprintf(stderr, "swc: %s:%d: function '%s' takes %d arg%s, got %d\n",
            guess_sw_path(ctx->mod_name), line > 0 ? line : 0,
            fname, expected, expected == 1 ? "" : "s", nargs);
    ctx->had_arity_error = 1;
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
    case N_LIST_COMP:
        scan_spawns(ctx, n->v.lcomp.iter);
        scan_spawns(ctx, n->v.lcomp.body);
        if (n->v.lcomp.guard) scan_spawns(ctx, n->v.lcomp.guard);
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
    case N_LIST_COMP:
        collect_idents(n->v.lcomp.iter, ids, nids, max);
        collect_idents(n->v.lcomp.body, ids, nids, max);
        if (n->v.lcomp.guard) collect_idents(n->v.lcomp.guard, ids, nids, max);
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

/* Collect names that are the LHS of an N_ASSIGN anywhere in a tree.
 * Used by scan_lambdas to distinguish lambda-local vars (assigned
 * inside the body, NOT captures from outer scope) from real captures. */
static void collect_assigned_names(node_t *n, char names[][128], int *nnames, int max) {
    if (!n) return;
    if (n->type == N_ASSIGN) {
        const char *nm = n->v.assign.name;
        for (int i = 0; i < *nnames; i++) if (strcmp(names[i], nm) == 0) goto recur;
        if (*nnames < max) { strncpy(names[*nnames], nm, 127); (*nnames)++; }
recur:
        collect_assigned_names(n->v.assign.value, names, nnames, max);
        return;
    }
    switch (n->type) {
    case N_BLOCK:
        for (int i = 0; i < n->v.block.nstmts; i++)
            collect_assigned_names(n->v.block.stmts[i], names, nnames, max);
        break;
    case N_CALL:
        for (int i = 0; i < n->v.call.nargs; i++)
            collect_assigned_names(n->v.call.args[i], names, nnames, max);
        break;
    case N_IF:
        collect_assigned_names(n->v.iff.then_b, names, nnames, max);
        collect_assigned_names(n->v.iff.else_b, names, nnames, max);
        break;
    case N_CASE:
        for (int i = 0; i < n->v.casex.nclauses; i++)
            collect_assigned_names(n->v.casex.clauses[i]->v.clause.body, names, nnames, max);
        break;
    case N_TRY:
        collect_assigned_names(n->v.trycatch.body, names, nnames, max);
        collect_assigned_names(n->v.trycatch.catch_body, names, nnames, max);
        break;
    default: break;
    }
}

static void collect_pattern_binders(node_t *pat, const char *names[], int *cnt, int max);

/* Collect names bound by case-arm / receive-clause PATTERNS anywhere in a
 * tree, into the same char[][128] form collect_assigned_names uses. These are
 * lambda-LOCAL bindings (they come into scope inside the body when the arm
 * matches), so — exactly like N_ASSIGN targets and for-loop vars — they must
 * NOT be mistaken for free variables to capture from the enclosing scope.
 *
 * Without this, `fn(x){ case x { m -> m } }` collected `m` as a referenced
 * ident (it appears in the arm body) but neither as a param nor an assigned
 * name, so it leaked into the lambda's capture list — emitting a phantom
 * `sw_val_t *m = _args[..]` param plus a read of a nonexistent outer `m` at
 * the call site ("use of undeclared identifier 'm'"). The pattern binder is
 * the missing local-name source. */
static void collect_pattern_bound_in_arms(node_t *n, char names[][128],
                                           int *nnames, int max) {
    if (!n || *nnames >= max) return;
    switch (n->type) {
    case N_CASE:
        collect_pattern_bound_in_arms(n->v.casex.subject, names, nnames, max);
        for (int i = 0; i < n->v.casex.nclauses; i++) {
            node_t *cl = n->v.casex.clauses[i];
            const char *binders[64];
            int nb = 0;
            collect_pattern_binders(cl->v.clause.pattern, binders, &nb, 64);
            for (int b = 0; b < nb && *nnames < max; b++) {
                int dup = 0;
                for (int k = 0; k < *nnames; k++)
                    if (strcmp(names[k], binders[b]) == 0) { dup = 1; break; }
                if (!dup) { strncpy(names[*nnames], binders[b], 127); (*nnames)++; }
            }
            collect_pattern_bound_in_arms(cl->v.clause.body, names, nnames, max);
            if (cl->v.clause.guard)
                collect_pattern_bound_in_arms(cl->v.clause.guard, names, nnames, max);
        }
        break;
    case N_RECEIVE:
        for (int i = 0; i < n->v.recv.nclauses; i++) {
            node_t *cl = n->v.recv.clauses[i];
            const char *binders[64];
            int nb = 0;
            collect_pattern_binders(cl->v.clause.pattern, binders, &nb, 64);
            for (int b = 0; b < nb && *nnames < max; b++) {
                int dup = 0;
                for (int k = 0; k < *nnames; k++)
                    if (strcmp(names[k], binders[b]) == 0) { dup = 1; break; }
                if (!dup) { strncpy(names[*nnames], binders[b], 127); (*nnames)++; }
            }
            collect_pattern_bound_in_arms(cl->v.clause.body, names, nnames, max);
        }
        if (n->v.recv.after_body)
            collect_pattern_bound_in_arms(n->v.recv.after_body, names, nnames, max);
        break;
    case N_BLOCK:
        for (int i = 0; i < n->v.block.nstmts; i++)
            collect_pattern_bound_in_arms(n->v.block.stmts[i], names, nnames, max);
        break;
    case N_ASSIGN:
        collect_pattern_bound_in_arms(n->v.assign.value, names, nnames, max);
        break;
    case N_CALL:
        for (int i = 0; i < n->v.call.nargs; i++)
            collect_pattern_bound_in_arms(n->v.call.args[i], names, nnames, max);
        break;
    case N_BINOP:
        collect_pattern_bound_in_arms(n->v.binop.left, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.binop.right, names, nnames, max);
        break;
    case N_UNARY:
        collect_pattern_bound_in_arms(n->v.unary.operand, names, nnames, max);
        break;
    case N_SEND:
        collect_pattern_bound_in_arms(n->v.send.to, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.send.msg, names, nnames, max);
        break;
    case N_IF:
        collect_pattern_bound_in_arms(n->v.iff.cond, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.iff.then_b, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.iff.else_b, names, nnames, max);
        break;
    case N_PIPE:
        collect_pattern_bound_in_arms(n->v.pipe.val, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.pipe.func, names, nnames, max);
        break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < n->v.coll.count; i++)
            collect_pattern_bound_in_arms(n->v.coll.items[i], names, nnames, max);
        break;
    case N_FOR:
        collect_pattern_bound_in_arms(n->v.forloop.iter, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.forloop.body, names, nnames, max);
        break;
    case N_LIST_COMP:
        collect_pattern_bound_in_arms(n->v.lcomp.iter, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.lcomp.body, names, nnames, max);
        if (n->v.lcomp.guard)
            collect_pattern_bound_in_arms(n->v.lcomp.guard, names, nnames, max);
        break;
    case N_MAP:
        for (int i = 0; i < n->v.map.count; i++) {
            collect_pattern_bound_in_arms(n->v.map.keys[i], names, nnames, max);
            collect_pattern_bound_in_arms(n->v.map.vals[i], names, nnames, max);
        }
        break;
    case N_TRY:
        /* The catch binds an error var (`try {...} catch e {...}`) inside the
         * body — also a lambda-local, not a capture. */
        if (n->v.trycatch.err_var[0] && strcmp(n->v.trycatch.err_var, "_") != 0
            && *nnames < max) {
            int dup = 0;
            for (int k = 0; k < *nnames; k++)
                if (strcmp(names[k], n->v.trycatch.err_var) == 0) { dup = 1; break; }
            if (!dup) { strncpy(names[*nnames], n->v.trycatch.err_var, 127); (*nnames)++; }
        }
        collect_pattern_bound_in_arms(n->v.trycatch.body, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.trycatch.catch_body, names, nnames, max);
        break;
    case N_RANGE:
        collect_pattern_bound_in_arms(n->v.range.from, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.range.to, names, nnames, max);
        break;
    case N_LIST_CONS:
        collect_pattern_bound_in_arms(n->v.cons.head, names, nnames, max);
        collect_pattern_bound_in_arms(n->v.cons.tail, names, nnames, max);
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

        /* Find free variables: identifiers in body that aren't params
         * AND aren't assigned inside the body. Without the assigned-
         * check, a lambda like `fun(t) { total = ... ; f"{total}" }`
         * would try to capture `total` from outer scope and fail. */
        char body_idents[64][128];
        int nbody_idents = 0;
        collect_idents(n->v.fun.body, body_idents, &nbody_idents, 64);

        char assigned[64][128];
        int nassigned = 0;
        collect_assigned_names(n->v.fun.body, assigned, &nassigned, 64);
        /* Also treat case-arm / receive-clause / catch pattern bindings as
         * lambda-locals (see collect_pattern_bound_in_arms): they're bound
         * inside the body when an arm matches, so they're never captures. */
        collect_pattern_bound_in_arms(n->v.fun.body, assigned, &nassigned, 64);

        li->ncaptures = 0;
        for (int i = 0; i < nbody_idents; i++) {
            int is_param = 0;
            for (int j = 0; j < n->v.fun.nparams; j++)
                if (strcmp(body_idents[i], n->v.fun.params[j]) == 0) { is_param = 1; break; }
            if (is_param) continue;
            int is_local = 0;
            for (int j = 0; j < nassigned; j++)
                if (strcmp(body_idents[i], assigned[j]) == 0) { is_local = 1; break; }
            if (is_local) continue;
            if (li->ncaptures < 32)
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
    case N_LIST_COMP:
        scan_lambdas(ctx, n->v.lcomp.iter);
        scan_lambdas(ctx, n->v.lcomp.body);
        if (n->v.lcomp.guard) scan_lambdas(ctx, n->v.lcomp.guard);
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
    fprintf(f, "#include \"swarmrt_varena.h\"\n");   /* Ownership v2: regions + adopt */
    fprintf(f, "#include \"swarmrt_ets.h\"\n");
    fprintf(f, "#include \"swarmrt_builtins_studio.h\"\n");
    fprintf(f, "#include <stdio.h>\n");
    fprintf(f, "#include <stdlib.h>\n");
    fprintf(f, "#include <string.h>\n");
    fprintf(f, "#include <unistd.h>\n");
    fprintf(f, "#include <sys/stat.h>\n");
    fprintf(f, "#include <sys/time.h>\n");
    fprintf(f, "#include <errno.h>\n\n");

    /* Generated execution state — _sw_error, _sw_current_line/_sw_current_file,
     * and the _sw_trace call-stack ring buffer — is PER PROCESS, not a
     * scheduler-thread-local. A blocking op (sleep/receive) yields the fiber,
     * so a thread-local would leak across the context switch: a try/catch could
     * catch an unrelated fiber's error (UAF on its freed value), and a panic
     * could print another fiber's line/file/call-chain. swarmrt_native.h holds
     * the per-process `sw_gen_exec_t` + the `_sw_gen` pointer (swapped on every
     * context switch); swarmrt_builtins_studio.h #defines _sw_error and the
     * _sw_current_line/_sw_current_file/_sw_trace* macros to reach through it.
     * So codegen emits NO storage for these here — just the push/pop helpers,
     * which operate on the running process's block via those macros. Codegen
     * still emits `_sw_current_line = N; _sw_current_file = "src/Mod.sw";` once
     * per source line (cheap: now one pointer hop + two stores). Capacity 64
     * frames — deeper recursion sets the overflow flag rather than blowing up. */
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
        "    if (a->type == SW_VAL_STRING || b->type == SW_VAL_STRING)\n"
        "        _sw_runtime_panic(\"cannot use + on text \\xe2\\x80\\x94 use ++ to concatenate (+ is numeric addition; ++ joins strings and lists)\");\n"
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
    /* print() is input-line-aware: when read_line() is engaged (raw-mode
     * line editor active) we serialise via _sw_term_lock and wipe+redraw
     * the input line around our output. Same scheme print_above() uses.
     * Fixes the "input gets clobbered while typing" symptom for any
     * print() called from a receive handler, background task, or wake
     * chain — no app-side changes needed. */
    fprintf(f,
        "static sw_val_t *_builtin_print(sw_val_t **a, int n) {\n"
        "    pthread_mutex_lock(&_sw_term_lock);\n"
        "    int _act = _sw_rl.active;\n"
        "    if (_act) fputs(\"\\r\\x1b[K\", stdout);\n"
        "    for (int i = 0; i < n; i++) { if (i) printf(\" \"); sw_val_print(a[i]); }\n"
        "    printf(\"\\n\");\n"
        "    if (_act && _sw_rl.cur_buf && *_sw_rl.cur_buf) {\n"
        "        _sw_rl_redraw_unlocked(_sw_rl.cur_prompt,\n"
        "                               *_sw_rl.cur_buf,\n"
        "                               _sw_rl.cur_len ? *_sw_rl.cur_len : 0,\n"
        "                               _sw_rl.cur_cursor ? *_sw_rl.cur_cursor : 0);\n"
        "    } else { fflush(stdout); }\n"
        "    pthread_mutex_unlock(&_sw_term_lock);\n"
        "    return sw_val_atom(\"ok\");\n"
        "}\n\n");


    fprintf(f,
        "static sw_val_t *_builtin_length(sw_val_t **a, int n) {\n"
        "    if (n < 1) return sw_val_int(0);\n"
        "    if (a[0]->type == SW_VAL_LIST || a[0]->type == SW_VAL_TUPLE)\n"
        "        return sw_val_int(a[0]->v.tuple.count);\n"
        "    if (a[0]->type == SW_VAL_STRING) return sw_val_int((int64_t)strlen(a[0]->v.str));\n"
        "    if (a[0]->type == SW_VAL_BYTES) return sw_val_int((int64_t)a[0]->v.bytes.len);\n"
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
        "    if (n < 1) return sw_val_nil();\n"
        "    if (a[0]->type == SW_VAL_INT) return sw_val_int(a[0]->v.i < 0 ? -a[0]->v.i : a[0]->v.i);\n"
        "    if (a[0]->type == SW_VAL_FLOAT) return sw_val_float(a[0]->v.f < 0 ? -a[0]->v.f : a[0]->v.f);\n"
        "    return sw_val_nil();\n"
        "}\n\n");

    /* Functional primitives: map, pmap, reduce, filter */
    fprintf(f,
        "static sw_val_t *_builtin_map(sw_val_t **a, int n) {\n"
        "    if (n < 2) return sw_val_list(NULL, 0);\n"
        "    /* Accept either order: map(fn, list) — historical Lisp/Erlang\n"
        "     * style — or map(list, fn) — Elixir style. Disambiguate by\n"
        "     * the runtime types of the args. */\n"
        "    sw_val_t *fn, *lst;\n"
        "    if (a[0] && a[0]->type == SW_VAL_FUN) { fn = a[0]; lst = a[1]; }\n"
        "    else if (a[1] && a[1]->type == SW_VAL_FUN) { lst = a[0]; fn = a[1]; }\n"
        "    else { fn = a[0]; lst = a[1]; }\n"
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
        "    /* Accept reduce(fn, list, acc) or reduce(list, fn, acc) — the\n"
        "     * latter is what the pipe produces (xs |> reduce(f, 0)). map and\n"
        "     * filter already had this tolerance; reduce lacking it made a\n"
        "     * piped reduce return its init value untouched. */\n"
        "    sw_val_t *fn, *lst;\n"
        "    if (a[0] && a[0]->type == SW_VAL_FUN) { fn = a[0]; lst = a[1]; }\n"
        "    else if (a[1] && a[1]->type == SW_VAL_FUN) { lst = a[0]; fn = a[1]; }\n"
        "    else { fn = a[0]; lst = a[1]; }\n"
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
        "    /* Accept filter(fn, list) or filter(list, fn). */\n"
        "    sw_val_t *fn, *lst;\n"
        "    if (a[0] && a[0]->type == SW_VAL_FUN) { fn = a[0]; lst = a[1]; }\n"
        "    else if (a[1] && a[1]->type == SW_VAL_FUN) { lst = a[0]; fn = a[1]; }\n"
        "    else { fn = a[0]; lst = a[1]; }\n"
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
        /* Ownership v2: take + adopt the SPAWN region holding fn+item into this
         * worker's arena (freed at worker exit) instead of leaking global copies. */
        "    { sw_value_arena_t *_r = sw_self_take_spawn_region(), *_cv = sw_self_varena();\n"
        "      if (_cv && _r) sw_varena_adopt(_cv, _r); }\n"
        "    sw_val_t *arg[] = {w->item};\n"
        "    sw_val_t *result = sw_val_apply(w->fn, arg, 1);\n"
        "    sw_val_t *ti[] = {sw_val_int(w->idx), result};\n"
        "    sw_val_t *msg = sw_val_tuple(ti, 2);\n"
        "    sw_send_value(w->caller, 11, msg);\n"  /* result → message region the caller adopts */
        "    free(w);\n"
        "}\n"
        "static sw_val_t *_builtin_pmap(sw_val_t **a, int n) {\n"
        "    if (n < 2) return sw_val_list(NULL, 0);\n"
        "    /* Accept either order: pmap(fn, list) or pmap(list, fn) —\n"
        "     * matches map/filter's permissive convention. Previously\n"
        "     * pmap was strict-order and returned [] when called as\n"
        "     * pmap(list, fn) (the order the docs showed). */\n"
        "    sw_val_t *fn, *lst;\n"
        "    if (a[0] && a[0]->type == SW_VAL_FUN) { fn = a[0]; lst = a[1]; }\n"
        "    else if (a[1] && a[1]->type == SW_VAL_FUN) { lst = a[0]; fn = a[1]; }\n"
        "    else { fn = a[0]; lst = a[1]; }\n"
        "    if (!lst || lst->type != SW_VAL_LIST) return sw_val_list(NULL, 0);\n"
        "    int cnt = lst->v.tuple.count;\n"
        "    if (cnt == 0) return sw_val_list(NULL, 0);\n"
        "    sw_process_t *self = sw_self();\n"
        "    sw_val_t **res = calloc(cnt, sizeof(sw_val_t*));\n"
        "    for (int i = 0; i < cnt; i++) {\n"
        "        _pmap_w_t *w = malloc(sizeof(_pmap_w_t));\n"
        /* Ownership v2: copy fn + item into a SPAWN region the worker adopts on
         * entry (freed at worker exit) — no global-heap leak, and safe even if a
         * worker outlives the caller's 5s collect timeout. GC-off → global copy. */
        "        sw_value_arena_t *_wr = sw_self_varena() ? sw_varena_create_kind(256, SW_REGION_SPAWN) : (sw_value_arena_t*)0;\n"
        "        w->fn = _wr ? deep_copy_into(fn, _wr) : sw_val_deep_copy_global(fn);\n"
        "        w->item = _wr ? deep_copy_into(lst->v.tuple.items[i], _wr) : sw_val_deep_copy_global(lst->v.tuple.items[i]);\n"
        "        w->caller = self; w->idx = i;\n"
        "        sw_process_t *_wp = sw_spawn_owned(_pmap_entry, w, _wr);\n"
        "        if (!_wp) { if (_wr) sw_varena_free_all(_wr); free(w); }\n"
        "    }\n"
        "    for (int i = 0; i < cnt; i++) {\n"
        "        uint64_t tag;\n"
        /* Adopt each result region into THIS (caller) arena so the returned list
         * is self-contained in the caller's lifecycle (no leaked global copies). */
        "        void *raw = sw_recv_any_adopt(5000, &tag);\n"
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
    /* Static storage for module-level globals */
    for (int i = 0; i < mod->v.mod.nglobals; i++) {
        fprintf(f, "static sw_val_t *_g_%s_%s = NULL;\n",
                ctx->mod_name, mod->v.mod.globals[i].name);
    }
    fprintf(f, "\n");
}

/* =========================================================================
 * Spawn trampolines
 * ========================================================================= */

static void emit_spawn_trampolines(cg_ctx_t *ctx) {
    FILE *f = ctx->out;

    /* Generic lambda-spawn trampoline. Used by `spawn(fun() { ... })`
     * where the spawned value is an sw_val_t* of SW_VAL_FUN type.
     * Guarded by `#ifndef` so multi-module builds (which call this once
     * per module into one merged TU) don't get a redefinition error. */
    fprintf(f, "#ifndef _SW_LAMBDA_SPAWN_TRAMPOLINE_DEFINED\n");
    fprintf(f, "#define _SW_LAMBDA_SPAWN_TRAMPOLINE_DEFINED\n");
    fprintf(f, "static void _sw_lambda_spawn_trampoline(void *_raw) {\n");
    fprintf(f, "    sw_val_t *_fn = (sw_val_t *)_raw;\n");
    fprintf(f, "    if (_fn) sw_val_apply(_fn, NULL, 0);\n");
    fprintf(f, "}\n");
    /* Ownership v2: region-carrying lambda spawn. The closure (+ captures) is
     * copied into a SPAWN region the child adopts on entry, then freed at child
     * exit — closing the leak in the `spawn(f)` / `spawn(fn(){...})` form. */
    fprintf(f, "typedef struct { sw_val_t *fn; } _sw_lam_sp_t;\n");
    fprintf(f, "static void _sw_lambda_spawn_trampoline2(void *_raw) {\n");
    fprintf(f, "    _sw_lam_sp_t *_s = (_sw_lam_sp_t *)_raw;\n");
    fprintf(f, "    { sw_value_arena_t *_r = sw_self_take_spawn_region(), *_cv = sw_self_varena();\n");
    fprintf(f, "      if (_cv && _r) sw_varena_adopt(_cv, _r); }\n");
    fprintf(f, "    if (_s->fn) sw_val_apply(_s->fn, NULL, 0);\n");
    fprintf(f, "    free(_s);\n");
    fprintf(f, "}\n");
    fprintf(f, "#endif\n\n");

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
        /* Ownership v2: take + adopt the spawn region (which owns the copied args)
         * into this child's arena — freed at child exit. Taking it (read+clear)
         * means a later kill won't double-free it. The arg pointers (_s->aN)
         * already point into the adopted chunks. */
        fprintf(f, "    { sw_value_arena_t *_r = sw_self_take_spawn_region(), *_cv = sw_self_varena();\n");
        fprintf(f, "      if (_cv && _r) sw_varena_adopt(_cv, _r); }\n");
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

/* Build the C expression that constructs the lookup KEY for a map-pattern
 * entry, mirroring pattern_match's N_MAP case in swarmrt_lang.c: keys are
 * literals — atoms (from `name:`), strings, or ints. Writes into `out`;
 * returns 1 on success, 0 for an unsupported key node (caller must then emit
 * a never-match condition, exactly as the interpreter `return 0`s). */
static int emit_map_key_expr(node_t *kn, char *out, size_t osz) {
    if (kn->type == N_ATOM) {
        snprintf(out, osz, "sw_val_atom(\"%s\")", kn->v.sval);
        return 1;
    } else if (kn->type == N_STRING) {
        char *esc = c_escape_str(kn->v.sval);
        snprintf(out, osz, "sw_val_string(\"%s\")", esc);
        free(esc);
        return 1;
    } else if (kn->type == N_INT) {
        snprintf(out, osz, "sw_val_int(%lldLL)", (long long)kn->v.ival);
        return 1;
    }
    return 0;
}

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
        /* [h | t] pattern: list with at least 1 element, head matches, and
         * — mirroring pattern_match's N_LIST_CONS in swarmrt_lang.c — the
         * TAIL (the list minus its head) matches pat->v.cons.tail. Without
         * the tail recursion `[h | []]` would match any non-empty list and
         * `[h | [a,b]]` would ignore the tail shape entirely. */
        fprintf(f, "(%s->type == SW_VAL_LIST && %s->v.tuple.count >= 1", val, val);
        /* Check head pattern */
        {
            char head_item[512];
            snprintf(head_item, sizeof(head_item), "%s->v.tuple.items[0]", val);
            fprintf(f, " && ");
            emit_pattern_cond(ctx, pat->v.cons.head, head_item);
        }
        /* Check tail pattern against the rest-of-list. An IDENT tail (the
         * common `[h | t]`) always matches, so skip the redundant call to
         * keep the generated condition lean. */
        if (pat->v.cons.tail->type != N_IDENT) {
            char tail_item[512];
            snprintf(tail_item, sizeof(tail_item),
                     "sw_val_list(%s->v.tuple.items + 1, %s->v.tuple.count - 1)", val, val);
            fprintf(f, " && ");
            emit_pattern_cond(ctx, pat->v.cons.tail, tail_item);
        }
        fprintf(f, ")");
        break;
    case N_MAP:
        /* %{k: subpat, ...} pattern. Mirrors pattern_match's N_MAP in
         * swarmrt_lang.c: value must be a map, and for EACH (key, subpat)
         * the key must be present (sw_val_map_get != nil) AND its looked-up
         * value must match subpat. A key whose lookup returns nil ⇒ NO match
         * (we deliberately do NOT distinguish absent-key from present-nil —
         * sw_val_map_get can't, and diverging re-opens the parity gap). */
        fprintf(f, "(%s->type == SW_VAL_MAP", val);
        for (int i = 0; i < pat->v.map.count; i++) {
            char keyexpr[256];
            if (!emit_map_key_expr(pat->v.map.keys[i], keyexpr, sizeof(keyexpr))) {
                /* Unsupported key node: interpreter `return 0`s → never match. */
                fprintf(f, " && 0");
                continue;
            }
            char lookup[512];
            snprintf(lookup, sizeof(lookup), "sw_val_map_get(%s, %s)", val, keyexpr);
            /* Present (non-nil) ... */
            fprintf(f, " && %s->type != SW_VAL_NIL", lookup);
            /* ... and the looked-up value matches the sub-pattern. */
            fprintf(f, " && ");
            emit_pattern_cond(ctx, pat->v.map.vals[i], lookup);
        }
        fprintf(f, ")");
        break;
    default:
        fprintf(f, "1");
        break;
    }
}

/* Collect every binder name in a pattern (skipping `_`) into `names`.
 * Used by check_linear_pattern to detect a duplicate binder like {x, x}
 * before it reaches cc as an opaque "redefinition of 'x'". */
static void collect_pattern_binders(node_t *pat, const char *names[], int *cnt, int max) {
    if (!pat || *cnt >= max) return;
    switch (pat->type) {
    case N_IDENT:
        if (strcmp(pat->v.sval, "_") != 0 && *cnt < max)
            names[(*cnt)++] = pat->v.sval;
        break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < pat->v.coll.count; i++)
            collect_pattern_binders(pat->v.coll.items[i], names, cnt, max);
        break;
    case N_LIST_CONS:
        collect_pattern_binders(pat->v.cons.head, names, cnt, max);
        collect_pattern_binders(pat->v.cons.tail, names, cnt, max);
        break;
    case N_MAP:
        for (int i = 0; i < pat->v.map.count; i++)
            collect_pattern_binders(pat->v.map.vals[i], names, cnt, max);
        break;
    default: break;
    }
}

/* sw patterns are LINEAR: each variable may be bound at most once. A
 * nonlinear pattern like {x, x} previously leaked a raw cc "redefinition
 * of 'x'". Detect the duplicate here and emit a Gleam-style hint instead.
 * Cheap, self-contained pre-pass — no state threaded through the binder. */
static void check_linear_pattern(cg_ctx_t *ctx, node_t *pat, int line) {
    const char *names[64];
    int cnt = 0;
    collect_pattern_binders(pat, names, &cnt, 64);
    for (int i = 0; i < cnt; i++)
        for (int j = i + 1; j < cnt; j++)
            if (strcmp(names[i], names[j]) == 0) {
                fprintf(stderr, "swc: src/%s.sw:%d: sw patterns are linear \xe2\x80\x94 '%s' is bound twice; bind once and compare with a guard (when %s == ...)\n",
                        ctx->mod_name, line, names[i], names[i]);
                ctx->had_arity_error = 1;
                return;
            }
}

static void emit_pattern_bind(cg_ctx_t *ctx, node_t *pat, const char *val) {
    FILE *f = ctx->out;
    switch (pat->type) {
    case N_IDENT:
        /* The conventional anonymous-bind `_` means "I don't care
         * about this value". Multiple `_` in one pattern (e.g.
         * `{'EXIT', _, _}`) used to collide as `sw_val_t *_ = ...; ...
         * sw_val_t *_ = ...;` — a C redefinition error. Skip the
         * emission entirely; no name to bind. */
        if (strcmp(pat->v.sval, "_") == 0) break;
        /* Always emit a fresh declaration (scoped to the if-block).
         * Also track it so the clause body can reference it. */
        fprintf(f, "        sw_val_t *%s = %s;\n", mangle_for_c(pat->v.sval), val);
        declare_var(ctx, pat->v.sval);
        break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < pat->v.coll.count; i++) {
            char item[512];
            snprintf(item, sizeof(item), "%s->v.tuple.items[%d]", val, i);
            emit_pattern_bind(ctx, pat->v.coll.items[i], item);
        }
        break;
    case N_LIST_CONS: {
        char head_item[512];
        snprintf(head_item, sizeof(head_item), "%s->v.tuple.items[0]", val);
        emit_pattern_bind(ctx, pat->v.cons.head, head_item);
        /* Bind through the tail (rest-of-list). For the common IDENT tail
         * (`[h | t]`) bind it directly; otherwise recurse so nested tail
         * patterns like `[h | [a, b]]` bind their inner names too. */
        if (pat->v.cons.tail->type == N_IDENT) {
            if (strcmp(pat->v.cons.tail->v.sval, "_") != 0) {
                fprintf(f, "        sw_val_t *%s = sw_val_list(%s->v.tuple.items + 1, %s->v.tuple.count - 1);\n",
                        mangle_for_c(pat->v.cons.tail->v.sval), val, val);
                declare_var(ctx, pat->v.cons.tail->v.sval);
            }
        } else {
            char tail_item[512];
            snprintf(tail_item, sizeof(tail_item),
                     "sw_val_list(%s->v.tuple.items + 1, %s->v.tuple.count - 1)", val, val);
            emit_pattern_bind(ctx, pat->v.cons.tail, tail_item);
        }
        break;
    }
    case N_MAP:
        /* Bind each subpattern against the value at its key, mirroring
         * pattern_match's N_MAP recursion. By the time we bind, the
         * condition has already guaranteed the key is present, so the
         * looked-up value is the matched (non-nil) value. */
        for (int i = 0; i < pat->v.map.count; i++) {
            char keyexpr[256];
            if (!emit_map_key_expr(pat->v.map.keys[i], keyexpr, sizeof(keyexpr)))
                continue;
            char lookup[512];
            snprintf(lookup, sizeof(lookup), "sw_val_map_get(%s, %s)", val, keyexpr);
            emit_pattern_bind(ctx, pat->v.map.vals[i], lookup);
        }
        break;
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

    if (strcmp(op, "+") == 0) {
        /* Gleam-style compile-time error when BOTH operands are string
         * literals: "a" + "b" is the classic Python/JS reflex. The
         * runtime guard in _op_add catches the dynamic case; this catches
         * the obvious literal case before cc even runs. */
        if (n->v.binop.left->type == N_STRING && n->v.binop.right->type == N_STRING) {
            fprintf(stderr, "swc: src/%s.sw:%d: cannot use + on text \xe2\x80\x94 use ++ to concatenate (+ is numeric addition; ++ joins strings and lists)\n",
                    ctx->mod_name, n->line);
            ctx->had_arity_error = 1;
        }
        fprintf(f, "    sw_val_t *%s = _op_add(%s, %s);\n", res, left, right);
    }
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
    "string_truncate", "strip_html", "clean_json", "is_list", "is_map",
    "base64_encode", "base64_decode",
    "bytes_from_base64", "bytes_to_base64", "byte_size", "byte_at",
    "byte_slice", "bytes_concat", "string_to_bytes", "bytes_to_string",
    "bytes_from_ints", "byte", "ord", "codepoint_at", "to_float",
    "to_int", "uuid", "now_iso",
    "math_sqrt", "math_sin", "math_cos", "math_pow", "math_exp", "math_log",
    "math_floor", "math_ceil", "math_round",
    "audio_ulaw_to_pcm16_b", "audio_pcm16_to_ulaw_b", "audio_resample_b",
    "json_encode", "json_decode", "json_get", "json_escape",
    "spawn", "self", "send", "register", "whereis",
    "ets_new", "ets_put", "ets_get", "ets_delete", "ets_update_counter", "ets_cas",
    "ets_take", "ets_update", "ets_list", "ets_count",
    "file_read", "file_write", "file_read_bytes", "file_write_bytes",
    "file_exists", "file_delete", "file_list",
    "file_append", "file_mkdir",
    "file_rename", "file_stat", "file_atomic_write", "file_temp",
    "http_get", "http_post", "http_request", "http_post_stream", "http_listen", "http_respond",
    "ws_send", "ws_close", "ws_set_handler", "ws_send_binary",
    "ws_request_headers", "ws_request_path",
    "wsc_connect", "wsc_connect_tls", "wsc_send", "wsc_recv", "wsc_set_handler", "wsc_close",
    "audio_ulaw_to_pcm16", "audio_pcm16_to_ulaw", "audio_resample",
    "chrome_launch", "term_cols", "term_rows", "stream_content_rows",
    "read_line", "read_char", "read_choice",
    "sleep", "timestamp", "getenv", "sys_exit", "shell", "pid_alive",
    "random_int", "supervise",
    "dyn_supervisor", "sup_start_child", "sup_terminate_child", "sup_count_children",
    "tool_define", "tool_call", "tool_list", "tool_rollback", "tool_history",
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
 * to stderr and mark the codegen as having seen an unknown call so
 * sw_codegen() returns -1 (swc aborts before cc, no cascading C
 * "undeclared function" wave). Up to 3 suggestions, sorted by
 * distance.
 *
 * Threshold:
 *   - long names (len >= 8): distance ≤ 3
 *   - shorter names: distance ≤ 2
 *   - very short (len ≤ 2): distance ≤ 1 (otherwise every 2-char
 *     identifier matches everything)
 *
 * The earlier formula was `min(3, len/2)`, which was tuned around the
 * README example `strng_length` → `string_length` (distance 1) but
 * gave bad recall for medium-length typos: `strng_lenghth` (distance
 * 2) sometimes squeaked through, while a less specifically-shaped
 * length-13 typo would silently produce nothing because the threshold
 * happened to be exactly 3 and the closest candidate was 4 away. The
 * new schedule is more generous for the >=8-char identifiers that
 * dominate real-world calls (most builtins are 8-20 chars). */
static void did_you_mean(cg_ctx_t *ctx, const char *fname, int line) {
    /* Mark the whole codegen as failing — the per-site message is the
     * error, sw_codegen() returns -1 once the walk completes. */
    ctx->had_unknown_fn = 1;

    int flen = (int)strlen(fname);
    int threshold;
    if (flen >= 8)      threshold = 3;
    else if (flen >= 3) threshold = 2;
    else                threshold = 1;

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

    /* Compile-time arity check for direct calls to user-defined module
     * functions. Builtins are skipped (they're variadic at the C ABI
     * level — array+count) and cross-module calls (Mod.fn) are also
     * skipped (the imported module's signatures aren't in our table).
     * Lambda / closure dispatch via is_declared is also intentionally
     * uncheckable here (the variable holds an opaque sw_val_t). */
    if (!is_builtin(fname) && !strchr(fname, '.'))
        check_user_call_arity(ctx, fname, nargs, n->line);

    /* Evaluate arguments */
    char arg_vars[16][32];
    for (int i = 0; i < nargs && i < 16; i++)
        emit_expr(ctx, n->v.call.args[i], 0, arg_vars[i], sizeof(arg_vars[i]));

    /* Tail call optimization: self-recursive call in tail position.
     *
     * Ownership v2 TURN-CHECKPOINT: a self-tail-call is one loop iteration. The
     * ONLY values that survive `goto _tail` are the recursion args (every other
     * turn value is dead after the jump). So: copy just the args into a FRESH
     * process arena (deep_copy_into is transitive — it pulls forward any
     * still-referenced adopted-message substructure too), swap it in, then
     * bulk-free the OLD arena (reclaiming this turn's garbage + unreferenced
     * adopted-message chunks). This bounds long-lived loops mid-life. ORDER is
     * load-bearing: copy-from-old-into-new BEFORE free-old. Skipped when there is
     * no process arena (SW_GC_OFF / interpreter) or on OOM — plain reassign. */
    if (tail && is_self_call(ctx, n)) {
        int np = nargs < ctx->cur_nparams ? nargs : ctx->cur_nparams;
        /* SCOPED turn-checkpoint: when this function has accumulated more than
         * the reset threshold ABOVE its entry floor, copy the recursion args
         * into a temp region, rewind the arena to the floor (reclaiming this
         * function's per-turn garbage + unreferenced adopted-message chunks, but
         * NOT the caller's values below the floor), then splice the args back in.
         * deep_copy_into is transitive (carries forward still-referenced adopted
         * substructure). Bounds long-lived loops without touching callers. */
        fprintf(f, "    { sw_value_arena_t *_gc_a = sw_self_varena();\n");
        fprintf(f, "      if (_gc_a && (_gc_a->total_bytes - _gc_floor.total_bytes) > SW_TURN_RESET_BYTES) {\n");
        fprintf(f, "        sw_value_arena_t *_gc_r = sw_varena_create_kind(256, SW_REGION_PROCESS);\n");
        fprintf(f, "        if (_gc_r) {\n");
        for (int i = 0; i < np; i++)
            fprintf(f, "          sw_val_t *_gc_t%d = deep_copy_into(%s, _gc_r);\n", i, arg_vars[i]);
        fprintf(f, "          sw_varena_reset_to(_gc_a, _gc_floor);\n");
        fprintf(f, "          sw_varena_adopt(_gc_a, _gc_r);\n");
        for (int i = 0; i < np; i++)
            fprintf(f, "          %s = _gc_t%d;\n", ctx->cur_params[i], i);
        fprintf(f, "        } else {\n");
        for (int i = 0; i < np; i++)
            fprintf(f, "          %s = %s;\n", ctx->cur_params[i], arg_vars[i]);
        fprintf(f, "        }\n");
        fprintf(f, "      } else {\n");
        for (int i = 0; i < np; i++)
            fprintf(f, "        %s = %s;\n", ctx->cur_params[i], arg_vars[i]);
        fprintf(f, "      }\n");
        fprintf(f, "    }\n");
        /* Reduction check at the loop backedge. A self-tail-call is the
         * language's only unbounded loop, and without this the compiled
         * loop never yields: a spawn storm on one scheduler kept every
         * child queued behind the running parent (80K live 128KB stacks
         * = ~160K VMAs, over Linux's default vm.max_map_count 65530 —
         * mmap/calloc then fail and the storm dies by SIGSEGV instead
         * of fairness). One TLS decrement per turn; yields every
         * SWARM_CONTEXT_REDS turns, same budget receive-blocking uses.
         * Both calls are no-ops outside a fiber (interpreter path). */
        fprintf(f, "    if (sw_check_reds()) sw_yield();\n");
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
        /* Gleam-style hint for the qualified-builtin reflex. Std.map and
         * Std.filter are REAL module functions now (lib/Std.sw aliases over
         * the global builtins — the Elixir-shaped guess models emit), so
         * only the names with no Std alias stay on this denylist:
         * Std.reduce can't exist (Std's own sum/product call the global
         * `reduce` bare; a same-named module fun would shadow it) and pmap
         * has no alias either. */
        const char *gf = dot + 1;
        if ((strcmp(gf, "reduce") == 0 || strcmp(gf, "pmap") == 0) &&
            strcmp(mod, "Std") == 0) {
            fprintf(stderr, "swc: src/%s.sw:%d: %s is a global builtin \xe2\x80\x94 write %s(fn, list, ...), not %s(...)\n",
                    ctx->mod_name, n->line, gf, gf, fname);
            ctx->had_unknown_fn = 1;
        }
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
             strcmp(fname, "unlink") == 0 || strcmp(fname, "demonitor") == 0 ||
             strcmp(fname, "exit_proc") == 0 || strcmp(fname, "trap_exit") == 0 ||
             strcmp(fname, "ets_new") == 0 || strcmp(fname, "ets_put") == 0 ||
             strcmp(fname, "ets_get") == 0 || strcmp(fname, "ets_delete") == 0 ||
             strcmp(fname, "ets_update_counter") == 0 || strcmp(fname, "ets_cas") == 0 ||
             strcmp(fname, "ets_take") == 0 || strcmp(fname, "ets_update") == 0 ||
             strcmp(fname, "sleep") == 0 || strcmp(fname, "getenv") == 0 ||
             strcmp(fname, "to_string") == 0 || strcmp(fname, "format") == 0 ||
             strcmp(fname, "timestamp") == 0 ||
             strcmp(fname, "file_write") == 0 || strcmp(fname, "file_read") == 0 ||
             strcmp(fname, "file_mkdir") == 0 || strcmp(fname, "http_post") == 0 ||
             strcmp(fname, "http_request") == 0 ||
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
             strcmp(fname, "shell_sandboxed") == 0 ||
             strcmp(fname, "json_encode") == 0 || strcmp(fname, "json_decode") == 0 ||
             strcmp(fname, "file_exists") == 0 || strcmp(fname, "file_list") == 0 ||
             strcmp(fname, "file_delete") == 0 || strcmp(fname, "file_append") == 0 ||
             strcmp(fname, "file_rename") == 0 || strcmp(fname, "file_stat") == 0 ||
             strcmp(fname, "file_atomic_write") == 0 || strcmp(fname, "file_temp") == 0 ||
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
             strcmp(fname, "is_list") == 0 ||
             strcmp(fname, "is_map") == 0 ||
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
             strcmp(fname, "ws_request_headers") == 0 ||
             strcmp(fname, "ws_request_path") == 0 ||
             strcmp(fname, "live_js") == 0 ||
             /* WebSocket CLIENT (CDP / browser control) */
             strcmp(fname, "wsc_connect") == 0 ||
             strcmp(fname, "wsc_connect_tls") == 0 ||
             strcmp(fname, "wsc_send") == 0 ||
             strcmp(fname, "wsc_recv") == 0 ||
             strcmp(fname, "wsc_set_handler") == 0 ||
             strcmp(fname, "wsc_close") == 0 ||
             strcmp(fname, "chrome_launch") == 0 ||
             /* WS binary frames + audio codecs (native voice agents) */
             strcmp(fname, "ws_send_binary") == 0 ||
             strcmp(fname, "audio_ulaw_to_pcm16") == 0 ||
             strcmp(fname, "audio_pcm16_to_ulaw") == 0 ||
             strcmp(fname, "audio_resample") == 0 ||
             /* String + base64 helpers */
             strcmp(fname, "string_index_of") == 0 ||
             strcmp(fname, "base64_encode") == 0 ||
             strcmp(fname, "base64_decode") == 0 ||
             /* Ed25519 signature verify (TLS-gated, openssl EVP) */
             strcmp(fname, "ed25519_verify") == 0 ||
             /* Bytes value type (length-carrying, NUL-safe) */
             strcmp(fname, "bytes_from_base64") == 0 ||
             strcmp(fname, "bytes_to_base64") == 0 ||
             strcmp(fname, "byte_size") == 0 ||
             strcmp(fname, "byte_at") == 0 ||
             strcmp(fname, "byte_slice") == 0 ||
             strcmp(fname, "bytes_concat") == 0 ||
             strcmp(fname, "string_to_bytes") == 0 ||
             strcmp(fname, "bytes_to_string") == 0 ||
             strcmp(fname, "bytes_from_ints") == 0 ||
             strcmp(fname, "byte") == 0 ||
             strcmp(fname, "audio_ulaw_to_pcm16_b") == 0 ||
             strcmp(fname, "audio_pcm16_to_ulaw_b") == 0 ||
             strcmp(fname, "audio_resample_b") == 0 ||
             /* Binary-safe file I/O + char codes + math (dogfood primitives) */
             strcmp(fname, "file_read_bytes") == 0 ||
             strcmp(fname, "file_write_bytes") == 0 ||
             strcmp(fname, "ord") == 0 ||
             strcmp(fname, "codepoint_at") == 0 ||
             strcmp(fname, "to_float") == 0 ||
             strcmp(fname, "to_int") == 0 ||
             strcmp(fname, "uuid") == 0 ||
             strcmp(fname, "now_iso") == 0 ||
             strcmp(fname, "math_sqrt") == 0 || strcmp(fname, "math_sin") == 0 ||
             strcmp(fname, "math_cos") == 0 || strcmp(fname, "math_pow") == 0 ||
             strcmp(fname, "math_exp") == 0 || strcmp(fname, "math_log") == 0 ||
             strcmp(fname, "math_floor") == 0 || strcmp(fname, "math_ceil") == 0 ||
             strcmp(fname, "math_round") == 0 ||
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
             strcmp(fname, "pid_alive") == 0 ||
             /* Phase 17: LLM streaming */
             strcmp(fname, "http_post_stream") == 0 ||
             /* Phase 18: terminal introspection */
             strcmp(fname, "term_cols") == 0 ||
             strcmp(fname, "term_rows") == 0 ||
             strcmp(fname, "stream_content_rows") == 0 ||
             /* Phase 19: interactive picker */
             strcmp(fname, "read_choice") == 0 ||
             /* Phase 20: process command-line arguments */
             strcmp(fname, "os_args") == 0 ||
             /* Phase 21: input-aware terminal output */
             strcmp(fname, "print_above") == 0 ||
             /* argv-style exec (no shell) */
             strcmp(fname, "exec_argv") == 0 ||
             /* DynamicSupervisor (runtime start_child) */
             strcmp(fname, "dyn_supervisor") == 0 ||
             strcmp(fname, "sup_start_child") == 0 ||
             strcmp(fname, "sup_terminate_child") == 0 ||
             strcmp(fname, "sup_count_children") == 0 ||
             /* Tool registry */
             strcmp(fname, "tool_define") == 0 ||
             strcmp(fname, "tool_call") == 0 ||
             strcmp(fname, "tool_list") == 0 ||
             strcmp(fname, "tool_rollback") == 0 ||
             strcmp(fname, "tool_history") == 0)
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
        /* Unknown function — print did-you-mean to stderr. did_you_mean
         * also sets ctx->had_unknown_fn, so sw_codegen() returns -1 and
         * swc aborts before invoking cc; no cascade of "undeclared
         * function" noise from clang on top of our own message.
         *
         * We still emit a placeholder call (NOT relied on for the user-
         * visible error any more) because the rest of emit_function /
         * the surrounding statement walker expects emit_call to produce
         * a valid C variable name. The emitted C is unreachable: swc
         * never invokes cc once had_unknown_fn is set. */
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
    /* sw_send_dispatch handles both SW_VAL_PID (local) and
     * SW_VAL_REMOTE_PID (cross-node TCP). Previously this called
     * sw_send_tagged(to->v.pid, ...) directly, which crashed for
     * remote pids since they don't have a pid pointer. */
    fprintf(f, "    sw_send_dispatch(%s, %s);\n", to_var, msg_var);
    strncpy(out, msg_var, osz - 1);
}

/* Wrap a freshly-spawned sw_process_t* (held in C var `proc_var`) into the
 * sw value `spawn` returns. For plain spawn that's a SW_VAL_PID; for
 * spawn_monitor it atomically monitors the child and returns {pid, ref}.
 * Atomic here means the child can't have its slot recycled between spawn
 * and monitor — we still hold the live sw_process_t*. */
static void emit_spawn_wrap(cg_ctx_t *ctx, int is_monitor,
                            const char *proc_var, char *out, int osz) {
    FILE *f = ctx->out;
    char res[32];
    fresh_var(ctx, res, sizeof(res));
    if (is_monitor) {
        fprintf(f, "    uint64_t %s_ref = sw_monitor(%s);\n", res, proc_var);
        fprintf(f, "    sw_val_t *%s_items[2] = { sw_val_pid(%s), sw_val_int((int64_t)%s_ref) };\n",
                res, proc_var, res);
        fprintf(f, "    sw_val_t *%s = sw_val_tuple(%s_items, 2);\n", res, res);
    } else {
        fprintf(f, "    sw_val_t *%s = sw_val_pid(%s);\n", res, proc_var);
    }
    strncpy(out, res, osz - 1);
}

static void emit_spawn(cg_ctx_t *ctx, node_t *n, char *out, int osz) {
    FILE *f = ctx->out;
    node_t *inner = n->v.spawn.expr;
    int is_monitor = n->v.spawn.monitor;

    /* Fun-value form: spawn(fun() { ... }), spawn(f) where `f` is a
     * closure-valued local/global, or spawn(worker) where `worker` is a
     * by-name module function. Any inner that ISN'T a direct call
     * `spawn(worker(args))` is evaluated to a sw_val_t value and handed
     * off to a generic trampoline that calls sw_val_apply with 0 args.
     * At runtime the trampoline no-ops on a non-callable value, so a
     * misuse degrades to an empty process rather than a crash.
     *
     * This is what makes `f = fn(){...} ; spawn(f)` work (the form LLMs
     * write); previously only the inline `spawn(fn(){...})` literal and
     * `spawn(named_call(...))` were accepted, and a closure-valued local
     * hit the hard "requires a function call" error below. */
    if (inner && inner->type != N_CALL) {
        char fn_val[32];
        emit_expr(ctx, inner, 0, fn_val, sizeof(fn_val));
        char pv[32], lr[32], ls[32];
        fresh_var(ctx, pv, sizeof(pv));
        fresh_var(ctx, lr, sizeof(lr));
        fresh_var(ctx, ls, sizeof(ls));
        /* Ownership v2: copy the closure (+ captures) into a SPAWN region the
         * child adopts on entry (freed at child exit), not the leaking global
         * heap. SW_GC_OFF / no-arena → fall back to the v1 global-heap copy. */
        fprintf(f, "    sw_value_arena_t *%s = sw_self_varena() ? sw_varena_create_kind(256, SW_REGION_SPAWN) : NULL;\n", lr);
        fprintf(f, "    _sw_lam_sp_t *%s = malloc(sizeof(_sw_lam_sp_t));\n", ls);
        fprintf(f, "    %s->fn = %s ? deep_copy_into(%s, %s) : sw_val_deep_copy_global(%s);\n",
                ls, lr, fn_val, lr, fn_val);
        fprintf(f, "    sw_process_t *%s = sw_spawn_owned(_sw_lambda_spawn_trampoline2, %s, %s);\n", pv, ls, lr);
        /* Spawn failure must be LOUD. Continuing with a pid wrapping NULL
         * made sends silently vanish at the SW_MAX_PROCS ceiling. */
        fprintf(f, "    if (!%s) { if (%s) sw_varena_free_all(%s); free(%s);\n", pv, lr, lr, ls);
        fprintf(f, "        _sw_runtime_panic(\"spawn failed: process table full \\xe2\\x80\\x94 raise SW_MAX_PROCS or reduce live processes\"); }\n");
        emit_spawn_wrap(ctx, is_monitor, pv, out, osz);
        return;
    }

    int sp_id = find_spawn_id(ctx, inner);
    if (sp_id < 0) {
        /* An N_CALL we never recorded a spawn-site for (e.g. it lives in a
         * context scan_spawns doesn't walk). Refuse rather than silently
         * returning pid 0, and point at the working shapes. */
        fprintf(stderr, "swc: src/%s.sw:%d: %s(...) requires a function call (e.g. `spawn(worker())`), a lambda (e.g. `spawn(fn() { body })`), or a function value (e.g. `f = fn(){...} ; spawn(f)`)\n",
                ctx->mod_name, n->line, is_monitor ? "spawn_monitor" : "spawn");
        ctx->had_arity_error = 1;
        char res[32];
        fresh_var(ctx, res, sizeof(res));
        fprintf(f, "    sw_val_t *%s = sw_val_pid(NULL);\n", res);
        strncpy(out, res, osz - 1);
        return;
    }

    spawn_info_t *sp = &ctx->spawns[sp_id];

    /* Compile-time arity check for `spawn helper(...)` when helper is a
     * user-defined module function. (Cross-module spawns aren't in our
     * table; builtin names can't be spawned at all.) */
    if (!strchr(sp->func_name, '.'))
        check_user_call_arity(ctx, sp->func_name, sp->nargs, n->line);

    char sa[32], rg[32];
    fresh_var(ctx, sa, sizeof(sa));
    fresh_var(ctx, rg, sizeof(rg));
    fprintf(f, "    _%s_sp%d_t *%s = malloc(sizeof(_%s_sp%d_t));\n",
            ctx->mod_name, sp_id, sa, ctx->mod_name, sp_id);

    /* Ownership v2: copy the args into a SPAWN region the child adopts on entry
     * (freed at child exit) instead of leaking them on the global heap. When the
     * process has no arena (SW_GC_OFF / interpreter), fall back to the v1
     * global-heap copy so the A/B path and SW_GC_OFF stay intact. */
    fprintf(f, "    sw_value_arena_t *%s = sw_self_varena() ? sw_varena_create_kind(256, SW_REGION_SPAWN) : NULL;\n", rg);
    for (int i = 0; i < sp->nargs; i++) {
        char arg[32];
        emit_expr(ctx, inner->v.call.args[i], 0, arg, sizeof(arg));
        fprintf(f, "    %s->a%d = %s ? deep_copy_into(%s, %s) : sw_val_deep_copy_global(%s);\n",
                sa, i, rg, arg, rg, arg);
    }

    char pv[32];
    fresh_var(ctx, pv, sizeof(pv));
    /* sw_spawn_owned records the region on the child so a pre-trampoline kill
     * reclaims it; the child adopts it on entry. */
    fprintf(f, "    sw_process_t *%s = sw_spawn_owned(_%s_sp%d_entry, %s, %s);\n",
            pv, ctx->mod_name, sp_id, sa, rg);
    /* Spawn failure: child never started, region not recorded — reclaim,
     * then panic LOUDLY. Continuing with a pid wrapping NULL made every
     * send to it silently vanish at the SW_MAX_PROCS ceiling. */
    fprintf(f, "    if (!%s) { if (%s) sw_varena_free_all(%s); free(%s);\n", pv, rg, rg, sa);
    fprintf(f, "        _sw_runtime_panic(\"spawn failed: process table full \\xe2\\x80\\x94 raise SW_MAX_PROCS or reduce live processes\"); }\n");
    emit_spawn_wrap(ctx, is_monitor, pv, out, osz);
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
    /* For runtime-injected signals (EXIT from a dying linked peer, DOWN
     * from a monitor) the payload is a sw_signal_t* not a sw_val_t* —
     * synthesise the documented tuple shape so the sw `receive {
     * {'EXIT', from, reason} -> ... }` and `{'DOWN', ref, 'process',
     * from, reason}` patterns match. Plain `send(pid, msg)` messages
     * pass through unchanged. */
    fprintf(f, "          sw_val_t *%s;\n", msg);
    fprintf(f, "          if (%s->tag == SW_TAG_EXIT && %s->payload) {\n", cur, cur);
    fprintf(f, "            sw_signal_t *_sig = (sw_signal_t *)%s->payload;\n", cur);
    /* Prefer the human-readable reason_str when present so trap_exit
     * handlers receive the panic message (e.g. "hd: list is empty")
     * instead of an opaque integer. Falls back to the int reason for
     * normal/killed exits where there's no message. */
    fprintf(f, "            sw_val_t *_reason = _sig->reason_str\n");
    fprintf(f, "                ? sw_val_string(_sig->reason_str)\n");
    fprintf(f, "                : sw_val_int((int64_t)_sig->reason);\n");
    /* Deliver the source as a SW_VAL_PID (resolved from the slab,
     * dead-or-alive) so `{'EXIT', from, _} -> from == some_pid` works.
     * Previously this was sw_val_int(pid), which never == a spawn() pid. */
    fprintf(f, "            sw_val_t *_items[3] = { sw_val_atom(\"EXIT\"), sw_val_pid(sw_find_by_pid_any(_sig->pid)), _reason };\n");
    fprintf(f, "            %s = sw_val_tuple(_items, 3);\n", msg);
    fprintf(f, "          } else if (%s->tag == SW_TAG_DOWN && %s->payload) {\n", cur, cur);
    fprintf(f, "            sw_signal_t *_sig = (sw_signal_t *)%s->payload;\n", cur);
    fprintf(f, "            sw_val_t *_reason = _sig->reason_str\n");
    fprintf(f, "                ? sw_val_string(_sig->reason_str)\n");
    fprintf(f, "                : sw_val_int((int64_t)_sig->reason);\n");
    /* {'DOWN', ref, 'process', PID, reason}: PID is a real SW_VAL_PID
     * resolved from the slab (dead-or-alive) so the idiomatic supervisor
     * pattern `dpid == child_pid` matches. ref stays an int (monitor()
     * returns an int ref). Previously PID was sw_val_int → always !=. */
    fprintf(f, "            sw_val_t *_items[5] = { sw_val_atom(\"DOWN\"), sw_val_int((int64_t)_sig->ref), sw_val_atom(\"process\"), sw_val_pid(sw_find_by_pid_any(_sig->pid)), _reason };\n");
    fprintf(f, "            %s = sw_val_tuple(_items, 5);\n", msg);
    fprintf(f, "          } else {\n");
    fprintf(f, "            %s = %s->payload ? (sw_val_t *)%s->payload : sw_val_nil();\n",
            msg, cur, cur);
    fprintf(f, "          }\n");

    /* Pattern matching clauses */
    for (int i = 0; i < n->v.recv.nclauses; i++) {
        node_t *cl = n->v.recv.clauses[i];
        check_linear_pattern(ctx, cl->v.clause.pattern, cl->line);
        fprintf(f, "          %sif (", i == 0 ? "" : "} else ");
        emit_pattern_cond(ctx, cl->v.clause.pattern, msg);
        fprintf(f, ") {\n");

        int saved_ndeclared = ctx->ndeclared;

        /* Guard check — if guard fails, don't match.
         *
         * After a match: remove the msg from the mailbox, run the
         * body, then release the sw_msg_t envelope. The PAYLOAD
         * (sw_val_t) is left alive on purpose — pattern bindings and
         * body_res can alias subparts of it, so the body owns it
         * after match. The envelope (sw_msg_t) is small and not
         * aliased; releasing it lets msg_alloc reuse it via the
         * per-thread freelist instead of hitting malloc on every
         * receive. Reduces glibc arena pressure during spawn-storms
         * (R6-A observed: heavy send/receive workloads previously
         * forced every msg_alloc through malloc and tcache stayed
         * empty). */
        if (cl->v.clause.guard) {
            emit_pattern_bind(ctx, cl->v.clause.pattern, msg);
            char guard_res[32];
            emit_expr(ctx, cl->v.clause.guard, 0, guard_res, sizeof(guard_res));
            fprintf(f, "            if (sw_val_is_truthy(%s)) {\n", guard_res);
            fprintf(f, "              sw_mailbox_remove_msg(%s);\n", cur);
            fprintf(f, "              _matched = 1;\n");
            /* Ownership v2: ADOPT-SPLICE the message region into the receiver's
             * arena so the bound payload aliases live in receiver lifecycle
             * (freed at process exit / next turn checkpoint). Skips EXIT/DOWN
             * (pkind RAW) and the GC-off path (no receiver arena). */
            fprintf(f, "              { sw_value_arena_t *_rv = sw_self_varena(); if (%s->pkind == SW_PK_VALUE && %s->region && _rv) { sw_varena_adopt(_rv, %s->region); %s->region = NULL; } }\n", cur, cur, cur, cur);
            fprintf(f, "              sw_msg_release(%s);\n", cur);
            char body_res[32];
            emit_expr(ctx, cl->v.clause.body, tail, body_res, sizeof(body_res));
            if (body_res[0])
                fprintf(f, "              %s = %s;\n", res, body_res);
            fprintf(f, "            }\n");
        } else {
            fprintf(f, "            sw_mailbox_remove_msg(%s);\n", cur);
            fprintf(f, "            _matched = 1;\n");
            emit_pattern_bind(ctx, cl->v.clause.pattern, msg);
            /* Ownership v2: ADOPT-SPLICE the message region (see guard arm). */
            fprintf(f, "            { sw_value_arena_t *_rv = sw_self_varena(); if (%s->pkind == SW_PK_VALUE && %s->region && _rv) { sw_varena_adopt(_rv, %s->region); %s->region = NULL; } }\n", cur, cur, cur, cur);
            fprintf(f, "            sw_msg_release(%s);\n", cur);
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
    char subj0[32], subj[32], res[32];
    emit_expr(ctx, n->v.casex.subject, 0, subj0, sizeof(subj0));
    /* Snapshot the subject into a fresh temp before any arm binds. When
     * the subject is a plain variable and an arm re-binds the same name
     * (`case n { n when n > 0 -> ... }`), binding straight off the
     * subject emitted `sw_val_t *n = n;` — C self-initialization, so the
     * arm-local n read indeterminate memory (wrong guard results at
     * best, SIGSEGV at worst). Tuple/cons/map sub-binds had the same
     * hazard through `n->v.tuple.items[i]`. One pointer copy fixes the
     * whole class; `with` desugars to case, so it is covered too. */
    fresh_var(ctx, subj, sizeof(subj));
    fprintf(f, "    sw_val_t *%s = %s;\n", subj, subj0);
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
        check_linear_pattern(ctx, cl->v.clause.pattern, cl->line);
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

        /* Same compile-time arity check as emit_call. */
        if (!is_builtin(fname) && !strchr(fname, '.'))
            check_user_call_arity(ctx, fname, nargs, func->line);

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

        /* Module-qualified call: `x |> Std.sum()` must become
         * `Std_sum(args, n)`, NOT `<current_mod>_Std.sum(...)`. */
        const char *dot = strchr(fname, '.');
        if (dot) {
            char mod[128], fn[128];
            int mlen = (int)(dot - fname);
            if (mlen >= (int)sizeof(mod)) mlen = sizeof(mod) - 1;
            strncpy(mod, fname, mlen); mod[mlen] = '\0';
            strncpy(fn, dot + 1, sizeof(fn) - 1); fn[sizeof(fn) - 1] = '\0';
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, %d);\n", res, mod, fn, arr, nargs);
        } else if (is_builtin(fname)) {
            fprintf(f, "    sw_val_t *%s = _builtin_%s(%s, %d);\n", res, fname, arr, nargs);
        } else {
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, %d);\n", res, ctx->mod_name, fname, arr, nargs);
        }

        strncpy(out, res, osz - 1);
    } else if (func->type == N_IDENT) {
        /* val |> func → func(val) */
        const char *fname = func->v.sval;

        /* Compile-time arity check: pipe always passes exactly 1 arg. */
        if (!is_builtin(fname) && !strchr(fname, '.'))
            check_user_call_arity(ctx, fname, 1, func->line);

        char arr[32];
        fresh_var(ctx, arr, sizeof(arr));
        fprintf(f, "    sw_val_t *%s[] = {%s};\n", arr, val);
        char res[32];
        fresh_var(ctx, res, sizeof(res));

        /* Same module-qualified handling as the N_CALL branch. */
        const char *dot = strchr(fname, '.');
        if (dot) {
            char mod[128], fn[128];
            int mlen = (int)(dot - fname);
            if (mlen >= (int)sizeof(mod)) mlen = sizeof(mod) - 1;
            strncpy(mod, fname, mlen); mod[mlen] = '\0';
            strncpy(fn, dot + 1, sizeof(fn) - 1); fn[sizeof(fn) - 1] = '\0';
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, 1);\n", res, mod, fn, arr);
        } else if (is_builtin(fname)) {
            fprintf(f, "    sw_val_t *%s = _builtin_%s(%s, 1);\n", res, fname, arr);
        } else {
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, 1);\n", res, ctx->mod_name, fname, arr);
        }

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
        } else if (is_global(ctx, n->v.sval)) {
            /* Module-level global (let x = ...) — read from static slot. */
            char v[32]; fresh_var(ctx, v, sizeof(v));
            fprintf(f, "    sw_val_t *%s = _g_%s_%s;\n", v, ctx->mod_name, n->v.sval);
            strncpy(out, v, osz - 1);
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
    case N_LIST_COMP: {
        /* [body for var in iter] or [body for var in iter when guard]
         * Emits: evaluate iter, allocate scratch array, loop with optional
         * guard filter, collect body results, build list via sw_val_list. */
        char iter_var[32];
        emit_expr(ctx, n->v.lcomp.iter, 0, iter_var, sizeof(iter_var));
        /* src_count = number of items in the source list */
        char src_cnt[32]; fresh_var(ctx, src_cnt, sizeof(src_cnt));
        fprintf(f, "    int %s = (%s->type == SW_VAL_LIST) ? %s->v.tuple.count : 0;\n",
                src_cnt, iter_var, iter_var);
        /* scratch array and output counter */
        char arr[32]; fresh_var(ctx, arr, sizeof(arr));
        char out_cnt[32]; fresh_var(ctx, out_cnt, sizeof(out_cnt));
        fprintf(f, "    sw_val_t **%s = malloc(sizeof(sw_val_t*) * (%s > 0 ? %s : 1));\n",
                arr, src_cnt, src_cnt);
        fprintf(f, "    int %s = 0;\n", out_cnt);
        /* loop variable */
        char idx[32]; fresh_var(ctx, idx, sizeof(idx));
        fprintf(f, "    for (int %s = 0; %s < %s; %s++) {\n", idx, idx, src_cnt, idx);
        /* Declare the loop variable fresh inside the for body.
         * Always emit `sw_val_t *var = ...` rather than relying on
         * is_declared — the variable is block-scoped to this for body.
         * Save and restore ndeclared so a second comprehension using the
         * same var name (e.g. two `[x * 2 for x in ...]`) also gets a
         * fresh `sw_val_t *x` declaration inside its own loop braces. */
        int saved_ndecl = ctx->ndeclared;
        fprintf(f, "    sw_val_t *%s = %s->v.tuple.items[%s];\n",
                n->v.lcomp.var, iter_var, idx);
        declare_var(ctx, n->v.lcomp.var);
        /* optional guard */
        if (n->v.lcomp.guard) {
            char gv[32];
            emit_expr(ctx, n->v.lcomp.guard, 0, gv, sizeof(gv));
            fprintf(f, "    if (!sw_val_is_truthy(%s)) continue;\n", gv);
        }
        /* evaluate body and store */
        char bv[32];
        emit_expr(ctx, n->v.lcomp.body, 0, bv, sizeof(bv));
        fprintf(f, "    %s[%s++] = %s;\n", arr, out_cnt, bv);
        fprintf(f, "    }\n");
        ctx->ndeclared = saved_ndecl; /* restore: loop var is block-scoped */
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_list(%s, %s);\n", v, arr, out_cnt);
        fprintf(f, "    free(%s);\n", arr);
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
         * error() must abort the REST of the try body and the dynamic
         * extent below it (an error() inside a callee lands here too),
         * exactly like the interpreter. The old shape ran the whole
         * body and tested _sw_error once at the end — so the statement
         * AFTER an error() still executed (and could panic, killing
         * the process before the catch was ever consulted).
         *
         * Now: push a _sw_try_frame_t (a C local — fiber-stack-
         * resident, so a mid-try yield + resume on another scheduler
         * is safe) onto the per-process chain and setjmp. error()
         * longjmps to the innermost frame. The frame is popped on BOTH
         * arms — at the top of the catch arm so an error() raised
         * INSIDE the catch body propagates to the enclosing try, not
         * back into this one (infinite loop otherwise).
         *
         * The result var is written after setjmp returns (either
         * entry), never read across the longjmp, so the setjmp
         * indeterminate-locals rule doesn't bite it.
         *
         * The catch block is its own lexical scope; snapshot ndeclared
         * around it (same pattern as emit_if) so a second try with the
         * same err_var re-declares rather than colliding. */
        char v[32]; fresh_var(ctx, v, sizeof(v));
        char fr[32]; fresh_var(ctx, fr, sizeof(fr));
        fprintf(f, "    sw_val_t *%s = sw_val_nil();\n", v);
        fprintf(f, "    _sw_try_frame_t _tf%s; _tf%s.prev = _sw_try_chain; _sw_try_chain = &_tf%s;\n",
                fr, fr, fr);
        fprintf(f, "    _sw_error = NULL;\n");
        fprintf(f, "    if (setjmp(_tf%s.jb) == 0) {\n", fr);
        char body_res[32];
        emit_expr(ctx, n->v.trycatch.body, 0, body_res, sizeof(body_res));
        fprintf(f, "        %s = %s;\n", v, body_res[0] ? body_res : "sw_val_nil()");
        fprintf(f, "        _sw_try_chain = _tf%s.prev;\n", fr);
        /* Belt-and-braces: a stale sentinel set by a chain-less error()
         * deeper in the call tree (no live frame at that moment) still
         * routes to catch here, preserving the old end-check behavior. */
        fprintf(f, "    }\n");
        fprintf(f, "    if (_sw_error) {\n");
        fprintf(f, "        _sw_try_chain = _tf%s.prev;\n", fr);
        int saved_ndeclared = ctx->ndeclared;
        fprintf(f, "        sw_val_t *%s = _sw_error;\n", mangle_for_c(n->v.trycatch.err_var));
        declare_var(ctx, n->v.trycatch.err_var);
        fprintf(f, "        _sw_error = NULL;\n");
        char catch_res[32];
        emit_expr(ctx, n->v.trycatch.catch_body, 0, catch_res, sizeof(catch_res));
        fprintf(f, "        %s = %s;\n", v, catch_res[0] ? catch_res : "sw_val_nil()");
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

/* Emit a function that initializes module-level globals.
 * Called once from _main_entry before the user's main(). */
static void emit_globals_init(cg_ctx_t *ctx, node_t *mod) {
    FILE *f = ctx->out;
    if (mod->v.mod.nglobals == 0) return;
    fprintf(f, "/* === Module globals initializer === */\n");
    fprintf(f, "static void _sw_init_globals_%s(void) {\n", ctx->mod_name);
    /* Use emit_expr to evaluate each global's RHS into its static slot.
     * We need a clean declared-variables context for the temporary vars. */
    int saved_ndeclared = ctx->ndeclared;
    ctx->ndeclared = 0;
    for (int i = 0; i < mod->v.mod.nglobals; i++) {
        char v[32];
        emit_expr(ctx, mod->v.mod.globals[i].val, 0, v, sizeof(v));
        fprintf(f, "    _g_%s_%s = %s;\n", ctx->mod_name,
                mod->v.mod.globals[i].name, v);
    }
    ctx->ndeclared = saved_ndeclared;
    fprintf(f, "}\n\n");
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

    /* Tail call label. Ownership v2: record the arena "floor" at entry — the
     * scoped turn-checkpoint (emit_call) reclaims only what THIS function
     * allocates above it, never the caller's live values below it. */
    if (ctx->has_tail) {
        fprintf(f, "    sw_varena_mark_t _gc_floor = sw_varena_mark(sw_self_varena());\n");
        fprintf(f, "    (void)_gc_floor;\n");
        fprintf(f, "_tail:;\n");
    }

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

/* `init_mods` lists EVERY module (entry + imported) that declared
 * top-level `let` globals, in the order their initializers must run
 * (imported modules before the entry module). _main_entry calls each
 * one's _sw_init_globals_<Mod>() before user main(). Previously only the
 * entry module's globals were initialized, so an imported module's `let X`
 * stayed NULL → SIGSEGV the moment the entry used MyMod.X. */
static void emit_entry_and_main(cg_ctx_t *ctx,
                                char init_mods[][128], int n_init_mods) {
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
    /* Initialize module globals for every module that has them — imported
     * modules first, entry module last (see init_mods ordering). */
    for (int i = 0; i < n_init_mods; i++)
        fprintf(f, "    _sw_init_globals_%s();\n", init_mods[i]);
    fprintf(f, "    %s_main(NULL, 0);\n", ctx->mod_name);
    fprintf(f, "    pthread_mutex_lock(&_sw_done_lock);\n");
    fprintf(f, "    _sw_done_flag = 1;\n");
    fprintf(f, "    pthread_cond_signal(&_sw_done_cond);\n");
    fprintf(f, "    pthread_mutex_unlock(&_sw_done_lock);\n");
    fprintf(f, "}\n\n");

    fprintf(f, "int main(int argc, char **argv) {\n");
    /* Hand argc/argv to the runtime so the os_args() builtin can read
     * them. Previously discarded — sw programs had no way to see CLI
     * arguments at all. */
    fprintf(f, "    sw_prog_argc = argc; sw_prog_argv = argv;\n");
    fprintf(f, "    setvbuf(stdout, NULL, _IONBF, 0);\n");
    /* Scheduler count: honor $SW_SCHEDULERS if set, else one OS thread
     * per online core (BEAM-style). Previously hardcoded to 2, which
     * silently underused every multicore host. */
    fprintf(f, "    {\n");
    fprintf(f, "        const char *_sw_env = getenv(\"SW_SCHEDULERS\");\n");
    fprintf(f, "        int _sw_nsched = _sw_env ? atoi(_sw_env) : (int)sysconf(_SC_NPROCESSORS_ONLN);\n");
    fprintf(f, "        if (_sw_nsched < 1) _sw_nsched = 1;\n");
    fprintf(f, "        sw_init(\"%s\", _sw_nsched);\n", ctx->mod_name);
    fprintf(f, "    }\n");
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

    /* Collect function names + arities for compile-time arity checks. */
    for (int i = 0; i < mod->v.mod.nfuns && i < CG_MAX_FUNCS; i++) {
        strncpy(ctx.func_names[i], mod->v.mod.funs[i]->v.fun.name, 127);
        ctx.func_nparams[i] = mod->v.mod.funs[i]->v.fun.nparams;
        ctx.nfuncs++;
    }

    /* Collect module-level global names so is_global() works inside functions. */
    for (int i = 0; i < mod->v.mod.nglobals && i < 16; i++) {
        strncpy(ctx.global_names[i], mod->v.mod.globals[i].name, 127);
        ctx.nglobals++;
    }
    ctx.has_globals = (ctx.nglobals > 0);

    /* Pre-scan for spawn sites and lambdas */
    scan_spawns(&ctx, mod);
    scan_lambdas(&ctx, mod);

    /* Emit everything */
    emit_preamble(&ctx);
    emit_forward_decls(&ctx, mod);
    emit_spawn_trampolines(&ctx);
    emit_lambda_functions(&ctx);
    emit_globals_init(&ctx, mod);

    fprintf(out, "/* === Functions === */\n");
    for (int i = 0; i < mod->v.mod.nfuns; i++)
        emit_function(&ctx, mod->v.mod.funs[i]);

    /* Single-module: only this module can have globals to init. */
    char single_init[1][128];
    int n_single_init = 0;
    if (ctx.has_globals) {
        strncpy(single_init[0], ctx.mod_name, 127);
        single_init[0][127] = '\0';
        n_single_init = 1;
    }
    emit_entry_and_main(&ctx, single_init, n_single_init);

    (void)obfuscate; /* handled externally by sw_obfuscate() */
    /* Fail the whole codegen if any call site had an arity mismatch
     * or unknown-function call. emit_call / did_you_mean already
     * reported per-site errors to stderr; this just makes swc return
     * non-zero so cc never runs on the bad C we still emitted (we keep
     * emitting so the walk surfaces ALL problems in one pass, not just
     * the first). */
    if (ctx.had_arity_error || ctx.had_unknown_fn) return -1;
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
        ctx.func_nparams[i] = mod->v.mod.funs[i]->v.fun.nparams;
        ctx.nfuncs++;
    }

    for (int i = 0; i < mod->v.mod.nglobals && i < 16; i++) {
        strncpy(ctx.global_names[i], mod->v.mod.globals[i].name, 127);
        ctx.nglobals++;
    }
    ctx.has_globals = (ctx.nglobals > 0);

    scan_spawns(&ctx, mod);
    scan_lambdas(&ctx, mod);

    /* Module-only: forward decls, trampolines, lambdas, functions — no preamble, no main */
    fprintf(out, "/* === Module: %s === */\n", ctx.mod_name);
    emit_forward_decls(&ctx, mod);
    emit_spawn_trampolines(&ctx);
    emit_lambda_functions(&ctx);
    emit_globals_init(&ctx, mod);

    for (int i = 0; i < mod->v.mod.nfuns; i++)
        emit_function(&ctx, mod->v.mod.funs[i]);

    if (ctx.had_arity_error || ctx.had_unknown_fn) return -1;
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
    int any_arity_error = 0;
    int any_unknown_fn = 0;
    for (int m = 0; m < nmodules; m++) {
        node_t *mod = (node_t *)modules[m];
        if (!mod || mod->type != N_MODULE) continue;

        cg_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.out = out;
        strncpy(ctx.mod_name, mod->v.mod.name, 127);

        for (int i = 0; i < mod->v.mod.nfuns && i < CG_MAX_FUNCS; i++) {
            strncpy(ctx.func_names[i], mod->v.mod.funs[i]->v.fun.name, 127);
            ctx.func_nparams[i] = mod->v.mod.funs[i]->v.fun.nparams;
            ctx.nfuncs++;
        }

        for (int i = 0; i < mod->v.mod.nglobals && i < 16; i++) {
            strncpy(ctx.global_names[i], mod->v.mod.globals[i].name, 127);
            ctx.nglobals++;
        }
        ctx.has_globals = (ctx.nglobals > 0);

        scan_spawns(&ctx, mod);
        scan_lambdas(&ctx, mod);

        fprintf(out, "\n/* === Module: %s === */\n", ctx.mod_name);
        emit_spawn_trampolines(&ctx);
        emit_lambda_functions(&ctx);
        emit_globals_init(&ctx, mod);

        for (int i = 0; i < mod->v.mod.nfuns; i++)
            emit_function(&ctx, mod->v.mod.funs[i]);

        if (ctx.had_arity_error) any_arity_error = 1;
        if (ctx.had_unknown_fn) any_unknown_fn = 1;
    }

    /* Entry point from main module.
     *
     * Collect EVERY module that declared top-level globals so _main_entry
     * initializes all of them — not just the entry module's. Order:
     * imported (non-entry) modules first (declaration order), entry module
     * last. That way an entry-module global RHS can reference an imported
     * module's already-initialized globals/functions. (Cross-module global
     * RHS that reaches back into a later-initialized module's globals is an
     * inherent ordering limitation, same as C static init across TUs.) */
    node_t *main_mod = (node_t *)modules[main_idx];
    char init_mods[64][128];
    int n_init_mods = 0;
    for (int m = 0; m < nmodules && n_init_mods < 64; m++) {
        if (m == main_idx) continue;
        node_t *mod = (node_t *)modules[m];
        if (!mod || mod->type != N_MODULE) continue;
        if (mod->v.mod.nglobals > 0) {
            strncpy(init_mods[n_init_mods], mod->v.mod.name, 127);
            init_mods[n_init_mods][127] = '\0';
            n_init_mods++;
        }
    }
    if (main_mod->v.mod.nglobals > 0 && n_init_mods < 64) {
        strncpy(init_mods[n_init_mods], main_mod->v.mod.name, 127);
        init_mods[n_init_mods][127] = '\0';
        n_init_mods++;
    }

    cg_ctx_t main_ctx;
    memset(&main_ctx, 0, sizeof(main_ctx));
    main_ctx.out = out;
    strncpy(main_ctx.mod_name, main_mod->v.mod.name, 127);
    main_ctx.has_globals = (main_mod->v.mod.nglobals > 0);
    emit_entry_and_main(&main_ctx, init_mods, n_init_mods);

    if (any_arity_error || any_unknown_fn) return -1;
    return 0;
}
