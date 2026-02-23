/*
 * SwarmRT Phase 10: Language Frontend
 *
 * Tree-walking interpreter for .sw files. Contains a self-contained lexer,
 * parser, and evaluator. Maps spawn/send/receive to SwarmRT runtime calls.
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "swarmrt_lang.h"

/* Weak stubs for runtime functions — allows linking swc without the runtime.
 * These are only called by the interpreter, never by the compiler. */
__attribute__((weak)) void *sw_receive_any(uint64_t t, uint64_t *tag) { (void)t; (void)tag; return NULL; }
__attribute__((weak)) void sw_send_tagged(sw_process_t *to, uint64_t tag, void *msg) { (void)to; (void)tag; (void)msg; }
__attribute__((weak)) sw_process_t *sw_self(void) { return NULL; }

/* =========================================================================
 * Lexer
 * ========================================================================= */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_ATOM,

    /* Keywords */
    TOK_SPAWN,
    TOK_RECEIVE,
    TOK_SEND,
    TOK_AFTER,
    TOK_SWARM,
    TOK_MAP,
    TOK_REDUCE,
    TOK_SUPERVISE,
    TOK_MODULE,
    TOK_EXPORT,
    TOK_FUN,
    TOK_WHEN,
    TOK_CASE,
    TOK_END,
    TOK_IF,
    TOK_ELSE,
    TOK_SELF,

    /* Operators */
    TOK_ARROW,      /* -> */
    TOK_FARROW,     /* => */
    TOK_PIPE,       /* |> */
    TOK_ASSIGN,     /* = */
    TOK_EQ,         /* == */
    TOK_NEQ,        /* != */
    TOK_LT,         /* < */
    TOK_GT,         /* > */
    TOK_LE,         /* <= */
    TOK_GE,         /* >= */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_STAR,       /* * */
    TOK_SLASH,      /* / */
    TOK_CONCAT,     /* ++ */
    TOK_AND,        /* && */
    TOK_OR,         /* || */

    /* Delimiters */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_DOT,
    TOK_COLON,
    TOK_SEMI,
    TOK_NEWLINE,

    /* Phase 12 tokens */
    TOK_ELSIF,
    TOK_FOR,
    TOK_IN,
    TOK_TRY,
    TOK_CATCH,
    TOK_IMPORT,
    TOK_DOTDOT,    /* .. */
    TOK_BAR,       /* | (single pipe, list cons) */
    TOK_PERCENT,   /* % */
} tok_type_t;

typedef struct {
    tok_type_t type;
    char text[2048];
    int line;
    int col;
    double num_val;
} tok_t;

typedef struct {
    const char *src;
    int pos;
    int line;
    int col;
} lex_t;

static struct { const char *w; tok_type_t t; } kw_table[] = {
    {"spawn",     TOK_SPAWN},
    {"receive",   TOK_RECEIVE},
    {"send",      TOK_SEND},
    {"after",     TOK_AFTER},
    {"swarm",     TOK_SWARM},
    {"module",    TOK_MODULE},
    {"export",    TOK_EXPORT},
    {"fun",       TOK_FUN},
    {"when",      TOK_WHEN},
    {"case",      TOK_CASE},
    {"end",       TOK_END},
    {"if",        TOK_IF},
    {"else",      TOK_ELSE},
    {"self",      TOK_SELF},
    {"elsif",     TOK_ELSIF},
    {"for",       TOK_FOR},
    {"in",        TOK_IN},
    {"try",       TOK_TRY},
    {"catch",     TOK_CATCH},
    {"import",    TOK_IMPORT},
    {"true",      TOK_ATOM},
    {"false",     TOK_ATOM},
    {"nil",       TOK_ATOM},
    {NULL, 0}
};

static void lex_init(lex_t *l, const char *src) {
    l->src = src; l->pos = 0; l->line = 1; l->col = 1;
}

static char lpeek(lex_t *l) { return l->src[l->pos]; }
static char lpeek2(lex_t *l) { return l->src[l->pos] ? l->src[l->pos+1] : 0; }

static char ladv(lex_t *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; } else { l->col++; }
    return c;
}

static void lskip(lex_t *l) {
    while (1) {
        char c = lpeek(l);
        if (c == ' ' || c == '\t' || c == '\r') { ladv(l); }
        else if (c == '#') { while (lpeek(l) && lpeek(l) != '\n') ladv(l); }
        else break;
    }
}

static tok_t lnext(lex_t *l) {
    lskip(l);
    tok_t t; memset(&t, 0, sizeof(t));
    t.line = l->line; t.col = l->col;
    char c = lpeek(l);

    if (!c) { t.type = TOK_EOF; return t; }
    if (c == '\n') { ladv(l); return lnext(l); } /* skip newlines */

    /* Numbers */
    if (isdigit(c)) {
        int i = 0;
        while (isdigit(lpeek(l)) || (lpeek(l) == '.' && lpeek2(l) != '.'))
            t.text[i++] = ladv(l);
        t.text[i] = 0;
        t.type = TOK_NUMBER;
        t.num_val = atof(t.text);
        return t;
    }

    /* Strings */
    if (c == '"') {
        ladv(l); int i = 0;
        while (lpeek(l) && lpeek(l) != '"' && i < (int)sizeof(t.text) - 2) {
            if (lpeek(l) == '\\') {
                ladv(l);
                char esc = ladv(l);
                switch (esc) {
                    case 'n': t.text[i++] = '\n'; break;
                    case 't': t.text[i++] = '\t'; break;
                    case 'r': t.text[i++] = '\r'; break;
                    case '#': t.text[i++] = '\x01'; break; /* sentinel: escaped # */
                    default:  t.text[i++] = esc; break;
                }
            }
            else t.text[i++] = ladv(l);
        }
        t.text[i] = 0;
        if (lpeek(l) == '"') ladv(l);
        t.type = TOK_STRING;
        return t;
    }

    /* Atoms (quoted) */
    if (c == '\'') {
        ladv(l); int i = 0;
        while (lpeek(l) && lpeek(l) != '\'') t.text[i++] = ladv(l);
        t.text[i] = 0;
        if (lpeek(l) == '\'') ladv(l);
        t.type = TOK_ATOM;
        return t;
    }

    /* Identifiers / keywords */
    if (isalpha(c) || c == '_') {
        int i = 0;
        while (isalnum(lpeek(l)) || lpeek(l) == '_' || lpeek(l) == '?')
            t.text[i++] = ladv(l);
        t.text[i] = 0;
        for (int k = 0; kw_table[k].w; k++) {
            if (strcmp(t.text, kw_table[k].w) == 0) {
                t.type = kw_table[k].t;
                return t;
            }
        }
        t.type = TOK_IDENT;
        return t;
    }

    /* Multi-char operators */
    if (c == '-' && lpeek2(l) == '>') { ladv(l); ladv(l); t.type = TOK_ARROW; strcpy(t.text, "->"); return t; }
    if (c == '=' && lpeek2(l) == '>') { ladv(l); ladv(l); t.type = TOK_FARROW; strcpy(t.text, "=>"); return t; }
    if (c == '=' && lpeek2(l) == '=') { ladv(l); ladv(l); t.type = TOK_EQ; strcpy(t.text, "=="); return t; }
    if (c == '!' && lpeek2(l) == '=') { ladv(l); ladv(l); t.type = TOK_NEQ; strcpy(t.text, "!="); return t; }
    if (c == '<' && lpeek2(l) == '=') { ladv(l); ladv(l); t.type = TOK_LE; strcpy(t.text, "<="); return t; }
    if (c == '>' && lpeek2(l) == '=') { ladv(l); ladv(l); t.type = TOK_GE; strcpy(t.text, ">="); return t; }
    if (c == '|' && lpeek2(l) == '>') { ladv(l); ladv(l); t.type = TOK_PIPE; strcpy(t.text, "|>"); return t; }
    if (c == '+' && lpeek2(l) == '+') { ladv(l); ladv(l); t.type = TOK_CONCAT; strcpy(t.text, "++"); return t; }
    if (c == '&' && lpeek2(l) == '&') { ladv(l); ladv(l); t.type = TOK_AND; strcpy(t.text, "&&"); return t; }
    if (c == '|' && lpeek2(l) == '|') { ladv(l); ladv(l); t.type = TOK_OR; strcpy(t.text, "||"); return t; }
    if (c == '.' && lpeek2(l) == '.') { ladv(l); ladv(l); t.type = TOK_DOTDOT; strcpy(t.text, ".."); return t; }
    if (c == '%' && lpeek2(l) == '{') { ladv(l); /* consume % only, { will be next */ t.type = TOK_PERCENT; strcpy(t.text, "%"); return t; }

    /* Single-char */
    ladv(l);
    t.text[0] = c; t.text[1] = 0;
    switch (c) {
        case '=': t.type = TOK_ASSIGN; break;
        case '<': t.type = TOK_LT; break;
        case '>': t.type = TOK_GT; break;
        case '+': t.type = TOK_PLUS; break;
        case '-': t.type = TOK_MINUS; break;
        case '*': t.type = TOK_STAR; break;
        case '/': t.type = TOK_SLASH; break;
        case '(': t.type = TOK_LPAREN; break;
        case ')': t.type = TOK_RPAREN; break;
        case '{': t.type = TOK_LBRACE; break;
        case '}': t.type = TOK_RBRACE; break;
        case '[': t.type = TOK_LBRACKET; break;
        case ']': t.type = TOK_RBRACKET; break;
        case ',': t.type = TOK_COMMA; break;
        case '.': t.type = TOK_DOT; break;
        case ':': t.type = TOK_COLON; break;
        case ';': t.type = TOK_SEMI; break;
        case '|': t.type = TOK_BAR; break;
        case '%': t.type = TOK_PERCENT; break;
        default:  t.type = TOK_EOF; break;
    }
    return t;
}

/* =========================================================================
 * AST (types defined in swarmrt_lang.h)
 * ========================================================================= */

static node_t *mknode(node_type_t type, int line) {
    node_t *n = calloc(1, sizeof(node_t));
    n->type = type; n->line = line;
    return n;
}

