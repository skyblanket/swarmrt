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

    /* all function names in module */
    char func_names[64][128];
    int nfuncs;

    /* spawn sites collected during pre-scan */
    spawn_info_t spawns[64];
    int nspawns;
} cg_ctx_t;

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void fresh_var(cg_ctx_t *ctx, char *buf, int sz) {
    snprintf(buf, sz, "_t%d", ctx->tmp_counter++);
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
           strcmp(name, "elem") == 0 || strcmp(name, "abs") == 0;
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
    default: break;
    }
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
    fprintf(f, "#include <stdio.h>\n");
    fprintf(f, "#include <stdlib.h>\n");
    fprintf(f, "#include <string.h>\n");
    fprintf(f, "#include <unistd.h>\n\n");

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

    fprintf(f,
        "static sw_val_t *_op_div(sw_val_t *a, sw_val_t *b) {\n"
        "    if (a->type == SW_VAL_INT && b->type == SW_VAL_INT)\n"
        "        return b->v.i ? sw_val_int(a->v.i / b->v.i) : sw_val_nil();\n"
        "    double fa = a->type == SW_VAL_INT ? (double)a->v.i : a->v.f;\n"
        "    double fb = b->type == SW_VAL_INT ? (double)b->v.i : b->v.f;\n"
        "    return fb != 0.0 ? sw_val_float(fa / fb) : sw_val_nil();\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_op_concat(sw_val_t *a, sw_val_t *b) {\n"
        "    char buf[1024] = \"\";\n"
        "    if (a->type == SW_VAL_STRING) strncat(buf, a->v.str, sizeof(buf)-1);\n"
        "    else if (a->type == SW_VAL_INT) { char t[64]; snprintf(t,64,\"%%lld\",(long long)a->v.i); strncat(buf,t,sizeof(buf)-strlen(buf)-1); }\n"
        "    else if (a->type == SW_VAL_FLOAT) { char t[64]; snprintf(t,64,\"%%g\",a->v.f); strncat(buf,t,sizeof(buf)-strlen(buf)-1); }\n"
        "    else if (a->type == SW_VAL_ATOM) strncat(buf, a->v.str, sizeof(buf)-strlen(buf)-1);\n"
        "    if (b->type == SW_VAL_STRING) strncat(buf, b->v.str, sizeof(buf)-strlen(buf)-1);\n"
        "    else if (b->type == SW_VAL_INT) { char t[64]; snprintf(t,64,\"%%lld\",(long long)b->v.i); strncat(buf,t,sizeof(buf)-strlen(buf)-1); }\n"
        "    else if (b->type == SW_VAL_FLOAT) { char t[64]; snprintf(t,64,\"%%g\",b->v.f); strncat(buf,t,sizeof(buf)-strlen(buf)-1); }\n"
        "    else if (b->type == SW_VAL_ATOM) strncat(buf, b->v.str, sizeof(buf)-strlen(buf)-1);\n"
        "    return sw_val_string(buf);\n"
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
        "    return sw_val_atom(\"false\");\n"
        "}\n\n");

    /* Builtins */
    fprintf(f,
        "static sw_val_t *_builtin_print(sw_val_t **a, int n) {\n"
        "    for (int i = 0; i < n; i++) { if (i) printf(\" \"); sw_val_print(a[i]); }\n"
        "    printf(\"\\n\"); return sw_val_atom(\"ok\");\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_length(sw_val_t **a, int n) {\n"
        "    if (n < 1) return sw_val_int(0);\n"
        "    if (a[0]->type == SW_VAL_LIST || a[0]->type == SW_VAL_TUPLE)\n"
        "        return sw_val_int(a[0]->v.tuple.count);\n"
        "    if (a[0]->type == SW_VAL_STRING) return sw_val_int((int64_t)strlen(a[0]->v.str));\n"
        "    return sw_val_int(0);\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_hd(sw_val_t **a, int n) {\n"
        "    if (n < 1 || a[0]->type != SW_VAL_LIST || a[0]->v.tuple.count == 0)\n"
        "        return sw_val_nil();\n"
        "    return a[0]->v.tuple.items[0];\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_tl(sw_val_t **a, int n) {\n"
        "    if (n < 1 || a[0]->type != SW_VAL_LIST || a[0]->v.tuple.count <= 1)\n"
        "        return sw_val_list(NULL, 0);\n"
        "    return sw_val_list(a[0]->v.tuple.items + 1, a[0]->v.tuple.count - 1);\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_elem(sw_val_t **a, int n) {\n"
        "    if (n < 2 || a[0]->type != SW_VAL_TUPLE || a[1]->type != SW_VAL_INT)\n"
        "        return sw_val_nil();\n"
        "    int idx = (int)a[1]->v.i;\n"
        "    if (idx < 0 || idx >= a[0]->v.tuple.count) return sw_val_nil();\n"
        "    return a[0]->v.tuple.items[idx];\n"
        "}\n\n");

    fprintf(f,
        "static sw_val_t *_builtin_abs(sw_val_t **a, int n) {\n"
        "    if (n < 1 || a[0]->type != SW_VAL_INT) return sw_val_nil();\n"
        "    return sw_val_int(a[0]->v.i < 0 ? -a[0]->v.i : a[0]->v.i);\n"
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
        /* Struct for captured args */
        fprintf(f, "typedef struct {");
        for (int a = 0; a < sp->nargs; a++)
            fprintf(f, " sw_val_t *a%d;", a);
        fprintf(f, " } _sp%d_t;\n", sp->id);

        /* Entry function */
        fprintf(f, "static void _sp%d_entry(void *_raw) {\n", sp->id);
        fprintf(f, "    _sp%d_t *_s = (_sp%d_t *)_raw;\n", sp->id, sp->id);
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
 * Pattern matching
 * ========================================================================= */

static void emit_pattern_cond(cg_ctx_t *ctx, node_t *pat, const char *val) {
    FILE *f = ctx->out;
    (void)ctx;
    switch (pat->type) {
    case N_ATOM:
        fprintf(f, "%s->type == SW_VAL_ATOM && strcmp(%s->v.str, \"%s\") == 0",
                val, val, pat->v.sval);
        break;
    case N_INT:
        fprintf(f, "%s->type == SW_VAL_INT && %s->v.i == %lldLL",
                val, val, (long long)pat->v.ival);
        break;
    case N_FLOAT:
        fprintf(f, "%s->type == SW_VAL_FLOAT && %s->v.f == %g",
                val, val, pat->v.fval);
        break;
    case N_STRING:
        fprintf(f, "%s->type == SW_VAL_STRING && strcmp(%s->v.str, \"%s\") == 0",
                val, val, pat->v.sval);
        break;
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
        fprintf(f, "        sw_val_t *%s = %s;\n", pat->v.sval, val);
        declare_var(ctx, pat->v.sval);
        break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < pat->v.coll.count; i++) {
            char item[128];
            snprintf(item, sizeof(item), "%s->v.tuple.items[%d]", val, i);
            emit_pattern_bind(ctx, pat->v.coll.items[i], item);
        }
        break;
    default: break;
    }
}