static void node_free(node_t *n) {
    if (!n) return;
    switch (n->type) {
    case N_MODULE:
        for (int i = 0; i < n->v.mod.nfuns; i++) node_free(n->v.mod.funs[i]);
        free(n->v.mod.funs); break;
    case N_FUN:
        node_free(n->v.fun.body);
        for (int i = 0; i < n->v.fun.nparams; i++) node_free(n->v.fun.defaults[i]);
        break;
    case N_BLOCK:
        for (int i = 0; i < n->v.block.nstmts; i++) node_free(n->v.block.stmts[i]);
        free(n->v.block.stmts); break;
    case N_ASSIGN: node_free(n->v.assign.value); break;
    case N_CALL:
        node_free(n->v.call.func);
        for (int i = 0; i < n->v.call.nargs; i++) node_free(n->v.call.args[i]);
        free(n->v.call.args); break;
    case N_SPAWN: node_free(n->v.spawn.expr); break;
    case N_SEND: node_free(n->v.send.to); node_free(n->v.send.msg); break;
    case N_RECEIVE:
        for (int i = 0; i < n->v.recv.nclauses; i++) node_free(n->v.recv.clauses[i]);
        free(n->v.recv.clauses);
        node_free(n->v.recv.after_body); break;
    case N_CLAUSE:
        node_free(n->v.clause.pattern);
        node_free(n->v.clause.guard);
        node_free(n->v.clause.body);
        break;
    case N_IF: node_free(n->v.iff.cond); node_free(n->v.iff.then_b); node_free(n->v.iff.else_b); break;
    case N_BINOP: node_free(n->v.binop.left); node_free(n->v.binop.right); break;
    case N_UNARY: node_free(n->v.unary.operand); break;
    case N_PIPE: node_free(n->v.pipe.val); node_free(n->v.pipe.func); break;
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < n->v.coll.count; i++) node_free(n->v.coll.items[i]);
        free(n->v.coll.items); break;
    case N_MAP:
        for (int i = 0; i < n->v.map.count; i++) { node_free(n->v.map.keys[i]); node_free(n->v.map.vals[i]); }
        free(n->v.map.keys); free(n->v.map.vals); break;
    case N_FOR: node_free(n->v.forloop.iter); node_free(n->v.forloop.body); break;
    case N_RANGE: node_free(n->v.range.from); node_free(n->v.range.to); break;
    case N_TRY: node_free(n->v.trycatch.body); node_free(n->v.trycatch.catch_body); break;
    case N_LIST_CONS: node_free(n->v.cons.head); node_free(n->v.cons.tail); break;
    default: break;
    }
    free(n);
}

/* =========================================================================
 * Parser
 * ========================================================================= */

typedef struct {
    lex_t lex;
    tok_t cur;
    int err;
    char errmsg[256];
} par_t;

static void par_init(par_t *p, const char *src) {
    lex_init(&p->lex, src);
    p->cur = lnext(&p->lex);
    p->err = 0; p->errmsg[0] = 0;
}

static tok_t par_adv(par_t *p) {
    tok_t prev = p->cur;
    p->cur = lnext(&p->lex);
    return prev;
}

static int par_match(par_t *p, tok_type_t t) {
    if (p->cur.type == t) { par_adv(p); return 1; } return 0;
}

static tok_t par_expect(par_t *p, tok_type_t t, const char *msg) {
    if (p->cur.type == t) return par_adv(p);
    snprintf(p->errmsg, sizeof(p->errmsg), "line %d: expected %s, got '%s'",
             p->cur.line, msg, p->cur.text);
    p->err = 1;
    return p->cur;
}

/* Forward declarations */
static node_t *par_expr(par_t *p);
static node_t *par_stmt(par_t *p);
static node_t *par_block(par_t *p);

/* Restore \x01 sentinels back to # */
static void restore_hash_sentinels(char *s) {
    for (; *s; s++) if (*s == '\x01') *s = '#';
}

/* String interpolation: "hello #{name}!" -> concat chain */
static node_t *parse_interp_string(const char *text, int line) {
    if (!strstr(text, "#{")) return NULL;
    node_t *result = NULL;
    const char *pos = text;
    while (*pos) {
        const char *interp = strstr(pos, "#{");
        if (!interp) {
            if (*pos) {
                node_t *s = mknode(N_STRING, line);
                strncpy(s->v.sval, pos, sizeof(s->v.sval)-1);
                if (result) {
                    node_t *cat = mknode(N_BINOP, line);
                    cat->v.binop.left = result; cat->v.binop.right = s;
                    strcpy(cat->v.binop.op, "++"); result = cat;
                } else result = s;
            }
            break;
        }
        if (interp > pos) {
            node_t *s = mknode(N_STRING, line);
            int len = (int)(interp - pos);
            if (len >= (int)sizeof(s->v.sval)) len = (int)sizeof(s->v.sval) - 1;
            memcpy(s->v.sval, pos, len); s->v.sval[len] = 0;
            restore_hash_sentinels(s->v.sval);
            if (result) {
                node_t *cat = mknode(N_BINOP, line);
                cat->v.binop.left = result; cat->v.binop.right = s;
                strcpy(cat->v.binop.op, "++"); result = cat;
            } else result = s;
        }
        interp += 2;
        const char *end = interp;
        int depth = 1;
        while (*end && depth > 0) {
            if (*end == '{') depth++;
            else if (*end == '}') depth--;
            if (depth > 0) end++;
        }
        char expr_text[2048];
        int elen = (int)(end - interp);
        if (elen >= (int)sizeof(expr_text)) elen = (int)sizeof(expr_text) - 1;
        memcpy(expr_text, interp, elen); expr_text[elen] = 0;
        par_t ep; par_init(&ep, expr_text);
        node_t *expr = par_expr(&ep);
        node_t *call = mknode(N_CALL, line);
        node_t *fn_node = mknode(N_IDENT, line);
        strcpy(fn_node->v.sval, "to_string");
        call->v.call.func = fn_node;
        call->v.call.args = malloc(sizeof(node_t*));
        call->v.call.args[0] = expr; call->v.call.nargs = 1;
        if (result) {
            node_t *cat = mknode(N_BINOP, line);
            cat->v.binop.left = result; cat->v.binop.right = call;
            strcpy(cat->v.binop.op, "++"); result = cat;
        } else result = call;
        pos = *end ? end + 1 : end;
    }
    return result ? result : mknode(N_STRING, line);
}

/* Primary expression */
static node_t *par_primary(par_t *p) {
    tok_t t = p->cur;

    if (t.type == TOK_NUMBER) {
        par_adv(p);
        /* Integer or float? */
        if (strchr(t.text, '.')) {
            node_t *n = mknode(N_FLOAT, t.line);
            n->v.fval = t.num_val;
            return n;
        }
        node_t *n = mknode(N_INT, t.line);
        n->v.ival = (int64_t)t.num_val;
        return n;
    }

    if (t.type == TOK_STRING) {
        par_adv(p);
        node_t *interp = parse_interp_string(t.text, t.line);
        if (interp) return interp;
        node_t *n = mknode(N_STRING, t.line);
        strncpy(n->v.sval, t.text, sizeof(n->v.sval)-1);
        restore_hash_sentinels(n->v.sval);
        return n;
    }

    if (t.type == TOK_ATOM) {
        par_adv(p);
        node_t *n = mknode(N_ATOM, t.line);
        strncpy(n->v.sval, t.text, sizeof(n->v.sval)-1);
        return n;
    }

    if (t.type == TOK_SELF) {
        par_adv(p);
        /* self() */
        if (par_match(p, TOK_LPAREN)) par_expect(p, TOK_RPAREN, "')'");
        return mknode(N_SELF, t.line);
    }

    if (t.type == TOK_IDENT) {
        par_adv(p);
        if (p->cur.type == TOK_DOT) {
            par_adv(p); /* consume . */
            tok_t fname = par_expect(p, TOK_IDENT, "field/function name");
            if (p->cur.type == TOK_LPAREN) {
                /* Module-qualified call: Module.function(args) */
                char qualified[256];
                snprintf(qualified, sizeof(qualified), "%s.%s", t.text, fname.text);
                par_adv(p); /* ( */
                node_t *call = mknode(N_CALL, t.line);
                node_t *fn_node = mknode(N_IDENT, t.line);
                strncpy(fn_node->v.sval, qualified, sizeof(fn_node->v.sval)-1);
                call->v.call.func = fn_node;
                call->v.call.args = NULL; call->v.call.nargs = 0;
                if (p->cur.type != TOK_RPAREN) {
                    do {
                        call->v.call.nargs++;
                        call->v.call.args = realloc(call->v.call.args, sizeof(node_t*) * call->v.call.nargs);
                        call->v.call.args[call->v.call.nargs-1] = par_expr(p);
                    } while (par_match(p, TOK_COMMA));
                }
                par_expect(p, TOK_RPAREN, "')'");
                return call;
            } else {
                /* Dot access: obj.field -> map_get(obj, 'field') */
                node_t *obj = mknode(N_IDENT, t.line);
                strncpy(obj->v.sval, t.text, sizeof(obj->v.sval)-1);
                node_t *key = mknode(N_ATOM, fname.line);
                strncpy(key->v.sval, fname.text, sizeof(key->v.sval)-1);
                node_t *call = mknode(N_CALL, t.line);
                node_t *fn_node = mknode(N_IDENT, t.line);
                strcpy(fn_node->v.sval, "map_get");
                call->v.call.func = fn_node;
                call->v.call.args = malloc(sizeof(node_t*) * 2);
                call->v.call.args[0] = obj; call->v.call.args[1] = key;
                call->v.call.nargs = 2;
                return call;
            }
        }
        /* Function call: ident( args ) */
        if (p->cur.type == TOK_LPAREN) {
            par_adv(p); /* ( */
            node_t *call = mknode(N_CALL, t.line);
            node_t *fn_node = mknode(N_IDENT, t.line);
            strncpy(fn_node->v.sval, t.text, sizeof(fn_node->v.sval)-1);
            call->v.call.func = fn_node;
            call->v.call.args = NULL; call->v.call.nargs = 0;
            if (p->cur.type != TOK_RPAREN) {
                do {
                    call->v.call.nargs++;
                    call->v.call.args = realloc(call->v.call.args, sizeof(node_t*) * call->v.call.nargs);
                    call->v.call.args[call->v.call.nargs-1] = par_expr(p);
                } while (par_match(p, TOK_COMMA));
            }
            par_expect(p, TOK_RPAREN, "')'");
            return call;
        }
        node_t *n = mknode(N_IDENT, t.line);
        strncpy(n->v.sval, t.text, sizeof(n->v.sval)-1);
        return n;
    }

    /* Anonymous function: fun(params) { body } */
    if (t.type == TOK_FUN) {
        par_adv(p);
        if (p->cur.type == TOK_LPAREN) {
            par_adv(p); /* ( */
            node_t *fn = mknode(N_FUN, t.line);
            fn->v.fun.name[0] = '\0'; /* empty name = anonymous */
            fn->v.fun.nparams = 0;
            if (p->cur.type != TOK_RPAREN) {
                do {
                    tok_t param = par_expect(p, TOK_IDENT, "parameter");
                    strncpy(fn->v.fun.params[fn->v.fun.nparams], param.text, 127);
                    fn->v.fun.nparams++;
                } while (par_match(p, TOK_COMMA));
            }
            par_expect(p, TOK_RPAREN, "')'");
            par_expect(p, TOK_LBRACE, "'{'");
            fn->v.fun.body = par_block(p);
            par_expect(p, TOK_RBRACE, "'}'");
            return fn;
        }
        /* Not anonymous — error in expression context */
        p->err = 1;
        snprintf(p->errmsg, sizeof(p->errmsg),
                 "line %d: named functions not allowed in expressions", t.line);
        return mknode(N_INT, t.line);
    }

    /* For loop: for x in expr { body } */
    if (t.type == TOK_FOR) {
        par_adv(p);
        tok_t var = par_expect(p, TOK_IDENT, "variable");
        par_expect(p, TOK_IN, "'in'");
        node_t *iter = par_expr(p);
        par_expect(p, TOK_LBRACE, "'{'");
        node_t *body = par_block(p);
        par_expect(p, TOK_RBRACE, "'}'");
        node_t *f = mknode(N_FOR, t.line);
        strncpy(f->v.forloop.var, var.text, 127);
        f->v.forloop.iter = iter;
        f->v.forloop.body = body;
        return f;
    }

    /* Try/catch: try { body } catch e { handler } */
    if (t.type == TOK_TRY) {
        par_adv(p);
        par_expect(p, TOK_LBRACE, "'{'");
        node_t *try_body = par_block(p);
        par_expect(p, TOK_RBRACE, "'}'");
        par_expect(p, TOK_CATCH, "'catch'");
        tok_t err_var = par_expect(p, TOK_IDENT, "error variable");
        par_expect(p, TOK_LBRACE, "'{'");
        node_t *catch_body = par_block(p);
        par_expect(p, TOK_RBRACE, "'}'");
        node_t *tc = mknode(N_TRY, t.line);
        tc->v.trycatch.body = try_body;
        strncpy(tc->v.trycatch.err_var, err_var.text, 127);
        tc->v.trycatch.catch_body = catch_body;
        return tc;
    }

    /* Spawn expression */
    if (t.type == TOK_SPAWN) {
        par_adv(p);
        par_expect(p, TOK_LPAREN, "'('");
        node_t *sp = mknode(N_SPAWN, t.line);
        sp->v.spawn.expr = par_expr(p);
        par_expect(p, TOK_RPAREN, "')'");
        return sp;
    }

    /* Send expression */
    if (t.type == TOK_SEND) {
        par_adv(p);
        par_expect(p, TOK_LPAREN, "'('");
        node_t *sn = mknode(N_SEND, t.line);
        sn->v.send.to = par_expr(p);
        par_expect(p, TOK_COMMA, "','");
        sn->v.send.msg = par_expr(p);
        par_expect(p, TOK_RPAREN, "')'");
        return sn;
    }

    /* Receive block */
    if (t.type == TOK_RECEIVE) {
        par_adv(p);
        par_expect(p, TOK_LBRACE, "'{'");
        node_t *recv = mknode(N_RECEIVE, t.line);
        recv->v.recv.clauses = NULL;
        recv->v.recv.nclauses = 0;
        recv->v.recv.after_body = NULL;
        recv->v.recv.after_ms = -1;  /* sentinel: no after clause */

        while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_AFTER && p->cur.type != TOK_EOF) {
            node_t *cl = mknode(N_CLAUSE, p->cur.line);
            cl->v.clause.pattern = par_expr(p);
            cl->v.clause.guard = NULL;
            if (p->cur.type == TOK_WHEN) {
                par_adv(p);
                cl->v.clause.guard = par_expr(p);
            }
            par_expect(p, TOK_ARROW, "'->'");
            /* Body: collect expressions until next clause pattern (expr ->) or } */
            node_t *body = mknode(N_BLOCK, p->cur.line);
            body->v.block.stmts = NULL;
            body->v.block.nstmts = 0;
            while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_AFTER && p->cur.type != TOK_EOF && !p->err) {
                lex_t save_lex = p->lex;
                tok_t save_cur = p->cur;
                int save_err = p->err;
                node_t *expr = par_stmt(p);
                if (p->cur.type == TOK_ARROW) {
                    /* This is the next clause's pattern — backtrack */
                    p->lex = save_lex;
                    p->cur = save_cur;
                    p->err = save_err;
                    node_free(expr);
                    break;
                }
                body->v.block.nstmts++;
                body->v.block.stmts = realloc(body->v.block.stmts,
                    sizeof(node_t*) * body->v.block.nstmts);
                body->v.block.stmts[body->v.block.nstmts-1] = expr;
            }
            if (body->v.block.nstmts == 1) {
                cl->v.clause.body = body->v.block.stmts[0];
                free(body->v.block.stmts);
                free(body);
            } else {
                cl->v.clause.body = body;
            }
            recv->v.recv.nclauses++;
            recv->v.recv.clauses = realloc(recv->v.recv.clauses,
                sizeof(node_t*) * recv->v.recv.nclauses);
            recv->v.recv.clauses[recv->v.recv.nclauses-1] = cl;
        }

        if (par_match(p, TOK_AFTER)) {
            node_t *timeout_expr = par_expr(p);
            if (timeout_expr && timeout_expr->type == N_INT) {
                recv->v.recv.after_ms = (int)timeout_expr->v.ival;
            }
            node_free(timeout_expr);
            par_expect(p, TOK_LBRACE, "'{'");
            recv->v.recv.after_body = par_expr(p);
            par_expect(p, TOK_RBRACE, "'}'");
        }

        par_expect(p, TOK_RBRACE, "'}'");
        return recv;
    }

    /* If expression */
    if (t.type == TOK_IF) {
        par_adv(p);
        node_t *iff = mknode(N_IF, t.line);
        par_expect(p, TOK_LPAREN, "'('");
        iff->v.iff.cond = par_expr(p);
        par_expect(p, TOK_RPAREN, "')'");
        par_expect(p, TOK_LBRACE, "'{'");
        iff->v.iff.then_b = par_block(p);
        par_expect(p, TOK_RBRACE, "'}'");
        if (par_match(p, TOK_ELSE)) {
            if (p->cur.type == TOK_IF) {
                iff->v.iff.else_b = par_primary(p);
            } else {
                par_expect(p, TOK_LBRACE, "'{'");
                iff->v.iff.else_b = par_block(p);
                par_expect(p, TOK_RBRACE, "'}'");
            }
        } else if (p->cur.type == TOK_ELSIF) {
            p->cur.type = TOK_IF;
            iff->v.iff.else_b = par_primary(p);
        }
        return iff;
    }

    /* Map literal: %{key: val, ...} */
    if (t.type == TOK_PERCENT) {
        par_adv(p);
        par_expect(p, TOK_LBRACE, "'{'");
        node_t *m = mknode(N_MAP, t.line);
        m->v.map.keys = NULL; m->v.map.vals = NULL; m->v.map.count = 0;
        if (p->cur.type != TOK_RBRACE) {
            do {
                node_t *key;
                if (p->cur.type == TOK_IDENT) {
                    /* Check for shorthand: name: val */
                    lex_t sl = p->lex; tok_t sc = p->cur;
                    tok_t k = par_adv(p);
                    if (p->cur.type == TOK_COLON) {
                        par_adv(p); /* consume : */
                        key = mknode(N_ATOM, k.line);
                        strncpy(key->v.sval, k.text, sizeof(key->v.sval)-1);
                    } else {
                        p->lex = sl; p->cur = sc;
                        key = par_expr(p);
                        par_expect(p, TOK_FARROW, "'=>'");
                    }
                } else if (p->cur.type == TOK_STRING || p->cur.type == TOK_ATOM) {
                    key = par_primary(p);
                    par_expect(p, TOK_FARROW, "'=>'");
                } else {
                    key = par_expr(p);
                    par_expect(p, TOK_FARROW, "'=>'");
                }
                node_t *val = par_expr(p);
                m->v.map.count++;
                m->v.map.keys = realloc(m->v.map.keys, sizeof(node_t*) * m->v.map.count);
                m->v.map.vals = realloc(m->v.map.vals, sizeof(node_t*) * m->v.map.count);
                m->v.map.keys[m->v.map.count-1] = key;
                m->v.map.vals[m->v.map.count-1] = val;
            } while (par_match(p, TOK_COMMA));
        }
        par_expect(p, TOK_RBRACE, "'}'");
        return m;
    }

    /* Tuple */
    if (t.type == TOK_LBRACE) {
        par_adv(p);
        node_t *tup = mknode(N_TUPLE, t.line);
        tup->v.coll.items = NULL; tup->v.coll.count = 0;
        if (p->cur.type != TOK_RBRACE) {
            do {
                tup->v.coll.count++;
                tup->v.coll.items = realloc(tup->v.coll.items,
                    sizeof(node_t*) * tup->v.coll.count);
                tup->v.coll.items[tup->v.coll.count-1] = par_expr(p);
            } while (par_match(p, TOK_COMMA));
        }
        par_expect(p, TOK_RBRACE, "'}'");
        return tup;
    }

    /* List */
    if (t.type == TOK_LBRACKET) {
        par_adv(p);
        if (p->cur.type == TOK_RBRACKET) {
            par_adv(p);
            node_t *lst = mknode(N_LIST, t.line);
            lst->v.coll.items = NULL; lst->v.coll.count = 0;
            return lst;
        }
        node_t *first = par_expr(p);
        if (p->cur.type == TOK_BAR) {
            /* List cons: [h | t] */
            par_adv(p);
            node_t *tail = par_expr(p);
            par_expect(p, TOK_RBRACKET, "']'");
            node_t *cons = mknode(N_LIST_CONS, t.line);
            cons->v.cons.head = first;
            cons->v.cons.tail = tail;
            return cons;
        }
        node_t *lst = mknode(N_LIST, t.line);
        lst->v.coll.items = malloc(sizeof(node_t*));
        lst->v.coll.items[0] = first;
        lst->v.coll.count = 1;
        while (par_match(p, TOK_COMMA)) {
            lst->v.coll.count++;
            lst->v.coll.items = realloc(lst->v.coll.items, sizeof(node_t*) * lst->v.coll.count);
            lst->v.coll.items[lst->v.coll.count-1] = par_expr(p);
        }
        par_expect(p, TOK_RBRACKET, "']'");
        return lst;
    }

    /* Grouped expression */
    if (t.type == TOK_LPAREN) {
        par_adv(p);
        node_t *e = par_expr(p);
        par_expect(p, TOK_RPAREN, "')'");
        return e;
    }

    /* Unary minus */
    if (t.type == TOK_MINUS) {
        par_adv(p);
        node_t *u = mknode(N_UNARY, t.line);
        u->v.unary.op = '-';
        u->v.unary.operand = par_primary(p);
        return u;
    }

    /* Unknown */
    snprintf(p->errmsg, sizeof(p->errmsg), "line %d: unexpected '%s'", t.line, t.text);
    p->err = 1;
    par_adv(p);
    return mknode(N_ATOM, t.line); /* dummy */
}