/* =========================================================================
 * Expression emission
 * ========================================================================= */

/* Forward declaration */
static void emit_expr(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz);

static void emit_binop(cg_ctx_t *ctx, node_t *n, char *out, int osz) {
    FILE *f = ctx->out;
    char left[32], right[32], res[32];
    emit_expr(ctx, n->v.binop.left, 0, left, sizeof(left));
    emit_expr(ctx, n->v.binop.right, 0, right, sizeof(right));
    fresh_var(ctx, res, sizeof(res));

    const char *op = n->v.binop.op;
    if (strcmp(op, "+") == 0)
        fprintf(f, "    sw_val_t *%s = _op_add(%s, %s);\n", res, left, right);
    else if (strcmp(op, "-") == 0)
        fprintf(f, "    sw_val_t *%s = _op_sub(%s, %s);\n", res, left, right);
    else if (strcmp(op, "*") == 0)
        fprintf(f, "    sw_val_t *%s = _op_mul(%s, %s);\n", res, left, right);
    else if (strcmp(op, "/") == 0)
        fprintf(f, "    sw_val_t *%s = _op_div(%s, %s);\n", res, left, right);
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
    else if (is_module_func(ctx, fname)) {
        if (nargs > 0)
            fprintf(f, "    sw_val_t *%s = %s_%s(%s, %d);\n", res, ctx->mod_name, fname, arr, nargs);
        else
            fprintf(f, "    sw_val_t *%s = %s_%s(NULL, 0);\n", res, ctx->mod_name, fname);
    } else {
        /* Unknown function — emit as extern call */
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
    fprintf(f, "    sw_send_tagged(%s->v.pid, 11, %s);\n", to_var, msg_var);
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
    fprintf(f, "    _sp%d_t *%s = malloc(sizeof(_sp%d_t));\n", sp_id, sa, sp_id);

    /* Evaluate and assign spawn arguments */
    for (int i = 0; i < sp->nargs; i++) {
        char arg[32];
        emit_expr(ctx, inner->v.call.args[i], 0, arg, sizeof(arg));
        fprintf(f, "    %s->a%d = %s;\n", sa, i, arg);
    }

    char res[32];
    fresh_var(ctx, res, sizeof(res));
    fprintf(f, "    sw_val_t *%s = sw_val_pid(sw_spawn(_sp%d_entry, %s));\n",
            res, sp_id, sa);
    strncpy(out, res, osz - 1);
}

static void emit_receive(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz) {
    FILE *f = ctx->out;
    char tag[32], raw[32], msg[32], res[32];
    fresh_var(ctx, tag, sizeof(tag));
    fresh_var(ctx, raw, sizeof(raw));
    fresh_var(ctx, msg, sizeof(msg));
    fresh_var(ctx, res, sizeof(res));

    /* Result variable */
    fprintf(f, "    sw_val_t *%s = sw_val_nil();\n", res);

    /* Receive with timeout */
    uint64_t timeout = (n->v.recv.after_ms > 0)
        ? (uint64_t)n->v.recv.after_ms : (uint64_t)-1;
    fprintf(f, "    uint64_t %s;\n", tag);
    fprintf(f, "    void *%s = sw_receive_any(%lluULL, &%s);\n", raw, (unsigned long long)timeout, tag);
    fprintf(f, "    sw_val_t *%s = %s ? (sw_val_t *)%s : sw_val_nil();\n", msg, raw, raw);

    /* Pattern matching clauses */
    for (int i = 0; i < n->v.recv.nclauses; i++) {
        node_t *cl = n->v.recv.clauses[i];
        if (i == 0)
            fprintf(f, "    if (");
        else
            fprintf(f, "    } else if (");
        emit_pattern_cond(ctx, cl->v.clause.pattern, msg);
        fprintf(f, ") {\n");

        /* Save declared scope — pattern bindings are local to this clause */
        int saved_ndeclared = ctx->ndeclared;

        /* Bind pattern variables */
        emit_pattern_bind(ctx, cl->v.clause.pattern, msg);

        /* Emit clause body */
        char body_res[32];
        emit_expr(ctx, cl->v.clause.body, tail, body_res, sizeof(body_res));
        if (body_res[0])
            fprintf(f, "        %s = %s;\n", res, body_res);

        /* Restore scope */
        ctx->ndeclared = saved_ndeclared;
    }

    if (n->v.recv.nclauses > 0)
        fprintf(f, "    }\n");

    /* After clause (timeout handler) */
    if (n->v.recv.after_body) {
        fprintf(f, "    if (!%s) {\n", raw);
        char after_res[32];
        emit_expr(ctx, n->v.recv.after_body, 0, after_res, sizeof(after_res));
        if (after_res[0])
            fprintf(f, "        %s = %s;\n", res, after_res);
        fprintf(f, "    }\n");
    }

    strncpy(out, res, osz - 1);
}

static void emit_if(cg_ctx_t *ctx, node_t *n, int tail, char *out, int osz) {
    FILE *f = ctx->out;
    char cond[32], res[32];
    emit_expr(ctx, n->v.iff.cond, 0, cond, sizeof(cond));
    fresh_var(ctx, res, sizeof(res));
    fprintf(f, "    sw_val_t *%s = sw_val_nil();\n", res);
    fprintf(f, "    if (sw_val_is_truthy(%s)) {\n", cond);

    char then_res[32];
    emit_expr(ctx, n->v.iff.then_b, tail, then_res, sizeof(then_res));
    if (then_res[0])
        fprintf(f, "        %s = %s;\n", res, then_res);
    fprintf(f, "    }");

    if (n->v.iff.else_b) {
        fprintf(f, " else {\n");
        char else_res[32];
        emit_expr(ctx, n->v.iff.else_b, tail, else_res, sizeof(else_res));
        if (else_res[0])
            fprintf(f, "        %s = %s;\n", res, else_res);
        fprintf(f, "    }");
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
        fprintf(f, "    sw_val_t *%s = sw_val_float(%g);\n", v, n->v.fval);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_STRING: {
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_string(\"%s\");\n", v, n->v.sval);
        strncpy(out, v, osz - 1);
        break;
    }
    case N_ATOM: {
        char v[32]; fresh_var(ctx, v, sizeof(v));
        fprintf(f, "    sw_val_t *%s = sw_val_atom(\"%s\");\n", v, n->v.sval);
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
            /* Known variable */
            strncpy(out, n->v.sval, osz - 1);
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
        if (is_declared(ctx, n->v.assign.name)) {
            fprintf(f, "    %s = %s;\n", n->v.assign.name, val);
        } else {
            fprintf(f, "    sw_val_t *%s = %s;\n", n->v.assign.name, val);
            declare_var(ctx, n->v.assign.name);
        }
        strncpy(out, n->v.assign.name, osz - 1);
        break;
    }
    case N_BLOCK: {
        char last[32] = "";
        for (int i = 0; i < n->v.block.nstmts; i++) {
            int is_last = (i == n->v.block.nstmts - 1);
            emit_expr(ctx, n->v.block.stmts[i], is_last ? tail : 0, last, sizeof(last));
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
    case N_IF:
        emit_if(ctx, n, tail, out, osz);
        break;
    case N_PIPE:
        emit_pipe(ctx, n, tail, out, osz);
        break;
    default:
        break;
    }
}

/* =========================================================================
 * Function emission
 * ========================================================================= */

static void emit_function(cg_ctx_t *ctx, node_t *fn) {
    FILE *f = ctx->out;

    /* Set up function context */
    strncpy(ctx->cur_func, fn->v.fun.name, 127);
    ctx->cur_nparams = fn->v.fun.nparams;
    for (int i = 0; i < fn->v.fun.nparams; i++)
        strncpy(ctx->cur_params[i], fn->v.fun.params[i], 127);
    ctx->ndeclared = 0;
    ctx->has_tail = has_tail_calls(ctx, fn->v.fun.body);

    fprintf(f, "static sw_val_t *%s_%s(sw_val_t **_args, int _nargs) {\n",
            ctx->mod_name, fn->v.fun.name);

    /* Parameter extraction */
    if (fn->v.fun.nparams > 0) {
        for (int i = 0; i < fn->v.fun.nparams; i++) {
            fprintf(f, "    sw_val_t *%s = _nargs > %d ? _args[%d] : sw_val_nil();\n",
                    fn->v.fun.params[i], i, i);
            declare_var(ctx, fn->v.fun.params[i]);
        }
    } else {
        fprintf(f, "    (void)_args; (void)_nargs;\n");
    }

    /* Tail call label */
    if (ctx->has_tail)
        fprintf(f, "_tail:;\n");

    /* Body */
    char result[32];
    emit_expr(ctx, fn->v.fun.body, 1, result, sizeof(result));

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

    fprintf(f, "/* === Entry point === */\n");
    fprintf(f, "static void _main_entry(void *_arg) {\n");
    fprintf(f, "    (void)_arg;\n");
    fprintf(f, "    %s_main(NULL, 0);\n", ctx->mod_name);
    fprintf(f, "}\n\n");

    fprintf(f, "int main(int argc, char **argv) {\n");
    fprintf(f, "    (void)argc; (void)argv;\n");
    fprintf(f, "    sw_init(\"%s\", 4);\n", ctx->mod_name);
    fprintf(f, "    sw_spawn(_main_entry, NULL);\n");
    fprintf(f, "    usleep(2000000);\n");
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
    for (int i = 0; i < mod->v.mod.nfuns && i < 64; i++) {
        strncpy(ctx.func_names[i], mod->v.mod.funs[i]->v.fun.name, 127);
        ctx.nfuncs++;
    }

    /* Pre-scan for spawn sites */
    scan_spawns(&ctx, mod);

    /* Emit everything */
    emit_preamble(&ctx);
    emit_forward_decls(&ctx, mod);
    emit_spawn_trampolines(&ctx);

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