/* Multiplicative */
static node_t *par_mul(par_t *p) {
    node_t *left = par_primary(p);
    while (p->cur.type == TOK_STAR || p->cur.type == TOK_SLASH) {
        tok_t op = par_adv(p);
        node_t *n = mknode(N_BINOP, op.line);
        n->v.binop.left = left;
        n->v.binop.right = par_primary(p);
        strncpy(n->v.binop.op, op.text, 3);
        left = n;
    }
    return left;
}

/* Additive */
static node_t *par_add(par_t *p) {
    node_t *left = par_mul(p);
    while (p->cur.type == TOK_PLUS || p->cur.type == TOK_MINUS || p->cur.type == TOK_CONCAT) {
        tok_t op = par_adv(p);
        node_t *n = mknode(N_BINOP, op.line);
        n->v.binop.left = left;
        n->v.binop.right = par_mul(p);
        strncpy(n->v.binop.op, op.text, 3);
        left = n;
    }
    return left;
}

/* Range */
static node_t *par_range(par_t *p) {
    node_t *left = par_add(p);
    if (p->cur.type == TOK_DOTDOT) {
        par_adv(p);
        node_t *r = mknode(N_RANGE, left->line);
        r->v.range.from = left;
        r->v.range.to = par_add(p);
        return r;
    }
    return left;
}

/* Comparison */
static node_t *par_cmp(par_t *p) {
    node_t *left = par_range(p);
    while (p->cur.type == TOK_EQ || p->cur.type == TOK_NEQ ||
           p->cur.type == TOK_LT || p->cur.type == TOK_GT ||
           p->cur.type == TOK_LE || p->cur.type == TOK_GE) {
        tok_t op = par_adv(p);
        node_t *n = mknode(N_BINOP, op.line);
        n->v.binop.left = left;
        n->v.binop.right = par_range(p);
        strncpy(n->v.binop.op, op.text, 3);
        left = n;
    }
    return left;
}

/* Logical */
static node_t *par_logic(par_t *p) {
    node_t *left = par_cmp(p);
    while (p->cur.type == TOK_AND || p->cur.type == TOK_OR) {
        tok_t op = par_adv(p);
        node_t *n = mknode(N_BINOP, op.line);
        n->v.binop.left = left;
        n->v.binop.right = par_cmp(p);
        strncpy(n->v.binop.op, op.text, 3);
        left = n;
    }
    return left;
}

/* Pipe */
static node_t *par_pipe(par_t *p) {
    node_t *left = par_logic(p);
    while (p->cur.type == TOK_PIPE) {
        par_adv(p);
        node_t *n = mknode(N_PIPE, p->cur.line);
        n->v.pipe.val = left;
        n->v.pipe.func = par_logic(p);
        left = n;
    }
    return left;
}

/* Full expression */
static node_t *par_expr(par_t *p) {
    return par_pipe(p);
}

/* Statement: assignment or expression */
static node_t *par_stmt(par_t *p) {
    if (p->cur.type == TOK_IDENT) {
        /* Peek: is next token '=' (but not '==')? */
        tok_t id = p->cur;
        /* Save lexer state for backtrack */
        lex_t save_lex = p->lex;
        tok_t save_cur = p->cur;

        par_adv(p);
        if (p->cur.type == TOK_ASSIGN) {
            par_adv(p); /* consume = */
            node_t *a = mknode(N_ASSIGN, id.line);
            strncpy(a->v.assign.name, id.text, sizeof(a->v.assign.name)-1);
            a->v.assign.value = par_expr(p);
            return a;
        }
        /* Backtrack */
        p->lex = save_lex;
        p->cur = save_cur;
    }
    return par_expr(p);
}

/* Block: list of statements */
static node_t *par_block(par_t *p) {
    node_t *block = mknode(N_BLOCK, p->cur.line);
    block->v.block.stmts = NULL;
    block->v.block.nstmts = 0;

    while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !p->err) {
        block->v.block.nstmts++;
        block->v.block.stmts = realloc(block->v.block.stmts,
            sizeof(node_t*) * block->v.block.nstmts);
        block->v.block.stmts[block->v.block.nstmts-1] = par_stmt(p);
    }
    return block;
}

/* Function definition */
static node_t *par_fun(par_t *p) {
    par_expect(p, TOK_FUN, "'fun'");
    tok_t name = par_expect(p, TOK_IDENT, "function name");
    par_expect(p, TOK_LPAREN, "'('");

    node_t *fn = mknode(N_FUN, name.line);
    strncpy(fn->v.fun.name, name.text, sizeof(fn->v.fun.name)-1);
    fn->v.fun.nparams = 0;

    if (p->cur.type != TOK_RPAREN) {
        do {
            tok_t param = par_expect(p, TOK_IDENT, "parameter");
            strncpy(fn->v.fun.params[fn->v.fun.nparams], param.text, 127);
            fn->v.fun.defaults[fn->v.fun.nparams] = NULL;
            if (par_match(p, TOK_ASSIGN)) {
                fn->v.fun.defaults[fn->v.fun.nparams] = par_expr(p);
            }
            fn->v.fun.nparams++;
        } while (par_match(p, TOK_COMMA));
    }
    par_expect(p, TOK_RPAREN, "')'");
    par_expect(p, TOK_LBRACE, "'{'");
    fn->v.fun.body = par_block(p);
    par_expect(p, TOK_RBRACE, "'}'");
    return fn;
}

/* Module */
static node_t *par_module(par_t *p) {
    par_expect(p, TOK_MODULE, "'module'");
    tok_t name = par_expect(p, TOK_IDENT, "module name");

    node_t *mod = mknode(N_MODULE, name.line);
    strncpy(mod->v.mod.name, name.text, sizeof(mod->v.mod.name)-1);
    mod->v.mod.funs = NULL;
    mod->v.mod.nfuns = 0;
    mod->v.mod.nimports = 0;

    /* Imports */
    while (p->cur.type == TOK_IMPORT && !p->err) {
        par_adv(p);
        tok_t iname = par_expect(p, TOK_IDENT, "module name");
        if (mod->v.mod.nimports < 16)
            strncpy(mod->v.mod.imports[mod->v.mod.nimports++], iname.text, 127);
    }

    /* Optional export */
    if (par_match(p, TOK_EXPORT)) {
        par_expect(p, TOK_LBRACKET, "'['");
        while (p->cur.type != TOK_RBRACKET && p->cur.type != TOK_EOF) {
            par_adv(p);
            par_match(p, TOK_COMMA);
        }
        par_expect(p, TOK_RBRACKET, "']'");
    }

    /* Functions */
    while (p->cur.type == TOK_FUN && !p->err) {
        mod->v.mod.nfuns++;
        mod->v.mod.funs = realloc(mod->v.mod.funs, sizeof(node_t*) * mod->v.mod.nfuns);
        mod->v.mod.funs[mod->v.mod.nfuns-1] = par_fun(p);
    }

    return mod;
}

/* Top-level parse for expression string (no module wrapper) */
static node_t *par_expr_string(const char *src) {
    par_t p;
    par_init(&p, src);
    node_t *e = par_expr(&p);
    if (p.err) { node_free(e); return NULL; }
    return e;
}

/* =========================================================================
 * Values
 * ========================================================================= */

sw_val_t *sw_val_nil(void) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_NIL;
    return v;
}

sw_val_t *sw_val_int(int64_t i) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_INT; v->v.i = i;
    return v;
}

sw_val_t *sw_val_float(double f) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_FLOAT; v->v.f = f;
    return v;
}

sw_val_t *sw_val_string(const char *s) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_STRING; v->v.str = strdup(s);
    return v;
}

sw_val_t *sw_val_atom(const char *s) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_ATOM; v->v.str = strdup(s);
    return v;
}

sw_val_t *sw_val_pid(sw_process_t *p) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_PID; v->v.pid = p;
    return v;
}

sw_val_t *sw_val_tuple(sw_val_t **items, int count) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_TUPLE;
    v->v.tuple.items = malloc(sizeof(sw_val_t*) * count);
    memcpy(v->v.tuple.items, items, sizeof(sw_val_t*) * count);
    v->v.tuple.count = count;
    return v;
}

sw_val_t *sw_val_list(sw_val_t **items, int count) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_LIST;
    v->v.tuple.items = malloc(sizeof(sw_val_t*) * count);
    memcpy(v->v.tuple.items, items, sizeof(sw_val_t*) * count);
    v->v.tuple.count = count;
    return v;
}

sw_val_t *sw_val_fun_native(void *fn_ptr, int nparams,
                             sw_val_t **captures, int ncaptures) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_FUN;
    v->v.fun.cfunc = fn_ptr;
    v->v.fun.num_params = nparams;
    v->v.fun.ncaptures = ncaptures;
    if (ncaptures > 0 && captures) {
        v->v.fun.captures = malloc(sizeof(sw_val_t*) * ncaptures);
        memcpy(v->v.fun.captures, captures, sizeof(sw_val_t*) * ncaptures);
    }
    return v;
}

sw_val_t *sw_val_apply(sw_val_t *fun, sw_val_t **args, int nargs) {
    if (!fun || fun->type != SW_VAL_FUN || !fun->v.fun.cfunc)
        return sw_val_nil();
    typedef sw_val_t *(*sw_fn_t)(sw_val_t **, int);
    sw_fn_t fn = (sw_fn_t)fun->v.fun.cfunc;
    if (fun->v.fun.ncaptures > 0) {
        int total = nargs + fun->v.fun.ncaptures;
        sw_val_t **all = malloc(sizeof(sw_val_t*) * total);
        if (nargs > 0) memcpy(all, args, sizeof(sw_val_t*) * nargs);
        memcpy(all + nargs, fun->v.fun.captures,
               sizeof(sw_val_t*) * fun->v.fun.ncaptures);
        sw_val_t *result = fn(all, total);
        free(all);
        return result;
    }
    return fn(args, nargs);
}

sw_val_t *sw_val_map_new(sw_val_t **keys, sw_val_t **vals, int count) {
    sw_val_t *v = calloc(1, sizeof(sw_val_t));
    v->type = SW_VAL_MAP;
    v->v.map.count = count;
    v->v.map.cap = count > 4 ? count : 4;
    v->v.map.keys = malloc(sizeof(sw_val_t*) * v->v.map.cap);
    v->v.map.vals = malloc(sizeof(sw_val_t*) * v->v.map.cap);
    if (count > 0 && keys && vals) {
        memcpy(v->v.map.keys, keys, sizeof(sw_val_t*) * count);
        memcpy(v->v.map.vals, vals, sizeof(sw_val_t*) * count);
    }
    return v;
}

sw_val_t *sw_val_map_get(sw_val_t *map, sw_val_t *key) {
    if (!map || map->type != SW_VAL_MAP) return sw_val_nil();
    for (int i = 0; i < map->v.map.count; i++)
        if (sw_val_equal(map->v.map.keys[i], key)) return map->v.map.vals[i];
    return sw_val_nil();
}

sw_val_t *sw_val_map_put(sw_val_t *map, sw_val_t *key, sw_val_t *val) {
    if (!map || map->type != SW_VAL_MAP) {
        return sw_val_map_new(&key, &val, 1);
    }
    for (int i = 0; i < map->v.map.count; i++) {
        if (sw_val_equal(map->v.map.keys[i], key)) {
            sw_val_t *nm = sw_val_map_new(map->v.map.keys, map->v.map.vals, map->v.map.count);
            nm->v.map.vals[i] = val;
            return nm;
        }
    }
    int n = map->v.map.count;
    sw_val_t **ks = malloc(sizeof(sw_val_t*) * (n+1));
    sw_val_t **vs = malloc(sizeof(sw_val_t*) * (n+1));
    if (n > 0) { memcpy(ks, map->v.map.keys, sizeof(sw_val_t*)*n); memcpy(vs, map->v.map.vals, sizeof(sw_val_t*)*n); }
    ks[n] = key; vs[n] = val;
    sw_val_t *nm = sw_val_map_new(ks, vs, n+1);
    free(ks); free(vs);
    return nm;
}

void sw_val_free(sw_val_t *v) {
    if (!v) return;
    switch (v->type) {
    case SW_VAL_STRING: case SW_VAL_ATOM: free(v->v.str); break;
    case SW_VAL_TUPLE: case SW_VAL_LIST:
        for (int i = 0; i < v->v.tuple.count; i++) sw_val_free(v->v.tuple.items[i]);
        free(v->v.tuple.items); break;
    case SW_VAL_FUN: /* don't free closure env -- owned by interpreter */ break;
    case SW_VAL_MAP:
        free(v->v.map.keys); free(v->v.map.vals); break;
    default: break;
    }
    free(v);
}

int sw_val_is_truthy(sw_val_t *v) {
    if (!v) return 0;
    switch (v->type) {
    case SW_VAL_NIL: return 0;
    case SW_VAL_INT: return v->v.i != 0;
    case SW_VAL_FLOAT: return v->v.f != 0.0;
    case SW_VAL_ATOM:
        return strcmp(v->v.str, "false") != 0 && strcmp(v->v.str, "nil") != 0;
    default: return 1;
    }
}

int sw_val_equal(sw_val_t *a, sw_val_t *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    /* Treat atom "nil" as equivalent to SW_VAL_NIL */
    if (a->type == SW_VAL_NIL && b->type == SW_VAL_ATOM && strcmp(b->v.str, "nil") == 0) return 1;
    if (b->type == SW_VAL_NIL && a->type == SW_VAL_ATOM && strcmp(a->v.str, "nil") == 0) return 1;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case SW_VAL_NIL: return 1;
    case SW_VAL_INT: return a->v.i == b->v.i;
    case SW_VAL_FLOAT: return a->v.f == b->v.f;
    case SW_VAL_STRING: case SW_VAL_ATOM: return strcmp(a->v.str, b->v.str) == 0;
    case SW_VAL_PID: return a->v.pid == b->v.pid;
    case SW_VAL_TUPLE: case SW_VAL_LIST:
        if (a->v.tuple.count != b->v.tuple.count) return 0;
        for (int i = 0; i < a->v.tuple.count; i++)
            if (!sw_val_equal(a->v.tuple.items[i], b->v.tuple.items[i])) return 0;
        return 1;
    case SW_VAL_MAP:
        if (a->v.map.count != b->v.map.count) return 0;
        for (int i = 0; i < a->v.map.count; i++) {
            sw_val_t *bv = sw_val_map_get((sw_val_t*)b, a->v.map.keys[i]);
            if (!sw_val_equal(a->v.map.vals[i], bv)) return 0;
        }
        return 1;
    default: return 0;
    }
}

void sw_val_print(sw_val_t *v) {
    if (!v) { printf("nil"); return; }
    switch (v->type) {
    case SW_VAL_NIL: printf("nil"); break;
    case SW_VAL_INT: printf("%lld", (long long)v->v.i); break;
    case SW_VAL_FLOAT: printf("%.17g", v->v.f); break;
    case SW_VAL_STRING: printf("%s", v->v.str); break;
    case SW_VAL_ATOM: printf(":%s", v->v.str); break;
    case SW_VAL_PID: printf("<pid:%llu>", v->v.pid ? v->v.pid->pid : 0); break;
    case SW_VAL_TUPLE:
        printf("{");
        for (int i = 0; i < v->v.tuple.count; i++) {
            if (i) printf(", ");
            sw_val_print(v->v.tuple.items[i]);
        }
        printf("}"); break;
    case SW_VAL_LIST:
        printf("[");
        for (int i = 0; i < v->v.tuple.count; i++) {
            if (i) printf(", ");
            sw_val_print(v->v.tuple.items[i]);
        }
        printf("]"); break;
    case SW_VAL_MAP:
        printf("%%{");
        for (int i = 0; i < v->v.map.count; i++) {
            if (i) printf(", ");
            sw_val_print(v->v.map.keys[i]);
            printf(": ");
            sw_val_print(v->v.map.vals[i]);
        }
        printf("}"); break;
    default: printf("?"); break;
    }
}

/* =========================================================================
 * Environment
 * ========================================================================= */

static sw_env_t *env_new(sw_env_t *parent) {
    sw_env_t *e = calloc(1, sizeof(sw_env_t));
    e->parent = parent;
    return e;
}

static uint32_t env_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (uint8_t)*s++;
    return h % SW_ENV_SLOTS;
}

static void env_set(sw_env_t *e, const char *name, sw_val_t *val) {
    uint32_t h = env_hash(name);
    /* Check existing */
    for (sw_env_entry_t *ent = e->buckets[h]; ent; ent = ent->next) {
        if (strcmp(ent->name, name) == 0) {
            ent->val = val; /* replace (no free — values may be shared) */
            return;
        }
    }
    sw_env_entry_t *ent = malloc(sizeof(sw_env_entry_t));
    ent->name = strdup(name);
    ent->val = val;
    ent->next = e->buckets[h];
    e->buckets[h] = ent;
}

static sw_val_t *env_get(sw_env_t *e, const char *name) {
    for (sw_env_t *cur = e; cur; cur = cur->parent) {
        uint32_t h = env_hash(name);
        for (sw_env_entry_t *ent = cur->buckets[h]; ent; ent = ent->next) {
            if (strcmp(ent->name, name) == 0) return ent->val;
        }
    }
    return NULL;
}

static void env_free(sw_env_t *e) {
    if (!e) return;
    for (int i = 0; i < SW_ENV_SLOTS; i++) {
        sw_env_entry_t *ent = e->buckets[i];
        while (ent) {
            sw_env_entry_t *next = ent->next;
            free(ent->name);
            /* Don't free val — owned by interpreter or caller */
            free(ent);
            ent = next;
        }
    }
    free(e);
}

/* =========================================================================
 * Interpreter
 * ========================================================================= */

/* Internal: evaluate a node in an environment */
static sw_val_t *eval(sw_interp_t *interp, node_t *n, sw_env_t *env);

/* Find function by name in module */
static node_t *find_fun(node_t *mod, const char *name) {
    for (int i = 0; i < mod->v.mod.nfuns; i++) {
        if (strcmp(mod->v.mod.funs[i]->v.fun.name, name) == 0)
            return mod->v.mod.funs[i];
    }
    return NULL;
}

/* Pattern match: check if val matches pattern, bind variables in env.
 * Returns 1 on match, 0 on mismatch. */
static int pattern_match(node_t *pattern, sw_val_t *val, sw_env_t *env) {
    if (!pattern || !val) return 0;

    switch (pattern->type) {
    case N_IDENT:
        /* Variable: always matches, binds */
        env_set(env, pattern->v.sval, val);
        return 1;

    case N_INT:
        return val->type == SW_VAL_INT && val->v.i == pattern->v.ival;

    case N_FLOAT:
        return val->type == SW_VAL_FLOAT && val->v.f == pattern->v.fval;

    case N_STRING:
        return val->type == SW_VAL_STRING && strcmp(val->v.str, pattern->v.sval) == 0;

    case N_ATOM:
        return val->type == SW_VAL_ATOM && strcmp(val->v.str, pattern->v.sval) == 0;

    case N_TUPLE:
        if (val->type != SW_VAL_TUPLE || val->v.tuple.count != pattern->v.coll.count) return 0;
        for (int i = 0; i < pattern->v.coll.count; i++) {
            if (!pattern_match(pattern->v.coll.items[i], val->v.tuple.items[i], env))
                return 0;
        }
        return 1;

    case N_LIST:
        if (val->type != SW_VAL_LIST || val->v.tuple.count != pattern->v.coll.count) return 0;
        for (int i = 0; i < pattern->v.coll.count; i++) {
            if (!pattern_match(pattern->v.coll.items[i], val->v.tuple.items[i], env))
                return 0;
        }
        return 1;

    case N_LIST_CONS:
        if (val->type != SW_VAL_LIST || val->v.tuple.count < 1) return 0;
        if (!pattern_match(pattern->v.cons.head, val->v.tuple.items[0], env)) return 0;
        {
            sw_val_t *tail = sw_val_list(val->v.tuple.items + 1, val->v.tuple.count - 1);
            if (!pattern_match(pattern->v.cons.tail, tail, env)) return 0;
        }
        return 1;

    case N_MAP:
        if (val->type != SW_VAL_MAP) return 0;
        for (int i = 0; i < pattern->v.map.count; i++) {
            /* Keys in map patterns are literals: atoms (from name:) or strings */
            node_t *kn = pattern->v.map.keys[i];
            sw_val_t *pkey = NULL;
            if (kn->type == N_ATOM) pkey = sw_val_atom(kn->v.sval);
            else if (kn->type == N_STRING) pkey = sw_val_string(kn->v.sval);
            else if (kn->type == N_INT) pkey = sw_val_int(kn->v.ival);
            else return 0;
            sw_val_t *pval_found = sw_val_map_get(val, pkey);
            if (pval_found->type == SW_VAL_NIL) return 0;
            if (!pattern_match(pattern->v.map.vals[i], pval_found, env)) return 0;
        }
        return 1;

    default:
        return 0;
    }
}

/* Convert sw_val_t to a serializable message for sw_send_tagged */
typedef struct {
    sw_val_type_t type;
    int64_t ival;
    double fval;
    int count;         /* tuple/list item count */
    char sval[128];
} sw_msg_val_t;

static sw_msg_val_t *serialize_val(sw_val_t *v, int *out_len) {
    /* Simple flat serialization: single value only (no nested tuples via wire) */
    sw_msg_val_t *m = calloc(1, sizeof(sw_msg_val_t));
    *out_len = sizeof(sw_msg_val_t);
    m->type = v->type;
    switch (v->type) {
    case SW_VAL_INT: m->ival = v->v.i; break;
    case SW_VAL_FLOAT: m->fval = v->v.f; break;
    case SW_VAL_STRING: case SW_VAL_ATOM:
        strncpy(m->sval, v->v.str, sizeof(m->sval)-1); break;
    case SW_VAL_TUPLE: case SW_VAL_LIST:
        m->count = v->v.tuple.count; break;
    default: break;
    }
    return m;
}

static sw_val_t *deserialize_val(sw_msg_val_t *m) {
    switch (m->type) {
    case SW_VAL_INT: return sw_val_int(m->ival);
    case SW_VAL_FLOAT: return sw_val_float(m->fval);
    case SW_VAL_STRING: return sw_val_string(m->sval);
    case SW_VAL_ATOM: return sw_val_atom(m->sval);
    default: return sw_val_nil();
    }
}

/* Built-in function: print */
static sw_val_t *builtin_print(sw_val_t **args, int nargs) {
    for (int i = 0; i < nargs; i++) {
        if (i) printf(" ");
        sw_val_print(args[i]);
    }
    printf("\n");
    return sw_val_atom("ok");
}

/* Built-in function: length */
static sw_val_t *builtin_length(sw_val_t **args, int nargs) {
    if (nargs < 1) return sw_val_int(0);
    sw_val_t *v = args[0];
    if (v->type == SW_VAL_LIST || v->type == SW_VAL_TUPLE)
        return sw_val_int(v->v.tuple.count);
    if (v->type == SW_VAL_STRING)
        return sw_val_int((int64_t)strlen(v->v.str));
    return sw_val_int(0);
}

/* Built-in function: hd (head of list) */
static sw_val_t *builtin_hd(sw_val_t **args, int nargs) {
    if (nargs < 1 || args[0]->type != SW_VAL_LIST || args[0]->v.tuple.count == 0)
        return sw_val_nil();
    return args[0]->v.tuple.items[0];
}

/* Built-in function: tl (tail of list) */
static sw_val_t *builtin_tl(sw_val_t **args, int nargs) {
    if (nargs < 1 || args[0]->type != SW_VAL_LIST || args[0]->v.tuple.count <= 1)
        return sw_val_list(NULL, 0);
    return sw_val_list(args[0]->v.tuple.items + 1, args[0]->v.tuple.count - 1);
}

/* Built-in function: elem (tuple element access) */
static sw_val_t *builtin_elem(sw_val_t **args, int nargs) {
    if (nargs < 2 || args[0]->type != SW_VAL_TUPLE || args[1]->type != SW_VAL_INT)
        return sw_val_nil();
    int idx = (int)args[1]->v.i;
    if (idx < 0 || idx >= args[0]->v.tuple.count) return sw_val_nil();
    return args[0]->v.tuple.items[idx];
}

/* Evaluate node */
static sw_val_t *eval(sw_interp_t *interp, node_t *n, sw_env_t *env) {
    if (!n || interp->error) return sw_val_nil();

    switch (n->type) {
    case N_INT: return sw_val_int(n->v.ival);
    case N_FLOAT: return sw_val_float(n->v.fval);
    case N_STRING: return sw_val_string(n->v.sval);
    case N_ATOM: return sw_val_atom(n->v.sval);
    case N_SELF: return sw_val_pid(sw_self());

    case N_IDENT: {
        sw_val_t *v = env_get(env, n->v.sval);
        if (v) return v;
        /* Could be a function reference */
        return sw_val_nil();
    }

    case N_ASSIGN: {
        sw_val_t *v = eval(interp, n->v.assign.value, env);
        env_set(env, n->v.assign.name, v);
        return v;
    }

    case N_BLOCK: {
        sw_val_t *result = sw_val_nil();
        for (int i = 0; i < n->v.block.nstmts; i++) {
            result = eval(interp, n->v.block.stmts[i], env);
        }
        return result;
    }

    case N_BINOP: {
        sw_val_t *left = eval(interp, n->v.binop.left, env);
        sw_val_t *right = eval(interp, n->v.binop.right, env);
        const char *op = n->v.binop.op;

        /* String concatenation */
        if (strcmp(op, "++") == 0) {
            char buf[1024] = "";
            if (left->type == SW_VAL_STRING) strcat(buf, left->v.str);
            else if (left->type == SW_VAL_INT) { char tmp[64]; snprintf(tmp, 64, "%lld", (long long)left->v.i); strcat(buf, tmp); }
            if (right->type == SW_VAL_STRING) strcat(buf, right->v.str);
            else if (right->type == SW_VAL_INT) { char tmp[64]; snprintf(tmp, 64, "%lld", (long long)right->v.i); strcat(buf, tmp); }
            return sw_val_string(buf);
        }

        /* Arithmetic on ints */
        if (left->type == SW_VAL_INT && right->type == SW_VAL_INT) {
            int64_t a = left->v.i, b = right->v.i;
            if (strcmp(op, "+") == 0)  return sw_val_int(a + b);
            if (strcmp(op, "-") == 0)  return sw_val_int(a - b);
            if (strcmp(op, "*") == 0)  return sw_val_int(a * b);
            if (strcmp(op, "/") == 0)  return b ? sw_val_int(a / b) : sw_val_nil();
            if (strcmp(op, "==") == 0) return sw_val_atom(a == b ? "true" : "false");
            if (strcmp(op, "!=") == 0) return sw_val_atom(a != b ? "true" : "false");
            if (strcmp(op, "<") == 0)  return sw_val_atom(a < b ? "true" : "false");
            if (strcmp(op, ">") == 0)  return sw_val_atom(a > b ? "true" : "false");
            if (strcmp(op, "<=") == 0) return sw_val_atom(a <= b ? "true" : "false");
            if (strcmp(op, ">=") == 0) return sw_val_atom(a >= b ? "true" : "false");
        }

        /* Arithmetic on floats */
        if ((left->type == SW_VAL_FLOAT || left->type == SW_VAL_INT) &&
            (right->type == SW_VAL_FLOAT || right->type == SW_VAL_INT)) {
            double a = left->type == SW_VAL_INT ? (double)left->v.i : left->v.f;
            double b = right->type == SW_VAL_INT ? (double)right->v.i : right->v.f;
            if (strcmp(op, "+") == 0) return sw_val_float(a + b);
            if (strcmp(op, "-") == 0) return sw_val_float(a - b);
            if (strcmp(op, "*") == 0) return sw_val_float(a * b);
            if (strcmp(op, "/") == 0) return b != 0 ? sw_val_float(a / b) : sw_val_nil();
        }

        /* Equality for atoms/strings */
        if (strcmp(op, "==") == 0) return sw_val_atom(sw_val_equal(left, right) ? "true" : "false");
        if (strcmp(op, "!=") == 0) return sw_val_atom(!sw_val_equal(left, right) ? "true" : "false");

        /* Logical */
        if (strcmp(op, "&&") == 0) return sw_val_atom(sw_val_is_truthy(left) && sw_val_is_truthy(right) ? "true" : "false");
        if (strcmp(op, "||") == 0) return sw_val_atom(sw_val_is_truthy(left) || sw_val_is_truthy(right) ? "true" : "false");

        return sw_val_nil();
    }

    case N_UNARY: {
        sw_val_t *v = eval(interp, n->v.unary.operand, env);
        if (n->v.unary.op == '-') {
            if (v->type == SW_VAL_INT) return sw_val_int(-v->v.i);
            if (v->type == SW_VAL_FLOAT) return sw_val_float(-v->v.f);
        }
        return v;
    }

    case N_IF: {
        sw_val_t *cond = eval(interp, n->v.iff.cond, env);
        if (sw_val_is_truthy(cond)) {
            return eval(interp, n->v.iff.then_b, env);
        } else if (n->v.iff.else_b) {
            return eval(interp, n->v.iff.else_b, env);
        }
        return sw_val_nil();
    }

    case N_TUPLE: {
        sw_val_t *items[64];
        int count = n->v.coll.count;
        if (count > 64) count = 64;
        for (int i = 0; i < count; i++)
            items[i] = eval(interp, n->v.coll.items[i], env);
        return sw_val_tuple(items, count);
    }

    case N_LIST: {
        sw_val_t *items[256];
        int count = n->v.coll.count;
        if (count > 256) count = 256;
        for (int i = 0; i < count; i++)
            items[i] = eval(interp, n->v.coll.items[i], env);
        return sw_val_list(items, count);
    }

    case N_PIPE: {
        /* val |> func  →  func(val) */
        sw_val_t *val = eval(interp, n->v.pipe.val, env);
        /* func should be a call or ident — apply val as first arg */
        if (n->v.pipe.func->type == N_CALL) {
            /* Add val as first arg */
            node_t *call = n->v.pipe.func;
            sw_val_t *func_name_val = eval(interp, call->v.call.func, env);
            (void)func_name_val;
            const char *fname = call->v.call.func->v.sval;

            sw_val_t *args[17];
            args[0] = val;
            int nargs = 1;
            for (int i = 0; i < call->v.call.nargs && nargs < 16; i++)
                args[nargs++] = eval(interp, call->v.call.args[i], env);

            /* Look up function in module */
            node_t *fn = find_fun(interp->module_ast, fname);
            if (fn) {
                sw_env_t *fenv = env_new(interp->global_env);
                for (int i = 0; i < fn->v.fun.nparams && i < nargs; i++)
                    env_set(fenv, fn->v.fun.params[i], args[i]);
                sw_val_t *r = eval(interp, fn->v.fun.body, fenv);
                env_free(fenv);
                return r;
            }
            /* Builtins */
            if (strcmp(fname, "print") == 0) return builtin_print(args, nargs);
            if (strcmp(fname, "length") == 0) return builtin_length(args, nargs);
        }
        return val;
    }

    case N_CALL: {
        const char *fname = n->v.call.func->v.sval;
        sw_val_t *args[16];
        int nargs = n->v.call.nargs;
        if (nargs > 16) nargs = 16;
        for (int i = 0; i < nargs; i++)
            args[i] = eval(interp, n->v.call.args[i], env);

        /* Built-ins */
        if (strcmp(fname, "print") == 0) return builtin_print(args, nargs);
        if (strcmp(fname, "length") == 0) return builtin_length(args, nargs);
        if (strcmp(fname, "hd") == 0) return builtin_hd(args, nargs);
        if (strcmp(fname, "tl") == 0) return builtin_tl(args, nargs);
        if (strcmp(fname, "elem") == 0) return builtin_elem(args, nargs);
        if (strcmp(fname, "abs") == 0 && nargs >= 1 && args[0]->type == SW_VAL_INT)
            return sw_val_int(args[0]->v.i < 0 ? -args[0]->v.i : args[0]->v.i);
        if (strcmp(fname, "to_string") == 0 && nargs >= 1) {
            char buf[256];
            switch (args[0]->type) {
            case SW_VAL_INT: snprintf(buf, sizeof(buf), "%lld", (long long)args[0]->v.i); return sw_val_string(buf);
            case SW_VAL_FLOAT: snprintf(buf, sizeof(buf), "%g", args[0]->v.f); return sw_val_string(buf);
            case SW_VAL_STRING: return args[0];
            case SW_VAL_ATOM: return sw_val_string(args[0]->v.str);
            case SW_VAL_NIL: return sw_val_string("nil");
            default: return sw_val_string("<val>");
            }
        }
        if (strcmp(fname, "map_get") == 0 && nargs >= 2) return sw_val_map_get(args[0], args[1]);
        if (strcmp(fname, "map_put") == 0 && nargs >= 3) return sw_val_map_put(args[0], args[1], args[2]);
        if (strcmp(fname, "map_new") == 0) return sw_val_map_new(NULL, NULL, 0);
        if (strcmp(fname, "map_keys") == 0 && nargs >= 1 && args[0]->type == SW_VAL_MAP) {
            return sw_val_list(args[0]->v.map.keys, args[0]->v.map.count);
        }
        if (strcmp(fname, "map_values") == 0 && nargs >= 1 && args[0]->type == SW_VAL_MAP) {
            return sw_val_list(args[0]->v.map.vals, args[0]->v.map.count);
        }
        if (strcmp(fname, "typeof") == 0 && nargs >= 1) {
            const char *names[] = {"nil","int","float","string","atom","pid","tuple","list","fun","map"};
            return sw_val_string(names[args[0]->type < 10 ? args[0]->type : 0]);
        }
        if (strcmp(fname, "list_append") == 0 && nargs >= 2 && args[0]->type == SW_VAL_LIST) {
            int cnt = args[0]->v.tuple.count;
            sw_val_t **items = malloc(sizeof(sw_val_t*) * (cnt + 1));
            memcpy(items, args[0]->v.tuple.items, sizeof(sw_val_t*) * cnt);
            items[cnt] = args[1];
            sw_val_t *r = sw_val_list(items, cnt + 1);
            free(items);
            return r;
        }

        /* User-defined function in module */
        node_t *fn = find_fun(interp->module_ast, fname);
        if (fn) {
            sw_env_t *fenv = env_new(interp->global_env);
            for (int i = 0; i < fn->v.fun.nparams; i++) {
                if (i < nargs)
                    env_set(fenv, fn->v.fun.params[i], args[i]);
                else if (fn->v.fun.defaults[i])
                    env_set(fenv, fn->v.fun.params[i], eval(interp, fn->v.fun.defaults[i], fenv));
                else
                    env_set(fenv, fn->v.fun.params[i], sw_val_nil());
            }
            sw_val_t *r = eval(interp, fn->v.fun.body, fenv);
            env_free(fenv);
            return r;
        }

        /* Dynamic dispatch: variable holds a closure */
        sw_val_t *fn_val = env_get(env, fname);
        if (fn_val && fn_val->type == SW_VAL_FUN && fn_val->v.fun.body) {
            node_t *fn_node = (node_t *)fn_val->v.fun.body;
            sw_env_t *fenv = env_new(fn_val->v.fun.closure_env ? fn_val->v.fun.closure_env : interp->global_env);
            for (int i = 0; i < fn_node->v.fun.nparams && i < nargs; i++)
                env_set(fenv, fn_node->v.fun.params[i], args[i]);
            sw_val_t *r = eval(interp, fn_node->v.fun.body, fenv);
            env_free(fenv);
            return r;
        }

        snprintf(interp->error_msg, sizeof(interp->error_msg),
                 "undefined function: %s/%d", fname, nargs);
        interp->error = 1;
        return sw_val_nil();
    }

    case N_SPAWN: {
        /* spawn(func(args)) — for now, limited: we can't easily spawn
         * an interpreter process. Store the expression result as a pid.
         * This is a simplified demo — real spawn would need a trampoline. */
        /* Just evaluate the inner expression and return nil pid for now */
        sw_val_t *inner = eval(interp, n->v.spawn.expr, env);
        (void)inner;
        return sw_val_pid(NULL);
    }

    case N_SEND: {
        sw_val_t *to = eval(interp, n->v.send.to, env);
        sw_val_t *msg = eval(interp, n->v.send.msg, env);
        if (to->type == SW_VAL_PID && to->v.pid) {
            int msg_len = 0;
            sw_msg_val_t *m = serialize_val(msg, &msg_len);
            sw_send_tagged(to->v.pid, SW_TAG_CAST, m);
        }
        return msg;
    }

    case N_MAP: {
        sw_val_t *keys[64], *vals[64];
        int count = n->v.map.count;
        if (count > 64) count = 64;
        for (int i = 0; i < count; i++) {
            keys[i] = eval(interp, n->v.map.keys[i], env);
            vals[i] = eval(interp, n->v.map.vals[i], env);
        }
        return sw_val_map_new(keys, vals, count);
    }

    case N_FOR: {
        sw_val_t *iter = eval(interp, n->v.forloop.iter, env);
        sw_val_t *result = sw_val_nil();
        if (iter->type == SW_VAL_LIST) {
            for (int i = 0; i < iter->v.tuple.count; i++) {
                env_set(env, n->v.forloop.var, iter->v.tuple.items[i]);
                result = eval(interp, n->v.forloop.body, env);
            }
        }
        return result;
    }

    case N_RANGE: {
        sw_val_t *from = eval(interp, n->v.range.from, env);
        sw_val_t *to = eval(interp, n->v.range.to, env);
        if (from->type != SW_VAL_INT || to->type != SW_VAL_INT)
            return sw_val_list(NULL, 0);
        int64_t lo = from->v.i, hi = to->v.i;
        int cnt = (int)(hi - lo + 1);
        if (cnt <= 0) return sw_val_list(NULL, 0);
        if (cnt > 10000) cnt = 10000;
        sw_val_t **items = malloc(sizeof(sw_val_t*) * cnt);
        for (int i = 0; i < cnt; i++) items[i] = sw_val_int(lo + i);
        sw_val_t *r = sw_val_list(items, cnt);
        free(items);
        return r;
    }

    case N_TRY: {
        sw_val_t *result = eval(interp, n->v.trycatch.body, env);
        if (interp->error) {
            interp->error = 0;
            sw_env_t *catch_env = env_new(env);
            env_set(catch_env, n->v.trycatch.err_var, sw_val_string(interp->error_msg));
            interp->error_msg[0] = 0;
            result = eval(interp, n->v.trycatch.catch_body, catch_env);
            env_free(catch_env);
        }
        return result;
    }

    case N_LIST_CONS: {
        /* [h | t] — cons: prepend head to tail list */
        sw_val_t *head = eval(interp, n->v.cons.head, env);
        sw_val_t *tail = eval(interp, n->v.cons.tail, env);
        if (tail->type == SW_VAL_LIST) {
            int cnt = tail->v.tuple.count + 1;
            sw_val_t **items = malloc(sizeof(sw_val_t*) * cnt);
            items[0] = head;
            memcpy(items + 1, tail->v.tuple.items, sizeof(sw_val_t*) * tail->v.tuple.count);
            sw_val_t *r = sw_val_list(items, cnt);
            free(items);
            return r;
        }
        return sw_val_list(&head, 1);
    }

    case N_FUN: {
        /* Anonymous function: capture closure env */
        sw_val_t *v = calloc(1, sizeof(sw_val_t));
        v->type = SW_VAL_FUN;
        v->v.fun.name = strdup(n->v.fun.name);
        v->v.fun.num_params = n->v.fun.nparams;
        v->v.fun.body = n;
        v->v.fun.closure_env = env;
        return v;
    }

    case N_RECEIVE: {
        /* Simplified receive: wait for a message, try to match clauses */
        uint64_t timeout = n->v.recv.after_ms >= 0 ? (uint64_t)n->v.recv.after_ms : 5000;
        uint64_t tag = 0;
        void *raw = sw_receive_any(timeout, &tag);
        if (!raw) {
            /* Timeout */
            if (n->v.recv.after_body) {
                return eval(interp, n->v.recv.after_body, env);
            }
            return sw_val_nil();
        }
        sw_msg_val_t *m = (sw_msg_val_t *)raw;
        sw_val_t *msg = deserialize_val(m);
        free(raw);

        /* Try clauses */
        for (int i = 0; i < n->v.recv.nclauses; i++) {
            node_t *cl = n->v.recv.clauses[i];
            sw_env_t *cl_env = env_new(env);
            if (pattern_match(cl->v.clause.pattern, msg, cl_env)) {
                /* Check guard if present */
                if (cl->v.clause.guard) {
                    sw_val_t *gv = eval(interp, cl->v.clause.guard, cl_env);
                    if (!sw_val_is_truthy(gv)) { env_free(cl_env); continue; }
                }
                sw_val_t *r = eval(interp, cl->v.clause.body, cl_env);
                env_free(cl_env);
                return r;
            }
            env_free(cl_env);
        }
        return msg;
    }

    default:
        return sw_val_nil();
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void *sw_lang_parse(const char *source) {
    par_t p;
    par_init(&p, source);
    node_t *mod = par_module(&p);
    if (p.err) {
        fprintf(stderr, "Parse error: %s\n", p.errmsg);
        node_free(mod);
        return NULL;
    }
    return mod;
}

sw_interp_t *sw_lang_new(void *module_ast) {
    sw_interp_t *interp = calloc(1, sizeof(sw_interp_t));
    interp->module_ast = module_ast;
    interp->global_env = env_new(NULL);
    return interp;
}

sw_val_t *sw_lang_call(sw_interp_t *interp, const char *func_name,
                        sw_val_t **args, int num_args) {
    node_t *mod = (node_t *)interp->module_ast;
    node_t *fn = find_fun(mod, func_name);
    if (!fn) {
        snprintf(interp->error_msg, sizeof(interp->error_msg),
                 "function '%s' not found in module '%s'", func_name, mod->v.mod.name);
        interp->error = 1;
        return sw_val_nil();
    }

    sw_env_t *fenv = env_new(interp->global_env);
    for (int i = 0; i < fn->v.fun.nparams && i < num_args; i++)
        env_set(fenv, fn->v.fun.params[i], args[i]);

    sw_val_t *result = eval(interp, fn->v.fun.body, fenv);
    env_free(fenv);
    return result;
}

sw_val_t *sw_lang_eval(sw_interp_t *interp, const char *expr_source) {
    /* Wrap in a minimal module for eval */
    char wrapped[2048];
    snprintf(wrapped, sizeof(wrapped),
             "module _Eval\nfun _eval() {\n%s\n}\n", expr_source);

    par_t p;
    par_init(&p, wrapped);
    node_t *mod = par_module(&p);
    if (p.err) {
        node_free(mod);
        return sw_val_nil();
    }

    void *save_mod = interp->module_ast;
    interp->module_ast = mod;
    sw_val_t *result = sw_lang_call(interp, "_eval", NULL, 0);
    interp->module_ast = save_mod;
    node_free(mod);
    return result;
}

void sw_lang_free(sw_interp_t *interp) {
    if (!interp) return;
    if (interp->module_ast) node_free(interp->module_ast);
    env_free(interp->global_env);
    free(interp);
}

/* === JSON decode for distribution layer === */

static sw_val_t *_jd_parse(const char **pp);

static void _jd_skip_ws(const char **pp) {
    while (**pp == ' ' || **pp == '\t' || **pp == '\n' || **pp == '\r') (*pp)++;
}

static sw_val_t *_jd_parse_string(const char **pp) {
    (*pp)++;
    char buf[8192];
    int len = 0;
    while (**pp && **pp != '"' && len < (int)sizeof(buf) - 2) {
        if (**pp == '\\') {
            (*pp)++;
            switch (**pp) {
                case 'n': buf[len++] = '\n'; break;
                case 't': buf[len++] = '\t'; break;
                case 'r': buf[len++] = '\r'; break;
                default:  buf[len++] = **pp; break;
            }
        } else buf[len++] = **pp;
        (*pp)++;
    }
    if (**pp == '"') (*pp)++;
    buf[len] = 0;
    return sw_val_string(buf);
}

static sw_val_t *_jd_parse(const char **pp) {
    _jd_skip_ws(pp);
    if (**pp == '"') return _jd_parse_string(pp);
    if (**pp == '[') {
        (*pp)++; _jd_skip_ws(pp);
        sw_val_t *items[256]; int cnt = 0;
        while (**pp && **pp != ']' && cnt < 256) {
            items[cnt++] = _jd_parse(pp);
            _jd_skip_ws(pp);
            if (**pp == ',') (*pp)++;
            _jd_skip_ws(pp);
        }
        if (**pp == ']') (*pp)++;
        return sw_val_list(items, cnt);
    }
    if (**pp == '{') {
        (*pp)++; _jd_skip_ws(pp);
        sw_val_t *keys[128], *vals[128]; int cnt = 0;
        while (**pp && **pp != '}' && cnt < 128) {
            _jd_skip_ws(pp);
            if (**pp != '"') break;
            sw_val_t *k = _jd_parse_string(pp);
            keys[cnt] = sw_val_atom(k->v.str);
            _jd_skip_ws(pp);
            if (**pp == ':') (*pp)++;
            vals[cnt] = _jd_parse(pp);
            cnt++;
            _jd_skip_ws(pp);
            if (**pp == ',') (*pp)++;
        }
        if (**pp == '}') (*pp)++;
        return sw_val_map_new(keys, vals, cnt);
    }
    if (**pp == 't' && strncmp(*pp, "true", 4) == 0)  { *pp += 4; return sw_val_atom("true"); }
    if (**pp == 'f' && strncmp(*pp, "false", 5) == 0) { *pp += 5; return sw_val_atom("false"); }
    if (**pp == 'n' && strncmp(*pp, "null", 4) == 0)  { *pp += 4; return sw_val_nil(); }
    /* Number */
    const char *start = *pp;
    int is_float = 0;
    if (**pp == '-') (*pp)++;
    while (**pp >= '0' && **pp <= '9') (*pp)++;
    if (**pp == '.') { is_float = 1; (*pp)++; while (**pp >= '0' && **pp <= '9') (*pp)++; }
    if (**pp == 'e' || **pp == 'E') { is_float = 1; (*pp)++; if (**pp == '+' || **pp == '-') (*pp)++; while (**pp >= '0' && **pp <= '9') (*pp)++; }
    if (*pp == start) { (*pp)++; return sw_val_nil(); }
    char tmp[64];
    size_t numlen = *pp - start;
    if (numlen > 63) numlen = 63;
    memcpy(tmp, start, numlen); tmp[numlen] = 0;
    return is_float ? sw_val_float(strtod(tmp, NULL)) : sw_val_int(strtoll(tmp, NULL, 10));
}

sw_val_t *sw_lang_json_decode(const char *s) {
    if (!s) return sw_val_nil();
    const char *p = s;
    return _jd_parse(&p);
}
