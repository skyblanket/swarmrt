/*
 * SwarmRT Phase 10: Language Frontend
 *
 * Tree-walking interpreter for .sw files. Contains a self-contained lexer,
 * parser, and evaluator. Maps spawn/send/receive to SwarmRT runtime calls.
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
#include <math.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sqlite3.h>
#include <pthread.h>
#include "swarmrt_lang.h"
#include "swarmrt_ets.h"
#include "swarmrt_varena.h"  /* per-process value arena (GC v1) */
#include "swarmrt_node.h"    /* sw_send_dispatch: structure-preserving send */
#include "swarmrt_audio.h"   /* G.711 / PCM16 / resample for REPL+test parity */
#include "swarmrt_otp.h"     /* sw_supervisor_start + child spec (interp supervise) */

/* ed25519_verify lives in swarmrt_builtins_studio.h for the compiled path, but
 * the interpreter (this file) doesn't pull in that header. To keep REPL/codegen
 * parity it gets a self-contained copy here, behind the same SWARMRT_TLS gate
 * (auto-on for non-Apple, opt-in via `make SWARMRT_TLS=1` on macOS). */
#ifndef SWARMRT_TLS
#  ifndef __APPLE__
#    define SWARMRT_TLS 1
#  endif
#endif
#ifdef SWARMRT_TLS
#  include <openssl/evp.h>
/* Coerce a sw value to a malloc'd raw byte buffer (mirrors _sw_ed_coerce_bytes
 * in swarmrt_builtins_studio.h): SW_VAL_BYTES -> copy; SW_VAL_STRING with
 * want_b64 -> base64-decode; SW_VAL_STRING without -> the string bytes verbatim.
 * NULL on type mismatch / decode failure. Caller frees. */
static uint8_t *_sw_lang_ed_coerce_bytes(sw_val_t *v, int want_b64, size_t *out_len) {
    *out_len = 0;
    if (!v) return NULL;
    if (v->type == SW_VAL_BYTES) {
        size_t n = v->v.bytes.len;
        uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
        if (!buf) return NULL;
        if (n) memcpy(buf, v->v.bytes.data, n);
        *out_len = n;
        return buf;
    }
    if (v->type == SW_VAL_STRING) {
        if (want_b64) {
            size_t n = 0;
            uint8_t *raw = _sw_audio_b64_decode(v->v.str, &n);
            if (!raw) return NULL;
            *out_len = n;
            return raw;
        }
        size_t n = strlen(v->v.str);
        uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
        if (!buf) return NULL;
        if (n) memcpy(buf, v->v.str, n);
        *out_len = n;
        return buf;
    }
    return NULL;
}
#endif

/* ed25519_verify(public_key, signature, message) -> 'true'|'false'|nil.
 * Self-contained interpreter twin of _builtin_ed25519_verify; identical
 * contract and behavior (see swarmrt_builtins_studio.h for the full doc). */
static sw_val_t *_sw_lang_ed25519_verify(sw_val_t **args, int nargs) {
#ifndef SWARMRT_TLS
    (void)args; (void)nargs;
    fprintf(stderr, "ed25519_verify: requires a TLS build "
                    "(rebuild with -DSWARMRT_TLS and link openssl); "
                    "or shell out to `openssl pkeyutl -verify -rawin`.\n");
    return sw_val_nil();
#else
    if (nargs < 3) return sw_val_atom("false");
    size_t pk_len = 0, sig_len = 0, msg_len = 0;
    uint8_t *pk  = _sw_lang_ed_coerce_bytes(args[0], 1, &pk_len);
    uint8_t *sig = _sw_lang_ed_coerce_bytes(args[1], 1, &sig_len);
    uint8_t *msg = _sw_lang_ed_coerce_bytes(args[2], 0, &msg_len);
    sw_val_t *result = sw_val_atom("false");
    if (pk && sig && msg && pk_len == 32 && sig_len == 64) {
        EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pk, pk_len);
        if (pkey) {
            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
            if (ctx) {
                if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1) {
                    int rc = EVP_DigestVerify(ctx, sig, sig_len, msg, msg_len);
                    if (rc == 1) result = sw_val_atom("true");
                }
                EVP_MD_CTX_free(ctx);
            }
            EVP_PKEY_free(pkey);
        }
    }
    free(pk); free(sig); free(msg);
    return result;
#endif
}

/* Weak stubs for runtime functions — allows linking swc without the runtime.
 * These are only called by the interpreter, never by the compiler. */
__attribute__((weak)) void *sw_receive_any(uint64_t t, uint64_t *tag) { (void)t; (void)tag; return NULL; }
__attribute__((weak)) void *sw_recv_any_adopt(uint64_t t, uint64_t *tag) { (void)t; (void)tag; return NULL; }
__attribute__((weak)) void sw_send_tagged(sw_process_t *to, uint64_t tag, void *msg) { (void)to; (void)tag; (void)msg; }
__attribute__((weak)) void sw_send_tagged_msg(sw_process_t *to, uint64_t tag, void *payload, struct sw_value_arena *region) { (void)to; (void)tag; (void)payload; (void)region; }
__attribute__((weak)) sw_process_t *sw_self(void) { return NULL; }
/* Returns the current process's value arena, or NULL (interpreter / pre-fiber).
 * Strong impl in swarmrt_native.c reads tls_current->varena; this weak stub
 * keeps swc linkable without the runtime and makes the interpreter use calloc. */
__attribute__((weak)) sw_value_arena_t *sw_self_varena(void) { return NULL; }
__attribute__((weak)) void sw_set_self_varena(sw_value_arena_t *a) { (void)a; }
/* Reports the current process fiber's stack bounds (1 + fills low/high) when
 * running on a fiber, else 0 (use OS-thread bounds). Strong impl in
 * swarmrt_native.c. Weak stub keeps swc/runtime-less linkable and makes the
 * interpreter fall back to pthread stack introspection. */
__attribute__((weak)) int sw_self_stack_bounds(uintptr_t *low, uintptr_t *high) {
    (void)low; (void)high; return 0;
}

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
    TOK_SPAWN_MONITOR,
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
    TOK_LARROW,     /* <- — `with` bind arrow */
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
    TOK_LET,       /* let — module-level global binding */
    TOK_DOTDOT,    /* .. */
    TOK_BAR,       /* | (single pipe, list cons) */
    TOK_PERCENT,   /* % — binary modulo */
    TOK_MAP_OPEN,  /* %{ — map-literal start (consumed atomically by lexer
                    *      so the parser can disambiguate from `expr % {…}`) */
    /* Phase 13: f-string interpolation. Lexer captures the body
     * verbatim (including {expr} placeholders); parser desugars to
     * format(template, expr1, expr2, ...). Single token type so we
     * don't have to thread state across multiple lex calls. */
    TOK_FSTRING,
    TOK_WITH,       /* with — error-chain construct (Elixir-shaped) */
} tok_type_t;

typedef struct {
    tok_type_t type;
    /* 8192 so multi-KB string literals (e.g. system prompts) survive the
     * lexer. Must stay >= node_t.sval[] in swarmrt_lang.h, which holds the
     * parsed literal. By-value buffer (no heap) keeps the fuzzer happy. */
    char text[8192];
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
    {"spawn_monitor", TOK_SPAWN_MONITOR},
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
    {"with",      TOK_WITH},
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
    {"let",       TOK_LET},
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
        int start_line = l->line;
        ladv(l); int i = 0;
        while (lpeek(l) && lpeek(l) != '"' && i < (int)sizeof(t.text) - 2) {
            if (lpeek(l) == '\\') {
                ladv(l);
                /* A backslash as the final byte (unterminated string ending
                 * in '\') would make the next ladv() step past the NUL
                 * terminator — the lexer is NUL-bounded, so reading beyond
                 * it is a heap overflow. Stop here instead. */
                if (!lpeek(l)) break;
                char esc = ladv(l);
                switch (esc) {
                    case 'n': t.text[i++] = '\n'; break;
                    case 't': t.text[i++] = '\t'; break;
                    case 'r': t.text[i++] = '\r'; break;
                    case 'e': t.text[i++] = '\x1b'; break; /* ANSI escape */
                    case '0': t.text[i++] = '\0'; break;
                    case '#': t.text[i++] = '\x01'; break; /* sentinel: escaped # */
                    default:  t.text[i++] = esc; break;
                }
            }
            else t.text[i++] = ladv(l);
        }
        t.text[i] = 0;
        /* If we stopped because the buffer filled (string still open, more
         * non-quote bytes ahead), emit a CLEAN diagnostic citing the true
         * start line instead of letting the parser fabricate "expected }"
         * from the truncated tail. Halt the lexer with TOK_EOF so parsing
         * stops right after this message rather than cascading. */
        if (i >= (int)sizeof(t.text) - 2 && lpeek(l) && lpeek(l) != '"') {
            fprintf(stderr,
                "Parse error: line %d: string literal exceeds %d bytes \xe2\x80\x94 "
                "split it with ++ or load it with file_read\n",
                start_line, (int)sizeof(t.text) - 2);
            t.type = TOK_EOF; t.text[0] = 0;
            return t;
        }
        if (lpeek(l) == '"') ladv(l);
        t.type = TOK_STRING;
        t.line = start_line;
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

    /* f-string: f"hello {name}" — desugared to "hello #{name}" so the
     * existing #{expr} interpolation handler does the work. Lexer
     * rewrites top-level `{` into `#{` while scanning. Inside an
     * embedded expression we track brace depth + inner-string state so
     * `f"name={get_name(\"key\")}"` parses correctly. After scanning
     * the body we emit TOK_STRING — the parser's parse_interp_string
     * picks it up unchanged. */
    if (c == 'f' && lpeek2(l) == '"') {
        ladv(l); /* skip f */
        ladv(l); /* skip opening " */
        int i = 0;
        int brace_depth = 0;
        int in_inner_str = 0;
        while (lpeek(l) && i < (int)sizeof(t.text) - 4) {
            char ch = lpeek(l);
            if (in_inner_str) {
                if (ch == '\\') {
                    t.text[i++] = ladv(l);
                    if (lpeek(l) && i < (int)sizeof(t.text) - 4)
                        t.text[i++] = ladv(l);
                    continue;
                }
                if (ch == '"') in_inner_str = 0;
                t.text[i++] = ladv(l);
                continue;
            }
            if (ch == '"') {
                if (brace_depth == 0) break; /* end of f-string */
                in_inner_str = 1;
                t.text[i++] = ladv(l);
                continue;
            }
            if (ch == '{' && brace_depth == 0) {
                /* `{{` is an escaped literal brace, NOT interpolation — so an
                 * agent can compose tool source (full of `{ }`) with an
                 * f-string. Emit one literal `{`, consume both. */
                if (lpeek2(l) == '{') { ladv(l); ladv(l); t.text[i++] = '{'; continue; }
                /* Top-level `{` — rewrite as `#{` so the existing
                 * #{expr} interpolation kicks in. */
                ladv(l);
                t.text[i++] = '#';
                t.text[i++] = '{';
                brace_depth = 1;
                continue;
            }
            if (ch == '{') { brace_depth++; t.text[i++] = ladv(l); continue; }
            if (ch == '}') {
                /* `}}` at top level is an escaped literal `}` (mirrors `{{`). */
                if (brace_depth == 0 && lpeek2(l) == '}') { ladv(l); ladv(l); t.text[i++] = '}'; continue; }
                if (brace_depth > 0) brace_depth--;
                t.text[i++] = ladv(l);
                continue;
            }
            if (ch == '\\') {
                ladv(l); /* consume '\' */
                /* A trailing backslash at end-of-source (`f"...\` then NUL)
                 * must NOT consume the NUL — ladv() advances unconditionally,
                 * so a second ladv() here would step `pos` PAST the terminator
                 * and the next lpeek() reads out of bounds (the parse fuzzer
                 * hits exactly this on a truncated f-string). Stop the scan. */
                if (!lpeek(l)) break;
                char esc = ladv(l);
                switch (esc) {
                    case 'n': t.text[i++] = '\n'; break;
                    case 't': t.text[i++] = '\t'; break;
                    case 'r': t.text[i++] = '\r'; break;
                    case 'e': t.text[i++] = '\x1b'; break;
                    case '"': t.text[i++] = '"'; break;
                    case '\\': t.text[i++] = '\\'; break;
                    case '#': t.text[i++] = '\x01'; break; /* sentinel: escaped # */
                    default:  t.text[i++] = esc; break;
                }
                continue;
            }
            t.text[i++] = ladv(l);
        }
        t.text[i] = 0;
        if (lpeek(l) == '"') ladv(l);
        t.type = TOK_STRING; /* parse_interp_string handles #{...} */
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
    if (c == '<' && lpeek2(l) == '-') { ladv(l); ladv(l); t.type = TOK_LARROW; strcpy(t.text, "<-"); return t; }
    if (c == '>' && lpeek2(l) == '=') { ladv(l); ladv(l); t.type = TOK_GE; strcpy(t.text, ">="); return t; }
    if (c == '|' && lpeek2(l) == '>') { ladv(l); ladv(l); t.type = TOK_PIPE; strcpy(t.text, "|>"); return t; }
    if (c == '+' && lpeek2(l) == '+') { ladv(l); ladv(l); t.type = TOK_CONCAT; strcpy(t.text, "++"); return t; }
    if (c == '&' && lpeek2(l) == '&') { ladv(l); ladv(l); t.type = TOK_AND; strcpy(t.text, "&&"); return t; }
    if (c == '|' && lpeek2(l) == '|') { ladv(l); ladv(l); t.type = TOK_OR; strcpy(t.text, "||"); return t; }
    if (c == '.' && lpeek2(l) == '.') { ladv(l); ladv(l); t.type = TOK_DOTDOT; strcpy(t.text, ".."); return t; }
    /* `::` — not a sw operator. Lexed as a single TOK_COLON whose text is
     * "::" so the parser can give the Elixir/Haskell cons reflex a precise
     * hint instead of a bare "unexpected ':'". */
    if (c == ':' && lpeek2(l) == ':') { ladv(l); ladv(l); t.type = TOK_COLON; strcpy(t.text, "::"); return t; }
    /* `%{` is a distinct token from bare `%` — needed so the parser
     * can tell map-literal start (`expr → %{...}`) from binary modulo
     * (`expr % expr`) at statement boundaries. Without this split,
     * `else { 0.2 } %{...}` parsed as `(else …) % {...}` since `%` is
     * a valid binary operator. */
    if (c == '%' && lpeek2(l) == '{') {
        ladv(l); /* consume %; { stays for the parser */
        t.type = TOK_MAP_OPEN; strcpy(t.text, "%{");
        return t;
    }

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

/* Per-parse AST-node budget. EVERY node (mknode + node_dup) is counted;
 * par_init resets the counter, and par_primary aborts the parse with a
 * clean error once it is exceeded. This is the O(1) backstop against any
 * pathological parser loop — a malformed `.sw` that drives a
 * non-advancing or unbounded loop (found by `make fuzz`: a single mutated
 * input allocated ~16 GB of nodes and OOM-killed swc) now fails parsing
 * instead of taking the host down. 4M nodes ≈ a 100k+-line program; far
 * beyond any real source, well under a memory problem (~256 MB of node
 * structs). Thread-local: parsers run per-thread (tool registry, LSP). */
#define SW_PARSE_MAX_NODES 4000000
static __thread long g_parse_node_count;

static node_t *mknode(node_type_t type, int line) {
    g_parse_node_count++;
    node_t *n = calloc(1, sizeof(node_t));
    n->type = type; n->line = line;
    return n;
}

static void node_free(node_t *n) {
    if (!n) return;
    switch (n->type) {
    case N_MODULE:
        for (int i = 0; i < n->v.mod.nfuns; i++) node_free(n->v.mod.funs[i]);
        free(n->v.mod.funs);
        for (int i = 0; i < n->v.mod.nglobals; i++) node_free(n->v.mod.globals[i].val);
        break;
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
        node_free(n->v.recv.after_body);
        node_free(n->v.recv.after_expr);
        break;
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
    case N_LIST_COMP:
        node_free(n->v.lcomp.iter);
        node_free(n->v.lcomp.body);
        if (n->v.lcomp.guard) node_free(n->v.lcomp.guard);
        break;
    case N_RANGE: node_free(n->v.range.from); node_free(n->v.range.to); break;
    case N_TRY: node_free(n->v.trycatch.body); node_free(n->v.trycatch.catch_body); break;
    case N_LIST_CONS: node_free(n->v.cons.head); node_free(n->v.cons.tail); break;
    case N_CASE:
        node_free(n->v.casex.subject);
        for (int i = 0; i < n->v.casex.nclauses; i++) node_free(n->v.casex.clauses[i]);
        free(n->v.casex.clauses);
        break;
    default: break;
    }
    free(n);
}

/* Deep-copy an AST subtree. Mirrors node_free's structure exactly (every
 * child node_free recurses into is dup'd here). Used by the `with` desugar,
 * which threads a single `else` handler into N generated `case` arms — each
 * arm needs its OWN copy so the later node_free doesn't double-free a shared
 * subtree. Leaf scalars (ival/fval/sval and all char[] fields) are carried by
 * the flat `*out = *n` struct copy; we then re-dup the pointer children. */
static node_t *node_dup(node_t *n) {
    if (!n) return NULL;
    g_parse_node_count++;
    node_t *out = calloc(1, sizeof(node_t));
    *out = *n;  /* copies scalars + char[] fields + (stale) child pointers */
    switch (n->type) {
    case N_MODULE:
        out->v.mod.funs = malloc(sizeof(node_t*) * (n->v.mod.nfuns ? n->v.mod.nfuns : 1));
        for (int i = 0; i < n->v.mod.nfuns; i++) out->v.mod.funs[i] = node_dup(n->v.mod.funs[i]);
        for (int i = 0; i < n->v.mod.nglobals; i++) out->v.mod.globals[i].val = node_dup(n->v.mod.globals[i].val);
        break;
    case N_FUN:
        out->v.fun.body = node_dup(n->v.fun.body);
        for (int i = 0; i < n->v.fun.nparams; i++) out->v.fun.defaults[i] = node_dup(n->v.fun.defaults[i]);
        break;
    case N_BLOCK:
        out->v.block.stmts = malloc(sizeof(node_t*) * (n->v.block.nstmts ? n->v.block.nstmts : 1));
        for (int i = 0; i < n->v.block.nstmts; i++) out->v.block.stmts[i] = node_dup(n->v.block.stmts[i]);
        break;
    case N_ASSIGN: out->v.assign.value = node_dup(n->v.assign.value); break;
    case N_CALL:
        out->v.call.func = node_dup(n->v.call.func);
        out->v.call.args = malloc(sizeof(node_t*) * (n->v.call.nargs ? n->v.call.nargs : 1));
        for (int i = 0; i < n->v.call.nargs; i++) out->v.call.args[i] = node_dup(n->v.call.args[i]);
        break;
    case N_SPAWN: out->v.spawn.expr = node_dup(n->v.spawn.expr); break;
    case N_SEND: out->v.send.to = node_dup(n->v.send.to); out->v.send.msg = node_dup(n->v.send.msg); break;
    case N_RECEIVE:
        out->v.recv.clauses = malloc(sizeof(node_t*) * (n->v.recv.nclauses ? n->v.recv.nclauses : 1));
        for (int i = 0; i < n->v.recv.nclauses; i++) out->v.recv.clauses[i] = node_dup(n->v.recv.clauses[i]);
        out->v.recv.after_body = node_dup(n->v.recv.after_body);
        out->v.recv.after_expr = node_dup(n->v.recv.after_expr);
        break;
    case N_CLAUSE:
        out->v.clause.pattern = node_dup(n->v.clause.pattern);
        out->v.clause.guard = node_dup(n->v.clause.guard);
        out->v.clause.body = node_dup(n->v.clause.body);
        break;
    case N_IF: out->v.iff.cond = node_dup(n->v.iff.cond); out->v.iff.then_b = node_dup(n->v.iff.then_b); out->v.iff.else_b = node_dup(n->v.iff.else_b); break;
    case N_BINOP: out->v.binop.left = node_dup(n->v.binop.left); out->v.binop.right = node_dup(n->v.binop.right); break;
    case N_UNARY: out->v.unary.operand = node_dup(n->v.unary.operand); break;
    case N_PIPE: out->v.pipe.val = node_dup(n->v.pipe.val); out->v.pipe.func = node_dup(n->v.pipe.func); break;
    case N_TUPLE: case N_LIST:
        out->v.coll.items = malloc(sizeof(node_t*) * (n->v.coll.count ? n->v.coll.count : 1));
        for (int i = 0; i < n->v.coll.count; i++) out->v.coll.items[i] = node_dup(n->v.coll.items[i]);
        break;
    case N_MAP:
        out->v.map.keys = malloc(sizeof(node_t*) * (n->v.map.count ? n->v.map.count : 1));
        out->v.map.vals = malloc(sizeof(node_t*) * (n->v.map.count ? n->v.map.count : 1));
        for (int i = 0; i < n->v.map.count; i++) { out->v.map.keys[i] = node_dup(n->v.map.keys[i]); out->v.map.vals[i] = node_dup(n->v.map.vals[i]); }
        break;
    case N_FOR: out->v.forloop.iter = node_dup(n->v.forloop.iter); out->v.forloop.body = node_dup(n->v.forloop.body); break;
    case N_LIST_COMP:
        out->v.lcomp.iter = node_dup(n->v.lcomp.iter);
        out->v.lcomp.body = node_dup(n->v.lcomp.body);
        out->v.lcomp.guard = node_dup(n->v.lcomp.guard);
        break;
    case N_RANGE: out->v.range.from = node_dup(n->v.range.from); out->v.range.to = node_dup(n->v.range.to); break;
    case N_TRY: out->v.trycatch.body = node_dup(n->v.trycatch.body); out->v.trycatch.catch_body = node_dup(n->v.trycatch.catch_body); break;
    case N_LIST_CONS: out->v.cons.head = node_dup(n->v.cons.head); out->v.cons.tail = node_dup(n->v.cons.tail); break;
    case N_CASE:
        out->v.casex.subject = node_dup(n->v.casex.subject);
        out->v.casex.clauses = malloc(sizeof(node_t*) * (n->v.casex.nclauses ? n->v.casex.nclauses : 1));
        for (int i = 0; i < n->v.casex.nclauses; i++) out->v.casex.clauses[i] = node_dup(n->v.casex.clauses[i]);
        break;
    default: break;  /* leaf nodes (N_INT/N_FLOAT/N_STRING/N_ATOM/N_IDENT/N_SELF): scalars already copied */
    }
    return out;
}

/* =========================================================================
 * Parser
 * ========================================================================= */

typedef struct {
    lex_t lex;
    tok_t cur;
    int err;
    char errmsg[256];
    /* Expression-nesting guard: the recursive-descent chain
     * (par_expr -> ... -> par_primary -> par_expr) consumes C stack per
     * nesting level, and adversarial input like ((((((... overflowed it
     * (found by make fuzz, first run, ASAN stack-overflow in
     * par_primary). 500 levels is far beyond any real program and well
     * inside a few hundred KB of stack. */
    int depth;
} par_t;

static void par_init(par_t *p, const char *src) {
    lex_init(&p->lex, src);
    p->cur = lnext(&p->lex);
    p->err = 0; p->errmsg[0] = 0;
    p->depth = 0;
    g_parse_node_count = 0;   /* reset the per-parse node budget */
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
    /* Don't clobber a more specific error already raised upstream (e.g.
     * the `return <expr>` guard in par_stmt). The first diagnostic is the
     * one the user needs; a cascading "expected '}'" only adds noise. */
    if (!p->err) {
        snprintf(p->errmsg, sizeof(p->errmsg), "line %d: expected %s, got '%s'",
                 p->cur.line, msg, p->cur.text);
        p->err = 1;
    }
    return p->cur;
}

/* Forward declarations */
static node_t *par_expr(par_t *p);
static node_t *par_primary_inner(par_t *p);
static node_t *par_stmt(par_t *p);
static node_t *par_block(par_t *p);

/* Restore \x01 sentinels back to # */
static void restore_hash_sentinels(char *s) {
    for (; *s; s++) if (*s == '\x01') *s = '#';
}

/* String interpolation: "hello #{name}!" -> concat chain */
static node_t *parse_interp_string(const char *text, int line, int parent_depth) {
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
        /* Inherit the caller's nesting budget: each interpolation level
         * previously got a FRESH parser (depth=0), so nested f-strings
         * recursed through unbounded new parser instances. */
        ep.depth = parent_depth;
        /* The sub-parser lexes the extracted text from scratch, so every
         * node it makes carries line 1 — and any diagnostic against an
         * interpolated expression ("unknown function ... at line 1")
         * pointed at the top of the file. Seed it with the f-string's
         * real line. */
        ep.lex.line = line;
        ep.cur.line = line;
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

/* Parse an anonymous-function tail `( params ) { body }`, with the
 * introducer keyword (`fun` or `fn`) already consumed and the parser
 * positioned at the opening `(`. Shared by the `fun` keyword path and
 * the `fn(params){body}` primary (see par_fn_lambda_ahead). `line` is the
 * keyword's source line, used for the N_FUN node. */
static node_t *par_lambda_tail(par_t *p, int line) {
    par_adv(p); /* ( */
    node_t *fn = mknode(N_FUN, line);
    fn->v.fun.name[0] = '\0'; /* empty name = anonymous */
    fn->v.fun.nparams = 0;
    if (p->cur.type != TOK_RPAREN) {
        do {
            tok_t param = par_expect(p, TOK_IDENT, "parameter");
            /* params[] is a fixed [16][128] buffer (swarmrt_lang.h). Cap at 16
             * so a long arg list can't overrun the node — the fuzzer hits this. */
            if (fn->v.fun.nparams < 16) {
                strncpy(fn->v.fun.params[fn->v.fun.nparams], param.text, 127);
                fn->v.fun.nparams++;
            } else if (!p->err) {
                snprintf(p->errmsg, sizeof(p->errmsg),
                         "line %d: too many lambda parameters (max 16)", line);
                p->err = 1;
            }
        } while (par_match(p, TOK_COMMA));
    }
    par_expect(p, TOK_RPAREN, "')'");
    par_expect(p, TOK_LBRACE, "'{'");
    fn->v.fun.body = par_block(p);
    par_expect(p, TOK_RBRACE, "'}'");
    return fn;
}

/* Speculative lookahead: with the parser positioned just after a bare `fn`
 * identifier (i.e. p->cur is the token following `fn`), decide whether this
 * is the lambda shape `fn ( params ) {` rather than a call/identifier named
 * `fn`. The disambiguator is purely structural: a `(` whose matching `)` is
 * immediately followed by `{`. This is what lets `fn` remain a perfectly good
 * variable/parameter name (the stdlib uses `fn` as a callback param all over)
 * while ALSO making `fn(x){ ... }` a first-class lambda literal in any
 * expression position. Restores the parser to its entry state before
 * returning, so it has no side effects. */
static int par_fn_lambda_ahead(par_t *p) {
    if (p->cur.type != TOK_LPAREN) return 0;
    /* Save full lexer + current token; lex_t/tok_t are by-value POD. */
    lex_t saved_lex = p->lex;
    tok_t saved_cur = p->cur;
    int is_lambda = 0;
    int depth = 0;
    /* Walk balanced parens using the real tokenizer (handles strings,
     * comments, nested parens correctly). Bail on EOF. */
    for (;;) {
        if (p->cur.type == TOK_EOF) break;
        if (p->cur.type == TOK_LPAREN) depth++;
        else if (p->cur.type == TOK_RPAREN) {
            depth--;
            if (depth == 0) {
                par_adv(p); /* consume the matching ')' */
                is_lambda = (p->cur.type == TOK_LBRACE);
                break;
            }
        }
        par_adv(p);
    }
    p->lex = saved_lex;
    p->cur = saved_cur;
    return is_lambda;
}

/* Speculative lookahead: with the parser positioned at a `{` that opens a
 * case-arm / receive-clause body (after `->`), decide whether that brace is a
 * TUPLE LITERAL (`{'error', why}`, `{a, b}`) rather than a statement BLOCK
 * (`{ x = ... ; x }`).
 *
 * The disambiguator is unambiguous and conservative: a `{ ... }` is a tuple
 * iff a comma appears at the TOP LEVEL of this very brace — i.e. at this
 * brace's depth, with all paren / bracket / inner-brace / map depths zero —
 * before its matching `}`. A statement block can never carry a top-level
 * comma (blocks separate statements with `;`/newlines only; any top-level
 * comma in a block is, and always was, a hard parse error), so this rule is a
 * pure superset: every input that parses as a block today still does, and the
 * shape that used to error (`-> {'atom', ...}`) now parses as the tuple the
 * user clearly meant. `%{...}` map literals and call-arg commas live at deeper
 * depth and are correctly skipped (TOK_MAP_OPEN opens a brace level too).
 *
 * Assumes p->cur is the opening TOK_LBRACE. Restores parser state before
 * returning, so it has no side effects. */
static int par_arm_brace_is_tuple(par_t *p) {
    if (p->cur.type != TOK_LBRACE) return 0;
    lex_t saved_lex = p->lex;
    tok_t saved_cur = p->cur;
    int is_tuple = 0;
    int brace = 0, paren = 0, bracket = 0;
    for (;;) {
        if (p->cur.type == TOK_EOF) break;
        switch (p->cur.type) {
        case TOK_LBRACE: case TOK_MAP_OPEN: brace++; break;
        case TOK_RBRACE:
            brace--;
            if (brace == 0) goto done;  /* matched our `}` — no top-level comma */
            break;
        case TOK_LPAREN:   paren++;   break;
        case TOK_RPAREN:   paren--;   break;
        case TOK_LBRACKET: bracket++; break;
        case TOK_RBRACKET: bracket--; break;
        case TOK_COMMA:
            /* Top level of the brace we opened: this `{...}` is a tuple. */
            if (brace == 1 && paren == 0 && bracket == 0) { is_tuple = 1; goto done; }
            break;
        default: break;
        }
        par_adv(p);
    }
done:
    p->lex = saved_lex;
    p->cur = saved_cur;
    return is_tuple;
}

/* Primary expression */
/* 128: beyond any real program (128 nested parens/calls), and small
 * enough that even ASAN-inflated parser frames (~30KB/cycle observed)
 * stay well inside an 8MB stack. */
#define SW_PARSE_MAX_DEPTH 128

static int interp_stack_near_limit(void);   /* defined with the interpreter below */

static node_t *par_primary(par_t *p) {
    /* Node-budget backstop FIRST — before the err short-circuit below — so
     * a pathological loop that keeps re-entering par_primary while p->err
     * is already set (the receive-clause spin) is still bounded. par_primary
     * is on the path of every expression; once we force the message the
     * enclosing loops drain on their !err guards. */
    if (g_parse_node_count > SW_PARSE_MAX_NODES) {
        if (!p->err) {
            snprintf(p->errmsg, sizeof(p->errmsg),
                     "line %d: program too large (parse exceeded %d AST nodes)",
                     p->cur.line, SW_PARSE_MAX_NODES);
            p->err = 1;
        }
        return mknode(N_INT, p->cur.line);
    }
    /* After ANY parse error, stop descending — callers don't all check
     * p->err and could re-enter on the unconsumed token, growing C stack
     * even with the depth budget exhausted-and-decremented. */
    if (p->err) return mknode(N_INT, p->cur.line);
    /* Two guards: a fixed nesting budget AND the real-headroom probe —
     * parser frames are enormous (tok_t carries an 8KB text buffer and
     * gets copied per frame; ASAN multiplies further), so a count alone
     * can lie about safety on small stacks. */
    if (++p->depth > SW_PARSE_MAX_DEPTH || interp_stack_near_limit()) {
        if (!p->err) {
            snprintf(p->errmsg, sizeof(p->errmsg),
                     "line %d: expression nesting too deep (max %d levels)",
                     p->cur.line, SW_PARSE_MAX_DEPTH);
            p->err = 1;
        }
        p->depth--;
        return mknode(N_INT, p->cur.line);   /* placeholder; parse failed */
    }
    node_t *_r = par_primary_inner(p);
    p->depth--;
    return _r;
}

static node_t *par_primary_inner(par_t *p) {
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
        node_t *interp = parse_interp_string(t.text, t.line, p->depth);
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
        /* `fn(params){ body }` — a Rust/Gleam-shaped lambda literal, the form
         * LLMs reach for. `fn` is NOT a reserved keyword (the stdlib uses it as
         * an ordinary callback parameter name), so we only treat it as a lambda
         * introducer when the structural lookahead sees `fn ( ... ) {`. Anything
         * else (`fn`, `fn(x)`) falls through to the normal ident/call path. */
        if (strcmp(t.text, "fn") == 0 && par_fn_lambda_ahead(p)) {
            return par_lambda_tail(p, t.line);
        }
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
            return par_lambda_tail(p, t.line);
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

    /* Case expression: case <subject> { pat -> body ; pat when g -> body ; ... } */
    if (t.type == TOK_CASE) {
        par_adv(p);
        node_t *cx = mknode(N_CASE, t.line);
        cx->v.casex.subject = par_expr(p);
        cx->v.casex.clauses = NULL;
        cx->v.casex.nclauses = 0;
        par_expect(p, TOK_LBRACE, "'{'");
        while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !p->err) {
            /* Tolerate `;` between arms (same way par_block does). */
            while (p->cur.type == TOK_SEMI) par_adv(p);
            if (p->cur.type == TOK_RBRACE || p->cur.type == TOK_EOF) break;

            node_t *cl = mknode(N_CLAUSE, p->cur.line);
            cl->v.clause.pattern = par_expr(p);
            cl->v.clause.guard = NULL;
            if (p->cur.type == TOK_WHEN) {
                par_adv(p);
                cl->v.clause.guard = par_expr(p);
            }
            par_expect(p, TOK_ARROW, "'->'");

            /* Body: collect statements up to next pattern (terminated by ->),
             * `;`, or the closing `}`. Same approach as receive arms.
             *
             * Special case: if the body starts with `{`, treat it as a
             * block delimiter (parsed via par_block) rather than the
             * default per-statement loop. Without this special-case the
             * default loop would invoke par_expr on `{` which parses it
             * as a tuple literal — that breaks any user who writes
             * `_ -> { x = ... ; x }` expecting an explicit block.
             * No existing code uses `_ -> {tuple}` as a clause body
             * (users write the tuple bare), so this is purely additive.
             *
             * BUT a bare TUPLE result (`_ -> {'error', why}`) is the form
             * LLMs reach for, and it has a top-level comma — which a block
             * can never have. par_arm_brace_is_tuple distinguishes the two
             * structurally, so the tuple parses as an expression while the
             * `{ stmt ; stmt }` block keeps its block parse. */
            node_t *body;
            if (p->cur.type == TOK_LBRACE && par_arm_brace_is_tuple(p)) {
                body = par_expr(p);   /* tuple literal result */
            } else if (p->cur.type == TOK_LBRACE) {
                par_adv(p);  /* consume '{' */
                body = par_block(p);
                par_expect(p, TOK_RBRACE, "'}'");
            } else {
                body = mknode(N_BLOCK, p->cur.line);
                body->v.block.stmts = NULL;
                body->v.block.nstmts = 0;
                while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !p->err) {
                    while (p->cur.type == TOK_SEMI) par_adv(p);
                    if (p->cur.type == TOK_RBRACE || p->cur.type == TOK_EOF) break;
                    lex_t save_lex = p->lex;
                    tok_t save_cur = p->cur;
                    int save_err = p->err;
                    node_t *expr = par_stmt(p);
                    /* Backtrack if the just-parsed expression turned out to
                     * be the NEXT clause's pattern. Two signatures: `pat ->`
                     * and `pat when guard ->`. We peek for either TOK_ARROW
                     * or TOK_WHEN. (Same as par_receive's arm body.) */
                    if (p->cur.type == TOK_ARROW || p->cur.type == TOK_WHEN) {
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
            }
            /* A tuple-literal body is already the expression; only unwrap
             * single-statement BLOCK bodies (the `body->v.block` union is
             * valid only for N_BLOCK). */
            if (body->type == N_BLOCK && body->v.block.nstmts == 1) {
                cl->v.clause.body = body->v.block.stmts[0];
                free(body->v.block.stmts);
                free(body);
            } else {
                cl->v.clause.body = body;
            }
            cx->v.casex.nclauses++;
            cx->v.casex.clauses = realloc(cx->v.casex.clauses,
                sizeof(node_t*) * cx->v.casex.nclauses);
            cx->v.casex.clauses[cx->v.casex.nclauses-1] = cl;
        }
        par_expect(p, TOK_RBRACE, "'}'");
        return cx;
    }

    /* `with` error-chain (Elixir-shaped):
     *
     *   with p1 <- e1, p2 <- e2, ... { B } else { o -> E }
     *
     * Evaluate e1, match p1; on match continue to the next bind, on mismatch
     * bind the failing value (to `o`) and run the else arm; the happy path
     * runs B. This is the load-bearing control-flow for fallible multi-step
     * tool/LLM pipelines (decode → check → extract → validate → call).
     *
     * DESUGAR — pure parse-time rewrite into nested `case`, so codegen and the
     * interpreter need ZERO changes and interp==compiled by construction:
     *
     *   case e1 { p1 -> case e2 { p2 -> B ; o -> E } ; o -> E }
     *
     * Built inside-out: innermost = case eN { pN -> B ; <else> }, then wrap.
     * Each generated fallback arm gets its OWN node_dup() of the (o, E) pair
     * so the AST stays a tree (no shared subtree → no double-free). */
    if (t.type == TOK_WITH) {
        par_adv(p);
        /* Collect binds: pat <- expr (comma-separated). Bounded — a long
         * with-chain is a code smell; this also keeps the stack arrays flat. */
        node_t *pats[32]; node_t *exprs[32]; int nbinds = 0;
        while (1) {
            if (nbinds >= 32) {
                if (!p->err) {
                    snprintf(p->errmsg, sizeof(p->errmsg),
                             "line %d: too many `with` binds (max 32)", t.line);
                    p->err = 1;
                }
                break;
            }
            node_t *pat = par_expr(p);
            par_expect(p, TOK_LARROW, "'<-'");
            node_t *ex = par_expr(p);
            pats[nbinds] = pat; exprs[nbinds] = ex; nbinds++;
            if (!par_match(p, TOK_COMMA)) break;
        }
        par_expect(p, TOK_LBRACE, "'{'");
        node_t *body = par_block(p);
        par_expect(p, TOK_RBRACE, "'}'");
        par_expect(p, TOK_ELSE, "'else'");
        par_expect(p, TOK_LBRACE, "'{'");
        /* else arms: one or more `pat [when guard] -> body` clauses, exactly
         * like case arms (Elixir's with/else shape — a single-arm else was
         * accepted before, but a second arm was a parse error and models
         * write multi-arm else constantly). Each non-matching bind value
         * falls through these arms in order. */
        node_t *e_pats[16]; node_t *e_guards[16]; node_t *e_bodies[16];
        int nelse = 0;
        while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !p->err) {
            while (p->cur.type == TOK_SEMI) par_adv(p);
            if (p->cur.type == TOK_RBRACE || p->cur.type == TOK_EOF) break;
            if (nelse >= 16) {
                snprintf(p->errmsg, sizeof(p->errmsg),
                         "line %d: too many `else` arms (max 16)", t.line);
                p->err = 1;
                break;
            }
            e_pats[nelse] = par_expr(p);
            e_guards[nelse] = NULL;
            if (p->cur.type == TOK_WHEN) {
                par_adv(p);
                e_guards[nelse] = par_expr(p);
            }
            par_expect(p, TOK_ARROW, "'->'");
            /* Arm body: same tri-shape as a case arm — bare tuple result,
             * explicit { block }, or statements up to the next `pat ->` /
             * `pat when` / closing brace. */
            node_t *ebody;
            if (p->cur.type == TOK_LBRACE && par_arm_brace_is_tuple(p)) {
                ebody = par_expr(p);
            } else if (p->cur.type == TOK_LBRACE) {
                par_adv(p);
                ebody = par_block(p);
                par_expect(p, TOK_RBRACE, "'}'");
            } else {
                ebody = mknode(N_BLOCK, p->cur.line);
                ebody->v.block.stmts = NULL;
                ebody->v.block.nstmts = 0;
                while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !p->err) {
                    while (p->cur.type == TOK_SEMI) par_adv(p);
                    if (p->cur.type == TOK_RBRACE || p->cur.type == TOK_EOF) break;
                    lex_t save_lex = p->lex;
                    tok_t save_cur = p->cur;
                    int save_err = p->err;
                    node_t *expr = par_stmt(p);
                    if (p->cur.type == TOK_ARROW || p->cur.type == TOK_WHEN) {
                        p->lex = save_lex;
                        p->cur = save_cur;
                        p->err = save_err;
                        node_free(expr);
                        break;
                    }
                    ebody->v.block.nstmts++;
                    ebody->v.block.stmts = realloc(ebody->v.block.stmts,
                        sizeof(node_t*) * ebody->v.block.nstmts);
                    ebody->v.block.stmts[ebody->v.block.nstmts - 1] = expr;
                }
            }
            e_bodies[nelse] = ebody;
            nelse++;
        }
        par_expect(p, TOK_RBRACE, "'}'");
        if (!p->err && nelse == 0) {
            snprintf(p->errmsg, sizeof(p->errmsg),
                     "line %d: `else` needs at least one `pattern -> body` arm", t.line);
            p->err = 1;
        }

        if (p->err) {
            for (int i = 0; i < nbinds; i++) { node_free(pats[i]); node_free(exprs[i]); }
            node_free(body);
            for (int i = 0; i < nelse; i++) {
                node_free(e_pats[i]);
                if (e_guards[i]) node_free(e_guards[i]);
                node_free(e_bodies[i]);
            }
            return mknode(N_INT, t.line);
        }

        /* Defensive: an empty bind list (`with { B } else {...}`) is a parse
         * error upstream (par_expr would have failed on `{`), but guard anyway
         * so the inside-out loop below always has an inner = B to wrap. */
        node_t *inner = body;
        for (int i = nbinds - 1; i >= 0; i--) {
            node_t *cx = mknode(N_CASE, exprs[i]->line);
            cx->v.casex.subject = exprs[i];
            cx->v.casex.nclauses = 1 + nelse;
            cx->v.casex.clauses = malloc(sizeof(node_t*) * (1 + nelse));
            /* happy arm: pat_i -> inner */
            node_t *ok = mknode(N_CLAUSE, pats[i]->line);
            ok->v.clause.pattern = pats[i];
            ok->v.clause.guard = NULL;
            ok->v.clause.body = inner;
            cx->v.casex.clauses[0] = ok;
            /* fallback arms: every else clause, in order (fresh copies per
             * bind level — each desugared case owns its own subtree). */
            for (int j = 0; j < nelse; j++) {
                node_t *fb = mknode(N_CLAUSE, e_pats[j]->line);
                fb->v.clause.pattern = node_dup(e_pats[j]);
                fb->v.clause.guard = e_guards[j] ? node_dup(e_guards[j]) : NULL;
                fb->v.clause.body = node_dup(e_bodies[j]);
                cx->v.casex.clauses[1 + j] = fb;
            }
            inner = cx;
        }
        /* The originals were only templates for the per-level dups. */
        for (int i = 0; i < nelse; i++) {
            node_free(e_pats[i]);
            if (e_guards[i]) node_free(e_guards[i]);
            node_free(e_bodies[i]);
        }
        return inner;
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

    /* Spawn expression (spawn / spawn_monitor — same node, monitor flag) */
    if (t.type == TOK_SPAWN || t.type == TOK_SPAWN_MONITOR) {
        par_adv(p);
        par_expect(p, TOK_LPAREN, "'('");
        node_t *sp = mknode(N_SPAWN, t.line);
        sp->v.spawn.expr = par_expr(p);
        sp->v.spawn.monitor = (t.type == TOK_SPAWN_MONITOR) ? 1 : 0;
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

        /* `&& !p->err`: once a clause fails to parse, the cursor stops
         * advancing (par_primary/par_expect short-circuit on err) but is
         * not necessarily on RBRACE/AFTER/EOF — without the err guard the
         * loop spins forever appending empty clauses (a stray token inside
         * a receive arm OOM'd swc; found by `make fuzz`). par_case's clause
         * loop already had this guard; par_receive's did not. */
        while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_AFTER &&
               p->cur.type != TOK_EOF && !p->err) {
            node_t *cl = mknode(N_CLAUSE, p->cur.line);
            cl->v.clause.pattern = par_expr(p);
            cl->v.clause.guard = NULL;
            if (p->cur.type == TOK_WHEN) {
                par_adv(p);
                cl->v.clause.guard = par_expr(p);
            }
            par_expect(p, TOK_ARROW, "'->'");
            /* Body: collect expressions until next clause pattern (expr ->) or }.
             * Special case for leading `{` — see the matching case-arm
             * comment in par_case for why, including the tuple-vs-block
             * disambiguation (`{'reply', x}` is a tuple result, not a block). */
            node_t *body;
            if (p->cur.type == TOK_LBRACE && par_arm_brace_is_tuple(p)) {
                body = par_expr(p);   /* tuple literal result */
            } else if (p->cur.type == TOK_LBRACE) {
                par_adv(p);  /* consume '{' */
                body = par_block(p);
                par_expect(p, TOK_RBRACE, "'}'");
            } else {
                body = mknode(N_BLOCK, p->cur.line);
                body->v.block.stmts = NULL;
                body->v.block.nstmts = 0;
                while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_AFTER && p->cur.type != TOK_EOF && !p->err) {
                    while (p->cur.type == TOK_SEMI) par_adv(p);
                    if (p->cur.type == TOK_RBRACE || p->cur.type == TOK_AFTER || p->cur.type == TOK_EOF) break;
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
            }
            /* Only unwrap single-statement BLOCK bodies (union valid only for
             * N_BLOCK); a tuple-literal body is already the expression. */
            if (body->type == N_BLOCK && body->v.block.nstmts == 1) {
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
                /* Literal-int fast path — folded into after_ms. */
                recv->v.recv.after_ms = (int)timeout_expr->v.ival;
                node_free(timeout_expr);
                recv->v.recv.after_expr = NULL;
            } else {
                /* Dynamic timeout — evaluated at runtime each receive.
                 * Common pattern: `after some_var { ... }`. */
                recv->v.recv.after_ms = -1;
                recv->v.recv.after_expr = timeout_expr;
            }
            /* Both body shapes are accepted: the canonical block form
             * `after MS { body }` and the Erlang-reflex arrow form
             * `after MS -> body` (every receive ARM is `pat -> body`, so
             * models emit the arrow here constantly; rejecting it bought
             * nothing). The arrow body runs to the closing brace of the
             * receive — `after` is necessarily the last clause. */
            if (par_match(p, TOK_ARROW)) {
                recv->v.recv.after_body = par_block(p);
            } else {
                par_expect(p, TOK_LBRACE, "'{'");
                /* Use par_block so multi-statement after bodies work — the
                 * common pattern is `after N { fn() ; loop(...) }` and
                 * par_expr would only consume `fn()`. */
                recv->v.recv.after_body = par_block(p);
                par_expect(p, TOK_RBRACE, "'}'");
            }
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

    /* Map literal: %{key: val, ...} — emitted as TOK_MAP_OPEN by the
     * lexer when `%{` appears as one unit. */
    if (t.type == TOK_MAP_OPEN) {
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
        /* `[h :: t]` is the Elixir/Haskell cons reflex. sw spells cons with
         * a single bar: `[h | t]`. Catch it here with a precise hint. */
        if (p->cur.type == TOK_COLON && strcmp(p->cur.text, "::") == 0) {
            snprintf(p->errmsg, sizeof(p->errmsg),
                     "line %d: use | for cons: [head | tail] (sw has no :: operator)",
                     p->cur.line);
            p->err = 1;
            return first;
        }
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
        if (p->cur.type == TOK_COMMA) {
            /* Could be a plain list `[a, b, c]` OR a multi-element cons
             * `[a, b | rest]`. Gather the comma-separated heads first; if a
             * `|` appears before the closing `]`, build a right-nested cons
             * chain `cons(a, cons(b, rest))` so the existing single-head
             * N_LIST_CONS machinery (pattern_match / eval / codegen, all of
             * which recurse on cons.tail) handles arbitrary arity for free.
             * Otherwise fall through to the plain-list builder below. */
            node_t **heads = malloc(sizeof(node_t*));
            heads[0] = first;
            int nheads = 1;
            while (par_match(p, TOK_COMMA)) {
                /* grow BEFORE writing — the array holds nheads slots, so
                 * writing heads[nheads] needs capacity nheads+1 first
                 * (the pre-grow write was a 1-elem heap-buffer-overflow on
                 * every 2+ element list literal). */
                heads = realloc(heads, sizeof(node_t*) * (nheads + 1));
                heads[nheads] = par_expr(p);
                nheads++;
            }
            if (p->cur.type == TOK_BAR) {
                /* Multi-element cons: [a, b, ... | rest] */
                par_adv(p);
                node_t *rest = par_expr(p);
                par_expect(p, TOK_RBRACKET, "']'");
                node_t *acc = rest;
                for (int i = nheads - 1; i >= 0; i--) {
                    node_t *c = mknode(N_LIST_CONS, t.line);
                    c->v.cons.head = heads[i];
                    c->v.cons.tail = acc;
                    acc = c;
                }
                free(heads);
                return acc;
            }
            /* Plain list: assemble from the gathered heads. */
            par_expect(p, TOK_RBRACKET, "']'");
            node_t *lst = mknode(N_LIST, t.line);
            lst->v.coll.items = heads;
            lst->v.coll.count = nheads;
            return lst;
        }
        if (p->cur.type == TOK_FOR) {
            /* List comprehension: [body for var in iter] or [body for var in iter when guard] */
            par_adv(p); /* consume 'for' */
            tok_t var = par_expect(p, TOK_IDENT, "variable");
            par_expect(p, TOK_IN, "'in'");
            node_t *iter = par_expr(p);
            node_t *guard = NULL;
            if (p->cur.type == TOK_WHEN) {
                par_adv(p); /* consume 'when' */
                guard = par_expr(p);
            }
            par_expect(p, TOK_RBRACKET, "']'");
            node_t *lc = mknode(N_LIST_COMP, t.line);
            strncpy(lc->v.lcomp.var, var.text, 127);
            lc->v.lcomp.iter  = iter;
            lc->v.lcomp.body  = first;
            lc->v.lcomp.guard = guard;
            return lc;
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
    if (t.type == TOK_COLON && strcmp(t.text, "::") == 0)
        snprintf(p->errmsg, sizeof(p->errmsg),
                 "line %d: use | for cons: [head | tail] (sw has no :: operator)", t.line);
    else
        snprintf(p->errmsg, sizeof(p->errmsg), "line %d: unexpected '%s'", t.line, t.text);
    p->err = 1;
    par_adv(p);
    return mknode(N_ATOM, t.line); /* dummy */
}

/* Multiplicative — *, /, and % (modulo). par_primary already consumed
 * any `%{...}` map-literal at expression start, so seeing TOK_PERCENT
 * here always means binary modulo. */
static node_t *par_mul(par_t *p) {
    node_t *left = par_primary(p);
    while (p->cur.type == TOK_STAR ||
           p->cur.type == TOK_SLASH ||
           p->cur.type == TOK_PERCENT) {
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
    /* Tuple-destructuring assignment: `{a, b} = expr` (and `{'ok', v} = ...`
     * muscle memory aside, only plain identifiers and `_` may appear on the
     * left). Desugars at PARSE time into a temp + per-element elem() binds,
     * so the interpreter and codegen get identical semantics for free:
     *
     *     __destruct_N = expr
     *     a = elem(__destruct_N, 0)
     *     b = elem(__destruct_N, 1)
     *     __destruct_N                  <- the whole statement's value
     *
     * A too-short tuple (or a non-tuple) panics through elem's existing
     * out-of-range/type panic; extra trailing elements are tolerated. */
    if (p->cur.type == TOK_LBRACE) {
        lex_t save_lex = p->lex;
        tok_t save_cur = p->cur;
        int save_err = p->err;
        node_t *lhs = par_expr(p);
        if (!p->err && lhs && lhs->type == N_TUPLE && p->cur.type == TOK_ASSIGN) {
            /* Destructurable: identifiers (bind), `_` (skip), and literal
             * atoms/ints/floats/strings (assert-match, Erlang-style — so
             * `{'ok', v} = fetch()` binds v AND panics on {'error', _}). */
            int all_idents = 1;
            for (int i = 0; i < lhs->v.coll.count; i++) {
                node_type_t it = lhs->v.coll.items[i]->type;
                if (it != N_IDENT && it != N_ATOM && it != N_INT &&
                    it != N_FLOAT && it != N_STRING) { all_idents = 0; break; }
            }
            if (all_idents && lhs->v.coll.count > 0) {
                par_adv(p);   /* consume '=' */
                int line = save_cur.line;
                static int dt_ctr = 0;
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "__destruct_%d", dt_ctr++);

                node_t *blk = mknode(N_BLOCK, line);
                blk->v.block.nstmts = 0;
                blk->v.block.stmts = malloc(sizeof(node_t*) * (lhs->v.coll.count + 2));

                node_t *bind = mknode(N_ASSIGN, line);
                strncpy(bind->v.assign.name, tmp, sizeof(bind->v.assign.name) - 1);
                bind->v.assign.value = par_expr(p);
                blk->v.block.stmts[blk->v.block.nstmts++] = bind;

                for (int i = 0; i < lhs->v.coll.count; i++) {
                    node_t *item = lhs->v.coll.items[i];
                    if (item->type == N_IDENT && strcmp(item->v.sval, "_") == 0)
                        continue;
                    /* elem(__destruct_N, i) */
                    node_t *get = mknode(N_CALL, line);
                    node_t *fn_id = mknode(N_IDENT, line);
                    strcpy(fn_id->v.sval, "elem");
                    get->v.call.func = fn_id;
                    get->v.call.args = malloc(sizeof(node_t*) * 2);
                    node_t *src_id = mknode(N_IDENT, line);
                    strncpy(src_id->v.sval, tmp, sizeof(src_id->v.sval) - 1);
                    node_t *idx = mknode(N_INT, line);
                    idx->v.ival = i;
                    get->v.call.args[0] = src_id;
                    get->v.call.args[1] = idx;
                    get->v.call.nargs = 2;
                    if (item->type == N_IDENT) {
                        /* bind: name = elem(tmp, i) */
                        node_t *as = mknode(N_ASSIGN, line);
                        strncpy(as->v.assign.name, item->v.sval,
                                sizeof(as->v.assign.name) - 1);
                        as->v.assign.value = get;
                        blk->v.block.stmts[blk->v.block.nstmts++] = as;
                    } else {
                        /* literal: expect(elem(tmp, i) == lit, msg) — panics
                         * on mismatch, Erlang match semantics. */
                        node_t *cmp = mknode(N_BINOP, line);
                        strcpy(cmp->v.binop.op, "==");
                        cmp->v.binop.left = get;
                        cmp->v.binop.right = node_dup(item);
                        node_t *msg = mknode(N_STRING, line);
                        snprintf(msg->v.sval, sizeof(msg->v.sval),
                                 "destructuring mismatch at element %d", i);
                        node_t *chk = mknode(N_CALL, line);
                        node_t *exp_id = mknode(N_IDENT, line);
                        strcpy(exp_id->v.sval, "expect");
                        chk->v.call.func = exp_id;
                        chk->v.call.args = malloc(sizeof(node_t*) * 2);
                        chk->v.call.args[0] = cmp;
                        chk->v.call.args[1] = msg;
                        chk->v.call.nargs = 2;
                        blk->v.block.stmts[blk->v.block.nstmts++] = chk;
                    }
                }
                node_t *valret = mknode(N_IDENT, line);
                strncpy(valret->v.sval, tmp, sizeof(valret->v.sval) - 1);
                blk->v.block.stmts[blk->v.block.nstmts++] = valret;
                node_free(lhs);
                return blk;
            }
        }
        /* Not a destructuring assignment — backtrack and parse normally. */
        node_free(lhs);
        p->lex = save_lex;
        p->cur = save_cur;
        p->err = save_err;
    }
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
        /* Gleam-style guard for the C/JS/Python `return <expr>` reflex.
         * sw has no `return` keyword — a function's value is its last
         * expression. We only error on the STATEMENT form `return <expr>`
         * (return immediately followed by something that starts an
         * expression). `return` stays a perfectly legal identifier: as a
         * value (`x = return`), an assignment target (`return = 1`), or a
         * call (`return(...)`) it falls through untouched. */
        if (strcmp(id.text, "return") == 0) {
            tok_t nx = p->cur;
            int starts_expr =
                nx.type == TOK_STRING || nx.type == TOK_NUMBER ||
                nx.type == TOK_IDENT  || nx.type == TOK_ATOM   ||
                nx.type == TOK_LBRACKET || nx.type == TOK_MAP_OPEN ||
                nx.type == TOK_FUN    || nx.type == TOK_SELF   ||
                nx.type == TOK_IF     || nx.type == TOK_CASE   ||
                nx.type == TOK_MINUS;
            if (starts_expr) {
                snprintf(p->errmsg, sizeof(p->errmsg),
                         "line %d: sw has no `return` \xe2\x80\x94 the last expression "
                         "in a function is its value (wrap branches so the value falls out)",
                         id.line);
                p->err = 1;
                return mknode(N_INT, id.line);   /* placeholder; parse already failed */
            }
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
        /* Tolerate `;` as a statement separator. Newlines are already
         * whitespace at the lexer level, so block bodies historically
         * only worked with newlines — `if (x) { a ; b }` choked.
         * Consuming any leading semicolons before parsing the next
         * statement makes both function bodies AND if-branch / receive-
         * arm bodies accept either. Pure superset, no existing code
         * regressed (the lexer ate inter-statement newlines before). */
        while (p->cur.type == TOK_SEMI) par_adv(p);
        if (p->cur.type == TOK_RBRACE || p->cur.type == TOK_EOF) break;
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
            /* params[]/defaults[] are fixed [16] buffers (swarmrt_lang.h). Cap
             * at 16 so a long parameter list can't overrun the node — the
             * lambda path (above) guards this; the top-level fun path didn't,
             * and a >16-param fn from LLM-generated source corrupted the heap. */
            if (fn->v.fun.nparams < 16) {
                strncpy(fn->v.fun.params[fn->v.fun.nparams], param.text, 127);
                fn->v.fun.defaults[fn->v.fun.nparams] = NULL;
                if (par_match(p, TOK_ASSIGN)) {
                    fn->v.fun.defaults[fn->v.fun.nparams] = par_expr(p);
                }
                fn->v.fun.nparams++;
            } else {
                if (par_match(p, TOK_ASSIGN)) node_free(par_expr(p)); /* consume, don't store */
                if (!p->err) {
                    snprintf(p->errmsg, sizeof(p->errmsg),
                             "too many function parameters (max 16)");
                    p->err = 1;
                }
            }
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

    /* Module-header clauses: imports and exports may appear in ANY order
     * and may be interleaved. The previous version parsed all imports
     * first and then at most one export; an `export [...]` placed before
     * an `import X` left the import (and, worse, every following function
     * body) unparsed — the function/global loop below starts at the first
     * unconsumed `import`/`export` and bails immediately, silently
     * dropping the whole module body. Loop over both clause kinds until
     * neither appears so order no longer matters. */
    while ((p->cur.type == TOK_IMPORT || p->cur.type == TOK_EXPORT) && !p->err) {
        if (p->cur.type == TOK_IMPORT) {
            par_adv(p);
            tok_t iname = par_expect(p, TOK_IDENT, "module name");
            if (mod->v.mod.nimports < 32)
                strncpy(mod->v.mod.imports[mod->v.mod.nimports++], iname.text, 127);
        } else { /* TOK_EXPORT */
            par_adv(p);
            par_expect(p, TOK_LBRACKET, "'['");
            while (p->cur.type != TOK_RBRACKET && p->cur.type != TOK_EOF) {
                par_adv(p);
                par_match(p, TOK_COMMA);
            }
            par_expect(p, TOK_RBRACKET, "']'");
        }
    }

    /* Functions and module-level globals (let x = literal) */
    while ((p->cur.type == TOK_FUN || p->cur.type == TOK_LET) && !p->err) {
        if (p->cur.type == TOK_LET) {
            par_adv(p); /* consume 'let' */
            tok_t gname = par_expect(p, TOK_IDENT, "global name");
            par_expect(p, TOK_ASSIGN, "'='");
            /* Full expression RHS: int, float, string, atom, map (%{}), list ([]), tuple.
             * par_expr already handles all of these; no need for a separate literal
             * parser. Values that require runtime state (function calls, variable
             * refs) are rejected at eval time since global_env is empty at load. */
            node_t *val = par_expr(p);
            if (p->err) break;
            if (mod->v.mod.nglobals < 16) {
                int gi = mod->v.mod.nglobals++;
                strncpy(mod->v.mod.globals[gi].name, gname.text,
                        sizeof(mod->v.mod.globals[gi].name)-1);
                mod->v.mod.globals[gi].val = val;
            } else {
                snprintf(p->errmsg, sizeof(p->errmsg),
                    "line %d: too many module globals (max 16)", gname.line);
                p->err = 1;
                node_free(val);
            }
        } else {
            mod->v.mod.nfuns++;
            mod->v.mod.funs = realloc(mod->v.mod.funs, sizeof(node_t*) * mod->v.mod.nfuns);
            mod->v.mod.funs[mod->v.mod.nfuns-1] = par_fun(p);
        }
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

/* Command-line arguments — see swarmrt_lang.h. A compiled program's
 * generated main() assigns these before the runtime starts; os_args()
 * reads them. Default zero/NULL so the interpreter and tests see an
 * empty arg list rather than garbage. */
int    sw_prog_argc = 0;
char **sw_prog_argv = NULL;

/* === GC v1: per-process value arena (see [[swarmrt-gc-design]]) ===========
 *
 * Every value constructor allocates through val_alloc/val_strdup/val_memdup.
 * Allocation target is chosen at call time:
 *   1. g_alloc_force_global set  -> global heap (calloc/strdup). Used by
 *      sw_val_deep_copy_global so values that ESCAPE a process (sent messages,
 *      spawn captures, ETS entries) land on the global heap and survive the
 *      sender's arena being freed on exit.
 *   2. else sw_self()->varena    -> the running process's arena (compiled
 *      backend); freed wholesale in process_destroy.
 *   3. else                      -> global heap (interpreter / REPL / pre-fiber
 *      builtins, where sw_self()==NULL — leaks until OS exit, as before).
 *
 * The flag is thread-local: a fiber never runs concurrently with itself, and
 * escapes go to the global heap (never another process's arena), so the arena
 * is single-writer and needs no lock. */
static __thread int g_alloc_force_global = 0;
/* Ownership v2: when set, constructors allocate into THIS region instead of the
 * running process's arena. The missing channel that lets deep_copy_into() target
 * a message/spawn/turn region (force_global only forces calloc). */
static __thread sw_value_arena_t *g_alloc_target = NULL;

/* Allocate a value node / owned blob. Routes (in order): force_global → global
 * heap; an explicit target region (deep_copy_into) → that region; the running
 * process's arena (compiled backend, freed at process exit) → that; else the
 * global heap (interpreter / pre-fiber). val_alloc zeroes (calloc semantics). */
/* Heap exhaustion is not recoverable from inside a value constructor (every
 * sw_val_* caller writes fields through the result unchecked), so fail LOUD
 * with the likely cause instead of letting the first write SIGSEGV at nil.
 * Observed in the wild: a single-scheduler 80K spawn storm runs the host out
 * of VMAs (each live fiber stack is its own mmap; Linux's default
 * vm.max_map_count is 65530), at which point mmap AND calloc return NULL. */
static void *val_alloc_oom(size_t n) {
    fprintf(stderr,
            "\n\x1b[1;31mpanic\x1b[0m: out of memory: value allocation of %zu bytes failed\n"
            "  (under a spawn storm on Linux this is often VMA exhaustion — each live\n"
            "   process stack is an mmap; check `sysctl vm.max_map_count` and the\n"
            "   number of simultaneously-live processes)\n",
            n);
    abort();
}

#ifdef SW_ALLOC_FAULT
int sw_alloc_fault_tick(void);   /* Phase 2.5 fault injection — see swarmrt_native.c */
#endif

static inline void *val_alloc(size_t n) {
    if (!g_alloc_force_global) {
        sw_value_arena_t *a = g_alloc_target ? g_alloc_target : sw_self_varena();
        if (a) {
            void *p = sw_varena_alloc(a, n);
            if (p) { memset(p, 0, n); return p; }
        }
    }
#ifdef SW_ALLOC_FAULT
    /* Inject a global-heap value-allocation failure: exercises the
     * loud-OOM path AND any caller that must unwind a half-built value. */
    if (sw_alloc_fault_tick()) return val_alloc_oom(n);
#endif
    void *p = calloc(1, n);
    return p ? p : val_alloc_oom(n);
}

static inline char *val_strdup(const char *s) {
    if (!s) return NULL;
    if (!g_alloc_force_global) {
        sw_value_arena_t *a = g_alloc_target ? g_alloc_target : sw_self_varena();
        if (a) {
            char *p = sw_varena_strdup(a, s);
            if (p) return p;
        }
    }
    char *p = strdup(s);
    return p ? p : (char *)val_alloc_oom(strlen(s) + 1);
}

sw_val_t *sw_val_nil(void) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_NIL;
    return v;
}

sw_val_t *sw_val_int(int64_t i) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_INT; v->v.i = i;
    return v;
}

sw_val_t *sw_val_float(double f) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_FLOAT; v->v.f = f;
    return v;
}

sw_val_t *sw_val_string(const char *s) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_STRING; v->v.str = val_strdup(s);
    return v;
}

sw_val_t *sw_val_atom(const char *s) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_ATOM; v->v.str = val_strdup(s);
    return v;
}

/* sw_val_bytes: length-carrying, NUL-safe byte vector. Always copies into a
 * fresh owned block (like sw_val_string's strdup) so the returned value owns
 * its bytes outright and sw_val_free can unconditionally free them. We never
 * malloc(0): len==0 still allocates 1 byte so `data` is non-NULL and
 * free()/memcmp() need no special-casing. */
sw_val_t *sw_val_bytes(const uint8_t *data, size_t len) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_BYTES;
    v->v.bytes.len = len;
    v->v.bytes.data = val_alloc(len ? len : 1);
    if (len && data) memcpy(v->v.bytes.data, data, len);
    return v;
}

sw_val_t *sw_val_pid(sw_process_t *p) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_PID; v->v.pid = p;
    return v;
}

/* sw_val_remote_pid: pid that lives on another node. `node` is the
 * remote node's name (e.g. "alpha@127.0.0.1"); `id` is the integer
 * pid id assigned on that node. `send(rpid, msg)` routes via TCP
 * through sw_node_send_pid. Constructed by sw_unmarshal when a
 * SW_MARSHAL_PID is decoded out of a wire message. */
sw_val_t *sw_val_remote_pid(const char *node, uint64_t id) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_REMOTE_PID;
    v->v.rpid.node = node ? val_strdup(node) : NULL;
    v->v.rpid.id = id;
    return v;
}

sw_val_t *sw_val_tuple(sw_val_t **items, int count) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_TUPLE;
    v->v.tuple.items = val_alloc(sizeof(sw_val_t*) * count);
    if (count > 0) memcpy(v->v.tuple.items, items, sizeof(sw_val_t*) * count);
    v->v.tuple.count = count;
    return v;
}

sw_val_t *sw_val_list(sw_val_t **items, int count) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_LIST;
    v->v.tuple.items = val_alloc(sizeof(sw_val_t*) * count);
    if (count > 0) memcpy(v->v.tuple.items, items, sizeof(sw_val_t*) * count);
    v->v.tuple.count = count;
    return v;
}

sw_val_t *sw_val_fun_native(void *fn_ptr, int nparams,
                             sw_val_t **captures, int ncaptures) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_FUN;
    v->v.fun.cfunc = fn_ptr;
    v->v.fun.num_params = nparams;
    v->v.fun.ncaptures = ncaptures;
    if (ncaptures > 0 && captures) {
        v->v.fun.captures = val_alloc(sizeof(sw_val_t*) * ncaptures);
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

/* === Shared builtin helpers (ONE impl, called by BOTH the interpreter and the
 * compiled runtime, so the backends can't drift). =========================== */

/* to_int(v): parse a string/atom, truncate a float, pass an int. nil on a
 * non-numeric string (a clear "couldn't parse", not a silent 0). */
sw_val_t *sw_coerce_int(sw_val_t *v) {
    if (!v) return sw_val_nil();
    if (v->type == SW_VAL_INT)   return sw_val_int(v->v.i);
    if (v->type == SW_VAL_FLOAT) return sw_val_int((int64_t)v->v.f);
    if ((v->type == SW_VAL_STRING || v->type == SW_VAL_ATOM) && v->v.str) {
        char *end = NULL;
        long long x = strtoll(v->v.str, &end, 10);
        if (end == v->v.str) return sw_val_nil();   /* no digits consumed */
        return sw_val_int((int64_t)x);
    }
    return sw_val_nil();
}

/* to_float(v): parse a string/atom, or widen/pass a number. nil on non-numeric. */
sw_val_t *sw_coerce_float(sw_val_t *v) {
    if (!v) return sw_val_nil();
    if (v->type == SW_VAL_INT)   return sw_val_float((double)v->v.i);
    if (v->type == SW_VAL_FLOAT) return sw_val_float(v->v.f);
    if ((v->type == SW_VAL_STRING || v->type == SW_VAL_ATOM) && v->v.str) {
        char *end = NULL;
        double x = strtod(v->v.str, &end);
        if (end == v->v.str) return sw_val_nil();
        return sw_val_float(x);
    }
    return sw_val_nil();
}

/* uuid(): a random RFC-4122 v4 UUID string. */
sw_val_t *sw_make_uuid(void) {
    uint8_t b[16];
    arc4random_buf(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);   /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);   /* variant 1 */
    char s[37];
    snprintf(s, sizeof(s),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
        b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    return sw_val_string(s);
}

/* now_iso(): current UTC time as ISO-8601, e.g. "2026-06-06T18:30:00Z". */
sw_val_t *sw_now_iso(void) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char s[32];
    strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return sw_val_string(s);
}

/* === Deep copy to the global heap (GC v1 copy-on-escape) ===================
 *
 * Rebuild the whole value DAG on the global heap so it survives the source
 * process's arena being freed. No memoization: there are no cycles (all
 * constructors build bottom-up) and DAG-shared subvalues copy as a tree, so
 * the result is alias-free and sw_val_free can recurse it safely. A depth
 * cap (matching the marshal/json limits) guards pathological nesting against
 * fiber-stack overflow.
 *
 * PIDs copy by value (the slab pointer outlives any one process). Compiled
 * closures (cfunc + captures) copy the cfunc and deep-copy each capture;
 * interpreter closures (body AST + closure_env) never reach here (they exist
 * only in the single-process interpreter, which has no arena to free). */
#define SW_COPY_MAX_DEPTH 256

static sw_val_t *deep_copy_rec(sw_val_t *v, int depth) {
    if (!v) return sw_val_nil();
    if (depth > SW_COPY_MAX_DEPTH) return sw_val_nil();
    switch (v->type) {
    case SW_VAL_NIL:    return sw_val_nil();
    case SW_VAL_INT:    return sw_val_int(v->v.i);
    case SW_VAL_FLOAT:  return sw_val_float(v->v.f);
    case SW_VAL_STRING: return sw_val_string(v->v.str ? v->v.str : "");
    case SW_VAL_ATOM:   return sw_val_atom(v->v.str ? v->v.str : "");
    case SW_VAL_BYTES:  return sw_val_bytes(v->v.bytes.data, v->v.bytes.len);
    case SW_VAL_PID:    return sw_val_pid(v->v.pid);
    case SW_VAL_REMOTE_PID: return sw_val_remote_pid(v->v.rpid.node, v->v.rpid.id);
    case SW_VAL_TUPLE:
    case SW_VAL_LIST: {
        int n = v->v.tuple.count;
        sw_val_t **items = (n > 0) ? malloc(sizeof(sw_val_t *) * n) : NULL;
        for (int i = 0; i < n; i++)
            items[i] = deep_copy_rec(v->v.tuple.items[i], depth + 1);
        sw_val_t *r = (v->type == SW_VAL_TUPLE)
                          ? sw_val_tuple(items, n)
                          : sw_val_list(items, n);
        free(items);
        return r;
    }
    case SW_VAL_MAP: {
        int n = v->v.map.count;
        sw_val_t **ks = (n > 0) ? malloc(sizeof(sw_val_t *) * n) : NULL;
        sw_val_t **vs = (n > 0) ? malloc(sizeof(sw_val_t *) * n) : NULL;
        for (int i = 0; i < n; i++) {
            ks[i] = deep_copy_rec(v->v.map.keys[i], depth + 1);
            vs[i] = deep_copy_rec(v->v.map.vals[i], depth + 1);
        }
        sw_val_t *r = sw_val_map_new(ks, vs, n);
        free(ks); free(vs);
        return r;
    }
    case SW_VAL_FUN: {
        int nc = v->v.fun.ncaptures;
        sw_val_t **caps = (nc > 0) ? malloc(sizeof(sw_val_t *) * nc) : NULL;
        for (int i = 0; i < nc; i++)
            caps[i] = deep_copy_rec(v->v.fun.captures[i], depth + 1);
        sw_val_t *r = sw_val_fun_native(v->v.fun.cfunc, v->v.fun.num_params, caps, nc);
        free(caps);
        return r;
    }
    default: return sw_val_nil();
    }
}

sw_val_t *sw_val_deep_copy_global(sw_val_t *v) {
    int save = g_alloc_force_global;
    g_alloc_force_global = 1;
    sw_val_t *r = deep_copy_rec(v, 0);
    g_alloc_force_global = save;
    return r;
}

/* Deep-copy into the running process's arena (default allocator). Explicitly
 * clears force-global AND any target region so the copy always lands in the
 * caller's arena (or the global heap when no process is current). Lets a
 * long-lived store hand a reader its OWN arena copy, so the store can free its
 * stored copy without dangling the reader (ETS copy-out). */
sw_val_t *sw_val_deep_copy_local(sw_val_t *v) {
    sw_value_arena_t *save_t = g_alloc_target;
    int save_g = g_alloc_force_global;
    g_alloc_target = NULL;
    g_alloc_force_global = 0;
    sw_val_t *r = deep_copy_rec(v, 0);
    g_alloc_target = save_t;
    g_alloc_force_global = save_g;
    return r;
}

/* Ownership v2: deep-copy v's whole graph INTO `region` (a message/spawn/turn
 * region). The region target wins over force_global and over the process arena.
 * Reads the source graph (wherever it lives) and writes the copy into `region`.
 * Returns the copied root. If region is NULL, falls back to a global-heap copy. */
sw_val_t *deep_copy_into(sw_val_t *v, sw_value_arena_t *region) {
    if (!region) return sw_val_deep_copy_global(v);
    sw_value_arena_t *save_t = g_alloc_target;
    int save_g = g_alloc_force_global;
    g_alloc_target = region;
    g_alloc_force_global = 0;
    sw_val_t *r = deep_copy_rec(v, 0);
    g_alloc_target = save_t;
    g_alloc_force_global = save_g;
    return r;
}

/* sw_send_value: enqueue an sw_val_t VALUE message, deep-copied to the global
 * heap so it outlives the sender's arena (GC v1 copy-on-send). This is the
 * type-safe choke point for every value send — its sw_val_t* parameter makes
 * passing a runtime struct (sw_call_t*, port/hotload structs, …) a compile
 * error, so those keep using the generic void* sw_send_tagged untouched.
 * Runs in the sender's context; the copy lands on the global heap regardless
 * of which process is current. NULL payload passes through unchanged (matches
 * the prior sw_send_tagged contract). */
void sw_send_value(sw_process_t *to, uint64_t tag, sw_val_t *v) {
    if (!to) return;
    if (!v) { sw_send_tagged(to, tag, NULL); return; }
    /* Ownership v2: when the sender runs under GC (has an arena, so the whole
     * binary does — the receiver will too), copy the payload into a MESSAGE
     * region the receiver adopts on match and that process_destroy bulk-frees if
     * undelivered. SW_GC_OFF / no arena / region-OOM → v1 global-heap copy. */
    if (sw_self_varena()) {
        sw_value_arena_t *r = sw_varena_create_kind(256, SW_REGION_MESSAGE);
        if (r) { sw_send_tagged_msg(to, tag, deep_copy_into(v, r), r); return; }
    }
    sw_send_tagged(to, tag, sw_val_deep_copy_global(v));
}

sw_val_t *sw_val_map_new(sw_val_t **keys, sw_val_t **vals, int count) {
    sw_val_t *v = val_alloc(sizeof(sw_val_t));
    v->type = SW_VAL_MAP;
    v->v.map.count = count;
    v->v.map.cap = count > 4 ? count : 4;
    v->v.map.keys = val_alloc(sizeof(sw_val_t*) * v->v.map.cap);
    v->v.map.vals = val_alloc(sizeof(sw_val_t*) * v->v.map.cap);
    if (count > 0 && keys && vals) {
        memcpy(v->v.map.keys, keys, sizeof(sw_val_t*) * count);
        memcpy(v->v.map.vals, vals, sizeof(sw_val_t*) * count);
    }
    return v;
}

sw_val_t *sw_val_map_get(sw_val_t *map, sw_val_t *key) {
    if (!map || map->type != SW_VAL_MAP) return sw_val_nil();
    /* First pass: exact value-equal match. */
    for (int i = 0; i < map->v.map.count; i++)
        if (sw_val_equal(map->v.map.keys[i], key)) return map->v.map.vals[i];
    /* Second pass: treat atom and string with the same text as
     * equivalent for lookup. JSON decode produces string keys; sw
     * map literals (`%{name: ...}`) produce atom keys; users expect
     * `map_get(json_decode(...), 'msg')` to work. */
    if (key && (key->type == SW_VAL_ATOM || key->type == SW_VAL_STRING) && key->v.str) {
        sw_val_type_t alt_type = (key->type == SW_VAL_ATOM) ? SW_VAL_STRING : SW_VAL_ATOM;
        for (int i = 0; i < map->v.map.count; i++) {
            sw_val_t *k = map->v.map.keys[i];
            if (k && k->type == alt_type && k->v.str && strcmp(k->v.str, key->v.str) == 0)
                return map->v.map.vals[i];
        }
    }
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
        /* Recurse into key/val nodes (mirrors the tuple/list case) — the
         * arrays alone left every entry's value node leaked. Safe: all
         * callers free fully-owned trees (json_decode output, test args);
         * none alias map sub-values. */
        for (int i = 0; i < v->v.map.count; i++) {
            sw_val_free(v->v.map.keys[i]);
            sw_val_free(v->v.map.vals[i]);
        }
        free(v->v.map.keys); free(v->v.map.vals); break;
    case SW_VAL_BYTES: free(v->v.bytes.data); break;  /* always own a block (len>=1 alloc) */
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
    case SW_VAL_BYTES:  /* NUL-safe: length-first then memcmp, never strcmp.
                           Bytes are only ever equal to bytes (Erlang keeps
                           binaries and char-lists distinct). */
        return a->v.bytes.len == b->v.bytes.len &&
               memcmp(a->v.bytes.data, b->v.bytes.data, a->v.bytes.len) == 0;
    case SW_VAL_PID:
        /* Identity is the numeric pid, not the slab pointer: a pid
         * reconstructed from a {'DOWN',...}/{'EXIT',...} message must
         * compare == the pid spawn() returned. Both point at the same
         * slab slot today, but comparing the id keeps them equal even if
         * one side carries a freshly-resolved pointer. NULL pids (id 0)
         * only equal other NULL pids. */
        return (a->v.pid ? a->v.pid->pid : 0) == (b->v.pid ? b->v.pid->pid : 0);
    case SW_VAL_REMOTE_PID:
        return a->v.rpid.id == b->v.rpid.id &&
               ((a->v.rpid.node == b->v.rpid.node) ||
                (a->v.rpid.node && b->v.rpid.node &&
                 strcmp(a->v.rpid.node, b->v.rpid.node) == 0));
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

void sw_val_format(FILE *f, sw_val_t *v) {
    if (!v) { fprintf(f, "nil"); return; }
    switch (v->type) {
    case SW_VAL_NIL: fprintf(f, "nil"); break;
    case SW_VAL_INT: fprintf(f, "%lld", (long long)v->v.i); break;
    case SW_VAL_FLOAT: fprintf(f, "%.17g", v->v.f); break;
    case SW_VAL_STRING: fprintf(f, "%s", v->v.str); break;
    case SW_VAL_ATOM: fprintf(f, ":%s", v->v.str); break;
    case SW_VAL_PID: fprintf(f, "<pid:%llu>", v->v.pid ? v->v.pid->pid : 0); break;
    case SW_VAL_REMOTE_PID:
        fprintf(f, "<rpid:%s:%llu>",
                v->v.rpid.node ? v->v.rpid.node : "?",
                (unsigned long long)v->v.rpid.id);
        break;
    case SW_VAL_TUPLE:
        fprintf(f, "{");
        for (int i = 0; i < v->v.tuple.count; i++) {
            if (i) fprintf(f, ", ");
            sw_val_format(f, v->v.tuple.items[i]);
        }
        fprintf(f, "}"); break;
    case SW_VAL_LIST:
        fprintf(f, "[");
        for (int i = 0; i < v->v.tuple.count; i++) {
            if (i) fprintf(f, ", ");
            sw_val_format(f, v->v.tuple.items[i]);
        }
        fprintf(f, "]"); break;
    case SW_VAL_MAP:
        fprintf(f, "%%{");
        for (int i = 0; i < v->v.map.count; i++) {
            if (i) fprintf(f, ", ");
            sw_val_format(f, v->v.map.keys[i]);
            fprintf(f, ": ");
            sw_val_format(f, v->v.map.vals[i]);
        }
        fprintf(f, "}"); break;
    case SW_VAL_BYTES: {
        /* Erlang-style <<d,d,...>> decimal, bounded so a megabyte of PCM
         * can't flood a log. Never dumps raw bytes to the terminal. */
        fprintf(f, "<<");
        size_t cap = v->v.bytes.len < 64 ? v->v.bytes.len : 64;
        for (size_t i = 0; i < cap; i++)
            fprintf(f, i ? ",%u" : "%u", (unsigned)v->v.bytes.data[i]);
        if (v->v.bytes.len > cap)
            fprintf(f, ",...(%zu bytes)", v->v.bytes.len);
        fprintf(f, ">>"); break;
    }
    default: fprintf(f, "?"); break;
    }
}

void sw_val_print(sw_val_t *v) {
    sw_val_format(stdout, v);
}

/* =========================================================================
 * Environment
 * ========================================================================= */

/* env lifetime: refcounted so closures that capture (escape) an env keep it
 * alive after the creating call frame returns. env_new returns refcount 1
 * (the frame's ref) and takes a ref on its parent so the whole scope chain
 * stays live as long as any descendant or capturing closure references it.
 * env_free drops one ref and only frees at 0, walking the parent chain. */
static void env_retain(sw_env_t *e) {
    if (e) e->refcount++;
}

static sw_env_t *env_new(sw_env_t *parent) {
    sw_env_t *e = calloc(1, sizeof(sw_env_t));
    e->parent = parent;
    e->refcount = 1;
    env_retain(parent);   /* this env keeps its parent alive */
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

/* Drop one ref. Frees buckets + struct (and releases parent) only at 0, so an
 * env that an escaping closure still references survives the frame's release. */
static void env_free(sw_env_t *e) {
    while (e) {
        if (--e->refcount > 0) return;     /* still referenced elsewhere */
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
        sw_env_t *parent = e->parent;       /* release our ref on parent */
        free(e);
        e = parent;                         /* loop instead of recursing */
    }
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
        /* Pattern `nil` matches both SW_VAL_NIL and the literal atom
         * `'nil'` (sw_val_equal treats them as equal at the value level,
         * so pattern match must too). */
        if (strcmp(pattern->v.sval, "nil") == 0) {
            return val->type == SW_VAL_NIL ||
                   (val->type == SW_VAL_ATOM && strcmp(val->v.str, "nil") == 0);
        }
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

/* The interpreter routes send()/receive through the SAME value-send /
 * value-receive plumbing the compiled path uses (sw_send_dispatch →
 * sw_send_value, msg_take_payload). That deep-copies the FULL sw_val_t
 * graph (tuples / lists / nested / maps / atoms / strings / ints / floats /
 * pids) into a SW_PK_VALUE message region, so structured messages round-trip
 * intact. The old flat sw_msg_val_t codec (serialize_val/deserialize_val)
 * was lossy — it dropped tuple/list item pointers and nil'd every nested
 * value — and has been removed in favour of the structure-preserving path. */

/* Monotonic milliseconds for the selective-receive timeout budget — local
 * mirror of the compiled path's _sw_now_ms (which lives in the studio
 * builtins header, not linkable here). */
static uint64_t recv_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Build a value from a queued mailbox message for pattern-matching, mirroring
 * the compiled emit_receive synthesis: runtime-injected EXIT/DOWN signals
 * carry an sw_signal_t* payload (not an sw_val_t*) and must be presented as
 * the documented {'EXIT', from, reason} / {'DOWN', ref, 'process', from,
 * reason} tuples so trap_exit / monitor patterns match. Plain send() messages
 * (SW_PK_VALUE / RAW value payloads) pass through as the live sw_val_t*. */
static sw_val_t *recv_msg_to_val(sw_msg_t *m) {
    if (m->tag == SW_TAG_EXIT && m->payload) {
        sw_signal_t *sig = (sw_signal_t *)m->payload;
        sw_val_t *reason = sig->reason_str ? sw_val_string(sig->reason_str)
                                           : sw_val_int((int64_t)sig->reason);
        sw_val_t *items[3] = { sw_val_atom("EXIT"),
                               sw_val_pid(sw_find_by_pid_any(sig->pid)),
                               reason };
        return sw_val_tuple(items, 3);
    }
    if (m->tag == SW_TAG_DOWN && m->payload) {
        sw_signal_t *sig = (sw_signal_t *)m->payload;
        sw_val_t *reason = sig->reason_str ? sw_val_string(sig->reason_str)
                                           : sw_val_int((int64_t)sig->reason);
        sw_val_t *items[5] = { sw_val_atom("DOWN"),
                               sw_val_int((int64_t)sig->ref),
                               sw_val_atom("process"),
                               sw_val_pid(sw_find_by_pid_any(sig->pid)),
                               reason };
        return sw_val_tuple(items, 5);
    }
    return m->payload ? (sw_val_t *)m->payload : sw_val_nil();
}

/* Built-in function: print.
 * The interpreter has no line editor — read_line + _sw_rl live only in
 * compiled binaries via swarmrt_builtins_studio.h. The codegen-emitted
 * _builtin_print does the input-line-aware wipe/redraw dance. */
static sw_val_t *builtin_print(sw_val_t **args, int nargs) {
    for (int i = 0; i < nargs; i++) {
        if (i) printf(" ");
        sw_val_print(args[i]);
    }
    printf("\n");
    return sw_val_atom("ok");
}

/* Built-in function: length */
/* Raise a catchable runtime panic from inside the interpreter, mirroring
 * the compiled path's _sw_runtime_panic. The message is stored verbatim
 * (e.g. "division by zero", "hd: list is empty") so assert_raises() can
 * substring-match it exactly as it would the compiled panic text, and is
 * "panic:"-prefixed to match the panic() builtin's error surface. */
static void interp_raise_panic(sw_interp_t *interp, int line, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    (void)line;
    snprintf(interp->error_msg, sizeof(interp->error_msg), "panic: %s", msg);
    interp->error = 1;
    interp->panicking = 1;   /* uncatchable — N_TRY must not absorb a panic */
}

/* Recursion-depth guard for the tree-walker.
 *
 * The interpreter recurses on the native C stack (no TCO — that lives only in
 * the compiled path, swc build). Deep sw recursion, or even moderate recursion
 * with deeply-nested expressions in the recursive call, used to blow the C
 * stack and SEGFAULT (exit 139). Instead of guessing a fixed call-depth limit
 * (unsafe: per-frame C-stack cost varies wildly with expression nesting), we
 * measure how close the *actual* C stack is to its limit and raise a clean,
 * catchable sw-level panic before we run out of room.
 *
 * SW_STACK_GUARD_MARGIN is the headroom we refuse to cross: when fewer than
 * this many bytes remain on the current thread's stack, we stop. 256 KB is
 * comfortably larger than the deepest single eval()-frame chain plus the C
 * library frames a panic unwind needs, while still permitting very deep
 * legitimate recursion on the 8 MB main-thread stack used by `swc test`/repl. */
#define SW_STACK_GUARD_MARGIN (256 * 1024)

/* Returns 1 if the C stack is within SW_STACK_GUARD_MARGIN of its limit
 * (i.e. another recursion level is unsafe), else 0. Falls back to "safe"
 * (0) if the platform can't report stack bounds, in which case the
 * call-depth counter below remains the backstop. */
static int interp_stack_near_limit(void) {
    char probe;                         /* address ~ current stack pointer */
    uintptr_t sp = (uintptr_t)&probe;

    /* If we're running ON a process fiber (e.g. `swc run`'s root process or a
     * studio tool fiber), the OS-thread stack bounds below belong to the
     * scheduler thread, NOT the swapped-in fiber — measure the fiber instead.
     * The fiber is small (128KB), so use a proportionally smaller margin; eval
     * has comfortable headroom in 128KB (parsing is offloaded to a big helper
     * thread), and the guard page below the fiber catches a true overflow. */
    uintptr_t flo, fhi;
    if (sw_self_stack_bounds(&flo, &fhi)) {
        /* Margin must be < the fiber size. The default runtime fiber is small
         * (128KB); `swc run` uses a large fiber (sw_proc_stack_size) so the
         * interpreter has OS-main-thread-like headroom. Cap the margin at a
         * quarter of the fiber so it's never larger than the stack itself. */
        size_t fiber = (size_t)(fhi - flo);
        size_t margin = SW_STACK_GUARD_MARGIN;
        if (margin > fiber / 4) margin = fiber / 4;
        return sp <= flo + margin;
    }
#if defined(__APPLE__)
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);   /* highest addr */
    size_t size = pthread_get_stacksize_np(self);
    if (!size) return 0;
    uintptr_t limit = top - size;       /* lowest addr the stack may reach */
    return sp <= limit + SW_STACK_GUARD_MARGIN;
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
    void *base = NULL; size_t size = 0;
    int rc = pthread_attr_getstack(&attr, &base, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || !size) return 0;
    uintptr_t limit = (uintptr_t)base; /* on Linux base is the LOWEST addr */
    return sp <= limit + SW_STACK_GUARD_MARGIN;
#else
    (void)sp;
    return 0;                           /* rely on the call-depth backstop */
#endif
}

/* Backstop call-depth ceiling for platforms where interp_stack_near_limit()
 * can't introspect the stack. Generous enough not to clip realistic programs,
 * low enough to stay under a small (e.g. 1 MB) stack. */
#define SW_CALL_DEPTH_MAX 1500

/* Check both guards. Raises a clean panic and returns 1 if recursion must
 * stop; 0 if it is safe to descend one more level. Call sites must NOT
 * increment call_depth before calling (this reads it as the pre-call depth). */
static int interp_recursion_blocked(sw_interp_t *interp) {
    if (interp->error) return 1;
    if (interp->call_depth >= SW_CALL_DEPTH_MAX || interp_stack_near_limit()) {
        interp_raise_panic(interp, 0,
            "interpreter recursion depth exceeded — compile with swc build "
            "for tail-call optimisation");
        return 1;
    }
    return 0;
}

/* === JSON encode (interpreter) ============================================
 * Real JSON, byte-for-byte identical to the compiled runtime's
 * _json_encode_val (swarmrt_builtins_studio.h): strings quoted+escaped,
 * nil → null, the true/false/nil atoms → JSON literals, other atoms
 * quoted (no leading ':'), ints, floats via %.17g, lists/tuples → arrays,
 * maps → objects. Replaces the old debug-repr "close-enough JSON" stub. */
static void interp_json_grow(char **buf, size_t *cap, size_t pos, size_t need) {
    size_t want = pos + need + 8;
    if (want <= *cap) return;
    size_t new_cap = *cap ? *cap : 256;
    while (new_cap < want) new_cap *= 2;
    *buf = (char *)realloc(*buf, new_cap);
    *cap = new_cap;
}

static void interp_json_append(char **buf, size_t *cap, size_t *pos, const char *s) {
    size_t len = strlen(s);
    interp_json_grow(buf, cap, *pos, len);
    memcpy(*buf + *pos, s, len);
    *pos += len;
}

static void interp_json_putc(char **buf, size_t *cap, size_t *pos, char c) {
    interp_json_grow(buf, cap, *pos, 1);
    (*buf)[(*pos)++] = c;
}

static void interp_json_encode_val(sw_val_t *v, char **buf, size_t *cap, size_t *pos) {
    if (!v || v->type == SW_VAL_NIL) {
        interp_json_append(buf, cap, pos, "null");
    } else if (v->type == SW_VAL_INT) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", (long long)v->v.i);
        interp_json_append(buf, cap, pos, tmp);
    } else if (v->type == SW_VAL_FLOAT) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.17g", v->v.f);
        interp_json_append(buf, cap, pos, tmp);
    } else if (v->type == SW_VAL_STRING) {
        interp_json_append(buf, cap, pos, "\"");
        for (const char *p = v->v.str; *p; p++) {
            switch (*p) {
                case '"':  interp_json_append(buf, cap, pos, "\\\""); break;
                case '\\': interp_json_append(buf, cap, pos, "\\\\"); break;
                case '\n': interp_json_append(buf, cap, pos, "\\n"); break;
                case '\r': interp_json_append(buf, cap, pos, "\\r"); break;
                case '\t': interp_json_append(buf, cap, pos, "\\t"); break;
                default:
                    if ((unsigned char)*p < 0x20) {
                        char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                        interp_json_append(buf, cap, pos, esc);
                    } else {
                        interp_json_putc(buf, cap, pos, *p);
                    }
            }
        }
        interp_json_append(buf, cap, pos, "\"");
    } else if (v->type == SW_VAL_ATOM) {
        if (strcmp(v->v.str, "true") == 0) interp_json_append(buf, cap, pos, "true");
        else if (strcmp(v->v.str, "false") == 0) interp_json_append(buf, cap, pos, "false");
        else if (strcmp(v->v.str, "nil") == 0) interp_json_append(buf, cap, pos, "null");
        else {
            interp_json_append(buf, cap, pos, "\"");
            interp_json_append(buf, cap, pos, v->v.str);
            interp_json_append(buf, cap, pos, "\"");
        }
    } else if (v->type == SW_VAL_LIST || v->type == SW_VAL_TUPLE) {
        interp_json_append(buf, cap, pos, "[");
        for (int i = 0; i < v->v.tuple.count; i++) {
            if (i > 0) interp_json_append(buf, cap, pos, ",");
            interp_json_encode_val(v->v.tuple.items[i], buf, cap, pos);
        }
        interp_json_append(buf, cap, pos, "]");
    } else if (v->type == SW_VAL_MAP) {
        interp_json_append(buf, cap, pos, "{");
        for (int i = 0; i < v->v.map.count; i++) {
            if (i > 0) interp_json_append(buf, cap, pos, ",");
            interp_json_append(buf, cap, pos, "\"");
            if (v->v.map.keys[i]->type == SW_VAL_STRING ||
                v->v.map.keys[i]->type == SW_VAL_ATOM)
                interp_json_append(buf, cap, pos, v->v.map.keys[i]->v.str);
            else {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%lld", (long long)v->v.map.keys[i]->v.i);
                interp_json_append(buf, cap, pos, tmp);
            }
            interp_json_append(buf, cap, pos, "\":");
            interp_json_encode_val(v->v.map.vals[i], buf, cap, pos);
        }
        interp_json_append(buf, cap, pos, "}");
    } else if (v->type == SW_VAL_BYTES) {
        /* JSON has no binary type — emit as a base64 string (matches the
         * codecs' base64-on-the-wire convention; NUL-safe via real len). */
        char *b64 = _sw_audio_b64_encode(v->v.bytes.data, v->v.bytes.len);
        interp_json_append(buf, cap, pos, "\"");
        if (b64) { interp_json_append(buf, cap, pos, b64); free(b64); }
        interp_json_append(buf, cap, pos, "\"");
    } else {
        interp_json_append(buf, cap, pos, "null");
    }
}

static sw_val_t *builtin_length(sw_val_t **args, int nargs) {
    if (nargs < 1) return sw_val_int(0);
    sw_val_t *v = args[0];
    if (v->type == SW_VAL_LIST || v->type == SW_VAL_TUPLE)
        return sw_val_int(v->v.tuple.count);
    if (v->type == SW_VAL_STRING)
        return sw_val_int((int64_t)strlen(v->v.str));
    if (v->type == SW_VAL_BYTES)
        return sw_val_int((int64_t)v->v.bytes.len);
    return sw_val_int(0);
}

/* Built-in function: hd (head of list).
 * Strict, matching compiled _builtin_hd: panics on non-list / empty list
 * (was silently returning nil). */
static sw_val_t *builtin_hd(sw_interp_t *interp, sw_val_t **args, int nargs) {
    if (nargs < 1 || !args[0]) { interp_raise_panic(interp, 0, "hd: no list given"); return sw_val_nil(); }
    if (args[0]->type != SW_VAL_LIST) { interp_raise_panic(interp, 0, "hd: not a list (got type %d)", args[0]->type); return sw_val_nil(); }
    if (args[0]->v.tuple.count == 0) { interp_raise_panic(interp, 0, "hd: list is empty"); return sw_val_nil(); }
    return args[0]->v.tuple.items[0];
}

/* Built-in function: tl (tail of list).
 * Strict, matching compiled _builtin_tl: panics on non-list / empty list;
 * tl of a 1-element list returns [] (Erlang semantics). */
static sw_val_t *builtin_tl(sw_interp_t *interp, sw_val_t **args, int nargs) {
    if (nargs < 1 || !args[0]) { interp_raise_panic(interp, 0, "tl: no list given"); return sw_val_nil(); }
    if (args[0]->type != SW_VAL_LIST) { interp_raise_panic(interp, 0, "tl: not a list (got type %d)", args[0]->type); return sw_val_nil(); }
    if (args[0]->v.tuple.count == 0) { interp_raise_panic(interp, 0, "tl: list is empty"); return sw_val_nil(); }
    if (args[0]->v.tuple.count == 1) return sw_val_list(NULL, 0);
    return sw_val_list(args[0]->v.tuple.items + 1, args[0]->v.tuple.count - 1);
}

/* Built-in function: elem (tuple element access).
 * Strict, matching compiled _builtin_elem: panics on non-tuple, non-int
 * index, or out-of-range/negative index (was silently returning nil). */
static sw_val_t *builtin_elem(sw_interp_t *interp, sw_val_t **args, int nargs) {
    if (nargs < 2 || !args[0] || !args[1]) { interp_raise_panic(interp, 0, "elem: needs (tuple, index)"); return sw_val_nil(); }
    if (args[0]->type != SW_VAL_TUPLE) { interp_raise_panic(interp, 0, "elem: not a tuple (got type %d)", args[0]->type); return sw_val_nil(); }
    if (args[1]->type != SW_VAL_INT) { interp_raise_panic(interp, 0, "elem: index must be int"); return sw_val_nil(); }
    int idx = (int)args[1]->v.i;
    if (idx < 0 || idx >= args[0]->v.tuple.count) {
        interp_raise_panic(interp, 0, "elem: index %d out of range for %d-tuple", idx, args[0]->v.tuple.count);
        return sw_val_nil();
    }
    return args[0]->v.tuple.items[idx];
}

/* Built-in: assert(condition) — fail test if falsy */
static sw_val_t *builtin_assert(sw_interp_t *interp, sw_val_t **args, int nargs, int line) {
    if (nargs < 1 || !sw_val_is_truthy(args[0])) {
        interp->assert_failed = 1;
        interp->assert_line = line;
        if (nargs >= 2 && args[1]->type == SW_VAL_STRING)
            snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                     "assertion failed: %s", args[1]->v.str);
        else
            snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                     "assertion failed");
        return sw_val_atom("error");
    }
    return sw_val_atom("ok");
}

/* Format a value into a buffer (avoids stdout redirect) */
static void val_to_str(sw_val_t *v, char *buf, size_t bufsz) {
    if (!v || !buf || bufsz == 0) return;
    switch (v->type) {
    case SW_VAL_NIL:    snprintf(buf, bufsz, "nil"); break;
    case SW_VAL_INT:    snprintf(buf, bufsz, "%lld", (long long)v->v.i); break;
    case SW_VAL_FLOAT:  snprintf(buf, bufsz, "%g", v->v.f); break;
    case SW_VAL_STRING: snprintf(buf, bufsz, "\"%s\"", v->v.str); break;
    case SW_VAL_ATOM:   snprintf(buf, bufsz, ":%s", v->v.str); break;
    case SW_VAL_PID:    snprintf(buf, bufsz, "#PID"); break;
    case SW_VAL_FUN:    snprintf(buf, bufsz, "#Fun"); break;
    default:            snprintf(buf, bufsz, "<val>"); break;
    }
}

/* Built-in: assert_eq(actual, expected) */
static sw_val_t *builtin_assert_eq(sw_interp_t *interp, sw_val_t **args, int nargs, int line) {
    if (nargs < 2 || !sw_val_equal(args[0], args[1])) {
        interp->assert_failed = 1;
        interp->assert_line = line;
        if (nargs >= 2) {
            char abuf[128], ebuf[128];
            val_to_str(args[0], abuf, sizeof(abuf));
            val_to_str(args[1], ebuf, sizeof(ebuf));
            snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                     "expected %s, got %s", ebuf, abuf);
        } else {
            snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                     "assert_eq requires 2 arguments");
        }
        return sw_val_atom("error");
    }
    return sw_val_atom("ok");
}

/* Built-in: assert_ne(a, b) — fail if equal */
static sw_val_t *builtin_assert_ne(sw_interp_t *interp, sw_val_t **args, int nargs, int line) {
    if (nargs < 2 || sw_val_equal(args[0], args[1])) {
        interp->assert_failed = 1;
        interp->assert_line = line;
        snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                 "expected values to differ, but both are equal");
        return sw_val_atom("error");
    }
    return sw_val_atom("ok");
}

/* Built-in: assert_raises(fn, expected_msg)
 *   fn          — zero-arg sw lambda (SW_VAL_FUN with a body node)
 *   expected_msg — string that must appear inside the panic/error message
 *
 * Pass: fn() triggers interp->error AND error_msg contains expected_msg.
 * Fail (no panic):   assert_failed set with "expected panic, got none".
 * Fail (wrong msg):  assert_failed set with "expected '...' in panic msg, got '...'".
 */
static sw_val_t *builtin_assert_raises(sw_interp_t *interp, sw_val_t **args, int nargs, int line) {
    if (nargs < 2) {
        interp->assert_failed = 1;
        interp->assert_line = line;
        snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                 "assert_raises requires 2 arguments: fn and expected_msg");
        return sw_val_atom("error");
    }

    sw_val_t *fn_val = args[0];
    const char *expected = (args[1]->type == SW_VAL_STRING || args[1]->type == SW_VAL_ATOM)
                           ? args[1]->v.str : "";

    if (!fn_val || fn_val->type != SW_VAL_FUN || !fn_val->v.fun.body) {
        interp->assert_failed = 1;
        interp->assert_line = line;
        snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                 "assert_raises: first argument must be a zero-arg lambda");
        return sw_val_atom("error");
    }

    /* Save current error state so nested assert_raises work correctly. */
    int saved_error = interp->error;
    int saved_panicking = interp->panicking;
    char saved_error_msg[256];
    memcpy(saved_error_msg, interp->error_msg, sizeof(saved_error_msg));

    /* Clear error so the lambda runs fresh. */
    interp->error = 0;
    interp->panicking = 0;
    interp->error_msg[0] = '\0';

    /* assert_raises is semantically a try: error() inside the probe fn must
     * register (try_depth gates the outside-a-try silent path), and the
     * panic interception below is this helper's sanctioned, documented
     * exception to "panics are uncatchable". */
    interp->try_depth++;

    /* Call the lambda body.  Mirrors the dynamic-dispatch closure pattern
     * (eval N_CALL, "Dynamic dispatch: variable holds a closure"). */
    node_t *fn_node = (node_t *)fn_val->v.fun.body;
    sw_env_t *fenv = env_new(fn_val->v.fun.closure_env
                             ? fn_val->v.fun.closure_env
                             : interp->global_env);
    /* Zero-arg lambda — no params to bind. */
    (void)eval(interp, fn_node->v.fun.body, fenv);
    env_free(fenv);

    interp->try_depth--;

    int did_error = interp->error;
    char actual_msg[256];
    memcpy(actual_msg, interp->error_msg, sizeof(actual_msg));

    /* Restore outer error state (panic interception included). */
    interp->error = saved_error;
    interp->panicking = saved_panicking;
    memcpy(interp->error_msg, saved_error_msg, sizeof(interp->error_msg));

    if (!did_error) {
        /* fn returned normally — expected a panic/error. */
        interp->assert_failed = 1;
        interp->assert_line = line;
        snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                 "expected panic containing '%s', but fn did not panic", expected);
        return sw_val_atom("error");
    }

    if (expected[0] != '\0' && strstr(actual_msg, expected) == NULL) {
        /* Panicked with wrong message. */
        interp->assert_failed = 1;
        interp->assert_line = line;
        snprintf(interp->assert_msg, sizeof(interp->assert_msg),
                 "expected panic containing '%s', got '%s'", expected, actual_msg);
        return sw_val_atom("error");
    }

    return sw_val_atom("ok");
}

/* =========================================================================
 * Interpreter-path ETS (value-aware, hash table, 256 buckets, 64 tables)
 *
 * The codegen path gets _vets_* via swarmrt_builtins_studio.h (included
 * in generated C).  The interpreter (REPL / swc test) needs its own copy
 * because swarmrt_lang.c is compiled independently.  These statics are
 * only alive during interpreter runs, so the duplication is harmless.
 * =========================================================================
 */

#define _INTERP_VETS_BUCKETS   256
#define _INTERP_VETS_MAX_TABLES 64

typedef struct _interp_vets_entry {
    sw_val_t *key;
    sw_val_t *value;
    struct _interp_vets_entry *next;
} _interp_vets_entry_t;

typedef struct {
    _interp_vets_entry_t *buckets[_INTERP_VETS_BUCKETS];
    pthread_rwlock_t lock;
    int active;
} _interp_vets_table_t;

static _interp_vets_table_t _interp_vets_tables[_INTERP_VETS_MAX_TABLES];
static int _interp_vets_next_id = 0;
static pthread_mutex_t _interp_vets_meta = PTHREAD_MUTEX_INITIALIZER;

static uint32_t _interp_vets_hash(sw_val_t *v) {
    uint64_t h = 14695981039346656037ULL;
    if (!v) return 0;
    switch (v->type) {
        case SW_VAL_INT: {
            uint64_t k = (uint64_t)v->v.i;
            for (int i = 0; i < 8; i++) { h ^= (k & 0xff); h *= 1099511628211ULL; k >>= 8; }
            break;
        }
        case SW_VAL_STRING: case SW_VAL_ATOM: {
            if (v->v.str)
                for (const char *s = v->v.str; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
            break;
        }
        case SW_VAL_BYTES: {
            for (size_t i = 0; i < v->v.bytes.len; i++) { h ^= v->v.bytes.data[i]; h *= 1099511628211ULL; }
            break;
        }
        default: {
            uint64_t k = (uint64_t)(uintptr_t)v;
            h ^= k; h *= 1099511628211ULL;
            break;
        }
    }
    return (uint32_t)(h % _INTERP_VETS_BUCKETS);
}

static int _interp_vets_key_eq(sw_val_t *a, sw_val_t *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    switch (a->type) {
        case SW_VAL_INT:    return a->v.i == b->v.i;
        case SW_VAL_STRING: case SW_VAL_ATOM:
            return a->v.str && b->v.str && strcmp(a->v.str, b->v.str) == 0;
        case SW_VAL_BYTES:  /* NUL-safe: length-first then memcmp */
            return a->v.bytes.len == b->v.bytes.len &&
                   memcmp(a->v.bytes.data, b->v.bytes.data, a->v.bytes.len) == 0;
        case SW_VAL_PID:    /* compare by numeric pid id, see sw_val_equal */
            return (a->v.pid ? a->v.pid->pid : 0) == (b->v.pid ? b->v.pid->pid : 0);
        default:            return a == b;
    }
}

/* =========================================================================
 * Extra interpreter/REPL builtins
 *
 * The interpreter (REPL + `swc test`) historically only knew ~30 builtins
 * while compiled `.sw` programs had ~100+, defined as static functions in
 * `swarmrt_builtins_studio.h` and emitted directly into generated C.
 * That meant a user could call db_open() in a compiled program but hit
 * "undefined function" in the REPL.
 *
 * This block bridges the gap with minimum-viable implementations of the
 * pure-functional builtins (no per-process scheduler state required).
 * Process-scheduler primitives (link/monitor/exit_proc/etc) print a one-
 * shot hint and return nil instead of silently dropping through.
 * ========================================================================= */

#define _REPL_SQLITE_MAX 32
static sqlite3 *_repl_sqlite_db[_REPL_SQLITE_MAX];

static int _repl_scheduler_warned = 0;
static void _repl_warn_scheduler(const char *name) {
    if (_repl_scheduler_warned) return;
    fprintf(stderr,
        "note: '%s' (and similar scheduler primitives) return nil in the REPL;\n"
        "      use `swc build file.sw -o bin/x && bin/x` for full process semantics.\n",
        name);
    _repl_scheduler_warned = 1;
}

/* ---- spawn(): re-enter the interpreter on a fresh fiber -------------------
 *
 * Heap boot-context handed to a spawned child. Carries everything the child
 * fiber needs to re-enter the tree-walker on its OWN process context (with
 * tls_current set to the child, so self()/receive/send resolve to it). Safe
 * because `swc run` forces nsched=1: the child runs as a cooperative fiber on
 * the SAME OS thread as its parent, never touching the shared interp/env
 * concurrently. b->fn_node and b->params[] point into the immortal module AST
 * (run_file never frees it); b->args hold parent-evaluated global-heap values.
 * The trampoline owns and frees b (and b->args) on exit. */
typedef struct {
    sw_interp_t  *interp;        /* the single shared interp (safe: nsched=1) */
    node_t       *fn_node;       /* the N_FUN node whose .v.fun.body we eval */
    sw_env_t     *closure_env;   /* closure env (NULL → interp->global_env) */
    sw_val_t    **args;          /* pre-evaluated arg values (heap), or NULL */
    int           nargs;
} spawn_boot_t;

static sw_val_t *eval(sw_interp_t *interp, node_t *n, sw_env_t *env);

/* Child-fiber panic propagation, shared by every interpreter fiber that runs a
 * user fn as a process (plain spawn AND supervised children). If `interp` was
 * left in a crashed state by eval(), (1) clear the SHARED flags so the crash
 * does not leak into other fibers, and (2) abort THIS process via the native
 * sw_process_panic so the scheduler's process_exit walks our links/monitors and
 * synthesises {'EXIT',from,reason}/{'DOWN',...} — exactly as _sw_runtime_panic
 * does for a compiled process. sw_process_panic is NORETURN. The reason mirrors
 * the compiled EXIT reason (bare message), so we strip interp_raise_panic's
 * "panic: " prefix. No-op (returns) if the fiber exited cleanly. */
static void interp_fiber_propagate_panic(sw_interp_t *interp) {
    if (!(interp->error || interp->panicking)) return;
    char reason[256];
    const char *m = interp->error_msg;
    if (strncmp(m, "panic: ", 7) == 0) m += 7;
    snprintf(reason, sizeof(reason), "%s", m);
    interp->error = 0;
    interp->panicking = 0;
    interp->error_msg[0] = '\0';
    sw_process_t *me = sw_self();
    if (me) sw_process_panic(me, -1, reason);  /* NORETURN */
}

/* C trampoline run on the child's fiber. Mirrors run_main_entry (swc.c) and
 * the compiled _main_entry: bind args into a fresh env (child of the closure
 * or global env), bracket call_depth, eval the fn body, tear down. */
static void spawn_trampoline(void *arg) {
    spawn_boot_t *b = (spawn_boot_t *)arg;
    sw_env_t *fenv = env_new(b->closure_env ? b->closure_env
                                            : b->interp->global_env);
    node_t *fn = b->fn_node;
    for (int i = 0; i < b->nargs && i < fn->v.fun.nparams && i < 16; i++)
        env_set(fenv, fn->v.fun.params[i], b->args[i]);
    sw_interp_t *interp = b->interp;   /* capture before free(b) below */
    interp->call_depth++;
    (void)eval(interp, fn->v.fun.body, fenv);
    interp->call_depth--;
    env_free(fenv);
    if (b->args) free(b->args);
    free(b);

    interp_fiber_propagate_panic(interp);
}

/* === Interpreter-aware supervisor child entry ============================
 *
 * The native supervisor (sw_supervisor_start) is fully reused for restart
 * strategy + circuit-breaker + registration; only the per-child START differs.
 * The compiled backend's _sup_child_entry dispatches the child closure via
 * sw_val_apply (cfunc-ONLY) — interpreter lambdas carry an AST body +
 * closure_env with NO cfunc, so that path would silently no-op. So we supply an
 * interpreter-native start_func that eval()s the lambda's body, plus the same
 * MASTER/per-incarnation-copy closure contract the compiled path uses:
 *   start_arg      = MASTER (freed at child removal / supervisor teardown)
 *   copy_start_arg = fresh per-incarnation copy (freed by the child itself)
 *   free_start_arg = frees a copy or the master
 * Single-scheduler (`swc run` forces nsched=1): the child is a cooperative
 * fiber on the parent's OS thread, never touching the shared interp/env
 * concurrently. fn_node points into the immortal module AST; closure_env lives
 * in (or under) the never-freed global_env — so the small struct is all that is
 * copied/freed, the value graph it references is stable. */
typedef struct {
    sw_interp_t *interp;
    node_t      *fn_node;       /* N_FUN whose .v.fun.body we eval (0 params) */
    sw_env_t    *closure_env;   /* NULL -> global_env */
} sup_interp_child_t;

static void *_sup_interp_copy(void *raw) {
    sup_interp_child_t *src = (sup_interp_child_t *)raw;
    sup_interp_child_t *cp = (sup_interp_child_t *)malloc(sizeof(sup_interp_child_t));
    *cp = *src;
    return cp;
}
static void _sup_interp_free(void *raw) {
    free(raw);
}
static void _sup_interp_entry(void *raw) {
    sup_interp_child_t *c = (sup_interp_child_t *)raw;
    sw_interp_t *interp = c->interp;
    node_t *fn = c->fn_node;
    if (fn && fn->v.fun.body) {
        sw_env_t *fenv = env_new(c->closure_env ? c->closure_env : interp->global_env);
        interp->call_depth++;
        (void)eval(interp, fn->v.fun.body, fenv);
        interp->call_depth--;
        env_free(fenv);
    }
    /* `c` is THIS incarnation's private copy; freed in process_destroy via the
     * dtor sup_start_child armed (free_start_arg) before this fiber ran. */
    interp_fiber_propagate_panic(interp);
}

/* Invoke an interpreter SW_VAL_FUN value (an AST lambda) with `nargs`
 * args. The compiled-path sw_val_apply() only handles cfunc closures; this
 * is its interpreter twin, used by the functional primitives (map/reduce/
 * filter) below. Returns nil on a non-callable value. */
static sw_val_t *interp_apply_fn(sw_interp_t *interp, sw_val_t *fn,
                                 sw_val_t **args, int nargs) {
    if (!fn || fn->type != SW_VAL_FUN || !fn->v.fun.body) return sw_val_nil();
    if (interp_recursion_blocked(interp)) return sw_val_nil();
    node_t *fn_node = (node_t *)fn->v.fun.body;
    sw_env_t *fenv = env_new(fn->v.fun.closure_env ? fn->v.fun.closure_env
                                                    : interp->global_env);
    for (int i = 0; i < fn_node->v.fun.nparams && i < nargs; i++)
        env_set(fenv, fn_node->v.fun.params[i], args[i]);
    interp->call_depth++;
    sw_val_t *r = eval(interp, fn_node->v.fun.body, fenv);
    interp->call_depth--;
    env_free(fenv);
    return r;
}

/* Returns NULL if `fname` isn't a recognized extra builtin — caller
 * continues to user-fn lookup. */
static sw_val_t *interp_extra_builtin(sw_interp_t *interp, const char *fname,
                                       sw_val_t **args, int nargs, int line) {
    (void)line;

    /* === Functional primitives (map / reduce / filter) =========
     * Compiled-path-only prelude builtins until now; the interpreter run
     * path (`swc run`) needs them too — Std.sum/product/etc. call reduce.
     * Argument order matches codegen: map accepts (fn,list) or (list,fn);
     * reduce is (fn, list, acc); filter is (fn, list) or (list, fn). */
    if (strcmp(fname, "map") == 0 && nargs >= 2) {
        sw_val_t *fn, *lst;
        if (args[0] && args[0]->type == SW_VAL_FUN)      { fn = args[0]; lst = args[1]; }
        else if (args[1] && args[1]->type == SW_VAL_FUN) { lst = args[0]; fn = args[1]; }
        else { fn = args[0]; lst = args[1]; }
        if (!lst || lst->type != SW_VAL_LIST) return sw_val_list(NULL, 0);
        int cnt = lst->v.tuple.count;
        if (cnt == 0) return sw_val_list(NULL, 0);
        sw_val_t **res = malloc(sizeof(sw_val_t *) * cnt);
        for (int i = 0; i < cnt; i++) {
            sw_val_t *arg = lst->v.tuple.items[i];
            res[i] = interp_apply_fn(interp, fn, &arg, 1);
        }
        sw_val_t *r = sw_val_list(res, cnt);
        free(res);
        return r;
    }
    if (strcmp(fname, "reduce") == 0 && nargs >= 3) {
        /* (fn, list, acc) or the piped (list, fn, acc) — codegen parity. */
        sw_val_t *fn, *lst;
        if (args[0] && args[0]->type == SW_VAL_FUN)      { fn = args[0]; lst = args[1]; }
        else if (args[1] && args[1]->type == SW_VAL_FUN) { lst = args[0]; fn = args[1]; }
        else { fn = args[0]; lst = args[1]; }
        sw_val_t *acc = args[2];
        if (!lst || lst->type != SW_VAL_LIST) return acc;
        for (int i = 0; i < lst->v.tuple.count; i++) {
            sw_val_t *a2[2] = { acc, lst->v.tuple.items[i] };
            acc = interp_apply_fn(interp, fn, a2, 2);
        }
        return acc;
    }
    if (strcmp(fname, "filter") == 0 && nargs >= 2) {
        sw_val_t *fn, *lst;
        if (args[0] && args[0]->type == SW_VAL_FUN)      { fn = args[0]; lst = args[1]; }
        else if (args[1] && args[1]->type == SW_VAL_FUN) { lst = args[0]; fn = args[1]; }
        else { fn = args[0]; lst = args[1]; }
        if (!lst || lst->type != SW_VAL_LIST) return sw_val_list(NULL, 0);
        int cnt = lst->v.tuple.count;
        if (cnt == 0) return sw_val_list(NULL, 0);
        sw_val_t **res = malloc(sizeof(sw_val_t *) * cnt);
        int nkeep = 0;
        for (int i = 0; i < cnt; i++) {
            sw_val_t *item = lst->v.tuple.items[i];
            sw_val_t *keep = interp_apply_fn(interp, fn, &item, 1);
            int truthy = keep && !(keep->type == SW_VAL_NIL) &&
                         !(keep->type == SW_VAL_ATOM && strcmp(keep->v.str, "false") == 0) &&
                         !(keep->type == SW_VAL_INT && keep->v.i == 0);
            if (truthy) res[nkeep++] = item;
        }
        sw_val_t *r = sw_val_list(res, nkeep);
        free(res);
        return r;
    }

    /* === System ================================================ */
    if (strcmp(fname, "sleep") == 0 && nargs >= 1 && args[0]->type == SW_VAL_INT) {
        int64_t ms = args[0]->v.i;
        if (ms > 0) {
            /* Yield to the scheduler (sw_receive_any with a timeout) when
             * running inside a live process, exactly like the compiled
             * _builtin_sleep — under the single-scheduler `swc run` path all
             * processes are cooperative fibers on ONE thread, so a raw usleep
             * would block the whole thread and starve spawned children (e.g.
             * `spawn(child) ; sleep(150)` would never let the child run).
             * Fall back to usleep only outside a process (REPL/test contexts
             * where tls_current is NULL). */
            if (sw_self()) {
                uint64_t tag = 0;
                void *m = sw_receive_any((uint64_t)ms, &tag);
                if (m) free(m);  /* discard any spurious message */
            } else {
                usleep((useconds_t)(ms * 1000));
            }
        }
        return sw_val_atom("ok");  /* matches codegen path */
    }
    if (strcmp(fname, "sys_exit") == 0 && nargs >= 1 && args[0]->type == SW_VAL_INT) {
        exit((int)args[0]->v.i);
    }
    if (strcmp(fname, "random_int") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_INT && args[1]->type == SW_VAL_INT) {
        int64_t lo = args[0]->v.i, hi = args[1]->v.i;
        if (hi <= lo) return sw_val_int(lo);
        return sw_val_int(lo + (rand() % (int)(hi - lo + 1)));
    }

    /* === Error model =========================================== */
    if (strcmp(fname, "panic") == 0) {
        char *buf = NULL; size_t blen = 0;
        FILE *m = open_memstream(&buf, &blen);
        if (m) {
            if (nargs > 0) sw_val_format(m, args[0]);
            else fputs("panic", m);
            fclose(m);
        }
        /* Panic: uncatchable (panicking flag bypasses N_TRY), aborts the run
         * with exit 1 at top level — same meaning as the compiled path. Only
         * assert_raises (the documented test seam) intercepts it. */
        snprintf(interp->error_msg, sizeof(interp->error_msg),
                 "panic: %s", buf ? buf : "(no message)");
        free(buf);
        interp->error = 1;
        interp->panicking = 1;
        return sw_val_nil();
    }
    if (strcmp(fname, "expect") == 0 && nargs >= 1) {
        int falsy = (args[0]->type == SW_VAL_NIL) ||
                    (args[0]->type == SW_VAL_ATOM &&
                     (strcmp(args[0]->v.str, "false") == 0 ||
                      strcmp(args[0]->v.str, "nil") == 0));
        if (falsy) {
            const char *msg = (nargs >= 2 && args[1]->type == SW_VAL_STRING)
                                ? args[1]->v.str : "expect failed";
            /* A failed expect IS a panic (compiled parity): uncatchable,
             * exit 1 at top level, interceptable only by assert_raises —
             * not a hard exit(1) here, which bypassed assert_raises and
             * skipped the uniform top-level panic report. */
            interp_raise_panic(interp, line, "%s", msg);
            return sw_val_nil();
        }
        return args[0];
    }
    if (strcmp(fname, "error") == 0) {
        /* Outside any dynamically-enclosing try (or assert_raises), error()
         * is documented as silent — execution continues with nil. Setting
         * the flag here used to silently unwind the entire rest of the
         * program instead (every eval short-circuits on interp->error). */
        if (interp->try_depth > 0) {
            interp->error = 1;
            if (nargs > 0 && (args[0]->type == SW_VAL_STRING || args[0]->type == SW_VAL_ATOM))
                snprintf(interp->error_msg, sizeof(interp->error_msg), "%s", args[0]->v.str);
            else
                snprintf(interp->error_msg, sizeof(interp->error_msg), "error");
        }
        return sw_val_nil();
    }

    /* === Strings ================================================ */
    if (strcmp(fname, "string_sub") == 0 && nargs >= 3 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_INT && args[2]->type == SW_VAL_INT) {
        const char *s = args[0]->v.str;
        int slen = (int)strlen(s);
        int start = (int)args[1]->v.i;
        int len = (int)args[2]->v.i;
        if (start < 0) start = 0;
        if (start >= slen) return sw_val_string("");
        if (start + len > slen) len = slen - start;
        if (len <= 0) return sw_val_string("");
        char *buf = (char *)malloc(len + 1);
        memcpy(buf, s + start, len); buf[len] = '\0';
        sw_val_t *r = sw_val_string(buf); free(buf); return r;
    }
    if (strcmp(fname, "string_replace") == 0 && nargs >= 3 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_STRING && args[2]->type == SW_VAL_STRING) {
        const char *src = args[0]->v.str, *old = args[1]->v.str, *rep = args[2]->v.str;
        size_t olen = strlen(old), rlen = strlen(rep), slen = strlen(src);
        if (olen == 0) return args[0];
        size_t cap = slen * 2 + rlen + 4096;
        char *buf = (char *)malloc(cap); size_t blen = 0;
        const char *p = src;
        while (*p) {
            const char *f = strstr(p, old);
            if (!f) {
                size_t r = strlen(p);
                if (blen + r < cap - 1) { memcpy(buf + blen, p, r); blen += r; }
                break;
            }
            size_t chunk = (size_t)(f - p);
            if (blen + chunk + rlen < cap - 1) {
                memcpy(buf + blen, p, chunk); blen += chunk;
                memcpy(buf + blen, rep, rlen); blen += rlen;
            }
            p = f + olen;
        }
        buf[blen] = '\0';
        sw_val_t *r = sw_val_string(buf); free(buf); return r;
    }
    if (strcmp(fname, "string_truncate") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_INT) {
        /* Matches codegen: hard truncate to max_len, no ellipsis. */
        const char *s = args[0]->v.str;
        int64_t max = args[1]->v.i;
        if (max <= 0) return sw_val_string("");
        size_t slen = strlen(s);
        if ((int64_t)slen <= max) return args[0];
        char *r = (char *)malloc((size_t)max + 1);
        memcpy(r, s, (size_t)max); r[max] = '\0';
        sw_val_t *v = sw_val_string(r); free(r); return v;
    }

    /* === File ================================================== */
    if (strcmp(fname, "file_read") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        FILE *fp = fopen(args[0]->v.str, "rb");
        if (!fp) return sw_val_nil();
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (sz < 0 || sz > 64 * 1024 * 1024) { fclose(fp); return sw_val_nil(); }
        char *buf = (char *)malloc(sz + 1);
        size_t got = fread(buf, 1, sz, fp); buf[got] = '\0'; fclose(fp);
        sw_val_t *r = sw_val_string(buf); free(buf); return r;
    }
    if (strcmp(fname, "file_write") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_STRING) {
        FILE *fp = fopen(args[0]->v.str, "wb");
        if (!fp) return sw_val_atom("error");
        fwrite(args[1]->v.str, 1, strlen(args[1]->v.str), fp); fclose(fp);
        return sw_val_atom("ok");
    }
    if (strcmp(fname, "file_append") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_STRING) {
        FILE *fp = fopen(args[0]->v.str, "ab");
        if (!fp) return sw_val_atom("error");
        fwrite(args[1]->v.str, 1, strlen(args[1]->v.str), fp); fclose(fp);
        return sw_val_atom("ok");
    }
    /* file_read_bytes(path) → bytes | nil — binary-safe, NUL-preserving,
     * no 1MB cap (parity with codegen). */
    if (strcmp(fname, "file_read_bytes") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        FILE *fp = fopen(args[0]->v.str, "rb");
        if (!fp) return sw_val_nil();
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (sz < 0) { fclose(fp); return sw_val_nil(); }
        uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
        size_t got = fread(buf, 1, (size_t)sz, fp); fclose(fp);
        sw_val_t *r = sw_val_bytes(buf, got); free(buf); return r;
    }
    /* file_write_bytes(path, bytes) → 'ok' | 'error' — writes exact byte
     * count of a SW_VAL_BYTES, no truncation (parity with codegen). */
    if (strcmp(fname, "file_write_bytes") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_BYTES) {
        FILE *fp = fopen(args[0]->v.str, "wb");
        if (!fp) return sw_val_atom("error");
        size_t len = args[1]->v.bytes.len;
        size_t wr = len ? fwrite(args[1]->v.bytes.data, 1, len, fp) : 0;
        int ok = (fclose(fp) == 0) && (wr == len);
        return sw_val_atom(ok ? "ok" : "error");
    }
    if (strcmp(fname, "file_exists") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        struct stat st;
        return sw_val_atom(stat(args[0]->v.str, &st) == 0 ? "true" : "false");
    }
    if (strcmp(fname, "file_delete") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        return sw_val_atom(unlink(args[0]->v.str) == 0 ? "ok" : "error");
    }
    if (strcmp(fname, "file_mkdir") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        int rc = mkdir(args[0]->v.str, 0755);
        return sw_val_atom((rc == 0 || errno == EEXIST) ? "ok" : "error");
    }
    if (strcmp(fname, "file_list") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        DIR *d = opendir(args[0]->v.str);
        if (!d) return sw_val_list(NULL, 0);
        sw_val_t *items[1024]; int count = 0;
        struct dirent *e;
        while ((e = readdir(d)) && count < 1024) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            items[count++] = sw_val_string(e->d_name);
        }
        closedir(d);
        return sw_val_list(items, count);
    }
    if (strcmp(fname, "getenv") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        const char *v = getenv(args[0]->v.str);
        return v ? sw_val_string(v) : sw_val_nil();
    }
    if (strcmp(fname, "os_args") == 0) {
        /* Command-line arguments as a list of strings, argv[0] first.
         * Empty under the interpreter unless a host populated them. */
        if (sw_prog_argc <= 0 || !sw_prog_argv) return sw_val_list(NULL, 0);
        sw_val_t **items = malloc(sizeof(sw_val_t *) * sw_prog_argc);
        for (int i = 0; i < sw_prog_argc; i++)
            items[i] = sw_val_string(sw_prog_argv[i] ? sw_prog_argv[i] : "");
        sw_val_t *r = sw_val_list(items, sw_prog_argc);
        free(items);
        return r;
    }
    if (strcmp(fname, "print_above") == 0) {
        /* No raw line editor under the interpreter — behaves as print. */
        for (int i = 0; i < nargs; i++) {
            if (i) printf(" ");
            sw_val_print(args[i]);
        }
        printf("\n");
        return sw_val_atom("ok");
    }

    /* === JSON helpers ========================================== */
    if (strcmp(fname, "json_escape") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        /* Matches codegen: returns a complete JSON string literal,
         * including surrounding double-quotes. */
        const char *s = args[0]->v.str;
        size_t slen = strlen(s);
        size_t cap = slen * 6 + 4;
        char *buf = (char *)malloc(cap); size_t blen = 0;
        buf[blen++] = '"';
        for (const char *p = s; *p; p++) {
            switch (*p) {
            case '"':  buf[blen++] = '\\'; buf[blen++] = '"'; break;
            case '\\': buf[blen++] = '\\'; buf[blen++] = '\\'; break;
            case '\n': buf[blen++] = '\\'; buf[blen++] = 'n'; break;
            case '\r': buf[blen++] = '\\'; buf[blen++] = 'r'; break;
            case '\t': buf[blen++] = '\\'; buf[blen++] = 't'; break;
            default:   buf[blen++] = *p; break;
            }
        }
        buf[blen++] = '"';
        buf[blen] = '\0';
        sw_val_t *r = sw_val_string(buf); free(buf); return r;
    }
    if (strcmp(fname, "json_get") == 0 && nargs >= 2 && args[0]->type == SW_VAL_STRING) {
        sw_val_t *decoded = sw_lang_json_decode(args[0]->v.str);
        if (!decoded || decoded->type != SW_VAL_MAP) return sw_val_nil();
        return sw_val_map_get(decoded, args[1]);
    }

    /* === Map ops =============================================== */
    if (strcmp(fname, "map_merge") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_MAP && args[1]->type == SW_VAL_MAP) {
        int total = args[0]->v.map.count + args[1]->v.map.count;
        sw_val_t **k = (sw_val_t **)malloc(sizeof(sw_val_t *) * total);
        sw_val_t **v = (sw_val_t **)malloc(sizeof(sw_val_t *) * total);
        int n = 0;
        for (int i = 0; i < args[0]->v.map.count; i++) {
            k[n] = args[0]->v.map.keys[i];
            v[n] = args[0]->v.map.vals[i];
            n++;
        }
        for (int i = 0; i < args[1]->v.map.count; i++) {
            int overrode = 0;
            for (int j = 0; j < n; j++) {
                if (sw_val_equal(k[j], args[1]->v.map.keys[i])) {
                    v[j] = args[1]->v.map.vals[i]; overrode = 1; break;
                }
            }
            if (!overrode) {
                k[n] = args[1]->v.map.keys[i];
                v[n] = args[1]->v.map.vals[i];
                n++;
            }
        }
        sw_val_t *r = sw_val_map_new(k, v, n);
        free(k); free(v); return r;
    }
    if (strcmp(fname, "map_remove") == 0 && nargs >= 2 && args[0]->type == SW_VAL_MAP) {
        int n = args[0]->v.map.count;
        sw_val_t **k = (sw_val_t **)malloc(sizeof(sw_val_t *) * (n > 0 ? n : 1));
        sw_val_t **v = (sw_val_t **)malloc(sizeof(sw_val_t *) * (n > 0 ? n : 1));
        int kept = 0;
        for (int i = 0; i < n; i++) {
            if (sw_val_equal(args[0]->v.map.keys[i], args[1])) continue;
            k[kept] = args[0]->v.map.keys[i];
            v[kept] = args[0]->v.map.vals[i];
            kept++;
        }
        sw_val_t *r = sw_val_map_new(k, v, kept);
        free(k); free(v); return r;
    }

    /* read_key: the interpreter has no raw-mode stdin watch — report "no key".
     * Compiled binaries use the real _builtin_read_key. */
    if (strcmp(fname, "read_key") == 0) { return sw_val_nil(); }

    /* shell_managed: the interpreter has no select loop, so behave like a
     * blocking shell and report interrupted='false'. The COMPILED binary
     * (swarm-code) uses the real interruptible _builtin_shell_managed from
     * swarmrt_builtins_studio.h; this path only keeps `swc run`/REPL of
     * tools.sw resolving the name and returning the 3-tuple shape. */
    if (strcmp(fname, "shell_managed") == 0) {
        if (nargs < 1 || !args[0] || args[0]->type != SW_VAL_STRING) {
            /* Match the compiled builtin's arg-guard 3-tuple instead of
             * falling through to an "undefined function" error. */
            sw_val_t *ae[3];
            ae[0] = sw_val_int(-1);
            ae[1] = sw_val_string("error: shell_managed needs a string command");
            ae[2] = sw_val_atom("false");
            return sw_val_tuple(ae, 3);
        }
        sw_val_t *base = interp_extra_builtin(interp, "shell", args, nargs, line);
        if (base && base->type == SW_VAL_TUPLE && base->v.tuple.count == 2) {
            sw_val_t *it[3];
            it[0] = base->v.tuple.items[0];
            it[1] = base->v.tuple.items[1];
            it[2] = sw_val_atom("false");
            return sw_val_tuple(it, 3);
        }
        /* shell delegate failed (popen returned nil) — normalize to the same
         * launch-failure 3-tuple the compiled builtin emits so elem(r,0/1/2)
         * stays safe under `swc run`/REPL as well. */
        sw_val_t *ef[3];
        ef[0] = sw_val_int(-1);
        ef[1] = sw_val_string("error: failed to launch command");
        ef[2] = sw_val_atom("false");
        return sw_val_tuple(ef, 3);
    }

    /* === Shell ================================================= */
    if (strcmp(fname, "shell") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        size_t buflen = strlen(args[0]->v.str) + 16;
        char *cmd_buf = (char *)malloc(buflen);
        snprintf(cmd_buf, buflen, "%s 2>&1", args[0]->v.str);
        FILE *fp = popen(cmd_buf, "r"); free(cmd_buf);
        if (!fp) return sw_val_nil();
        size_t cap = 65536;
        char *out = (char *)malloc(cap);
        size_t got = fread(out, 1, cap - 1, fp); out[got] = '\0';
        int status = pclose(fp);
        sw_val_t *items[2];
        items[0] = sw_val_int(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        items[1] = sw_val_string(out);
        free(out);
        return sw_val_tuple(items, 2);
    }
    if (strcmp(fname, "shell_sandboxed") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        /* REPL skips sandbox-exec — just delegates to shell. The compiled
         * path still uses sandbox-exec/firejail. Note this in docs. */
        return interp_extra_builtin(interp, "shell", args, nargs, line);
    }
    if (strcmp(fname, "exec_argv") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
#ifdef _WIN32
        return sw_val_nil();
#else
        const char *cmd = args[0]->v.str;
        int nlist = 0;
        sw_val_t *lst = (nargs >= 2 && args[1] && args[1]->type == SW_VAL_LIST) ? args[1] : NULL;
        if (lst) nlist = lst->v.tuple.count;
        char **argv2 = (char **)malloc(sizeof(char *) * (1 + nlist + 1));
        if (!argv2) return sw_val_nil();
        argv2[0] = (char *)cmd;
        for (int i = 0; i < nlist; i++) {
            sw_val_t *elem = lst->v.tuple.items[i];
            argv2[1 + i] = (elem && elem->type == SW_VAL_STRING) ? elem->v.str : (char *)"";
        }
        argv2[1 + nlist] = NULL;
        /* pipe + fork + exec */
        int pfd[2];
        if (pipe(pfd) != 0) { free(argv2); return sw_val_nil(); }
        pid_t pid = fork();
        if (pid < 0) { close(pfd[0]); close(pfd[1]); free(argv2); return sw_val_nil(); }
        if (pid == 0) {
            /* child */
            close(pfd[0]);
            dup2(pfd[1], STDOUT_FILENO);
            dup2(pfd[1], STDERR_FILENO);
            close(pfd[1]);
            execvp(argv2[0], argv2);
            _exit(127);
        }
        close(pfd[1]);
        free(argv2);
        size_t cap = 65536, got = 0;
        char *buf = (char *)malloc(cap);
        if (!buf) { close(pfd[0]); waitpid(pid, NULL, 0); return sw_val_nil(); }
        char chunk[8192]; ssize_t rd;
        while ((rd = read(pfd[0], chunk, sizeof(chunk))) > 0) {
            if (got + (size_t)rd + 1 > cap) {
                while (got + (size_t)rd + 1 > cap) cap *= 2;
                char *tmp = (char *)realloc(buf, cap);
                if (!tmp) { free(buf); close(pfd[0]); waitpid(pid, NULL, 0); return sw_val_nil(); }
                buf = tmp;
            }
            memcpy(buf + got, chunk, (size_t)rd);
            got += (size_t)rd;
        }
        close(pfd[0]);
        buf[got] = '\0';
        int wstatus = 0; waitpid(pid, &wstatus, 0);
        int exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
        sw_val_t *items[2];
        items[0] = sw_val_int(exit_code);
        items[1] = sw_val_string(buf);
        free(buf);
        return sw_val_tuple(items, 2);
#endif
    }

    /* === SQLite ================================================ */
    if (strcmp(fname, "db_open") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        int slot = -1;
        for (int i = 0; i < _REPL_SQLITE_MAX; i++)
            if (!_repl_sqlite_db[i]) { slot = i; break; }
        if (slot < 0) return sw_val_int(-1);
        sqlite3 *db = NULL;
        if (sqlite3_open(args[0]->v.str, &db) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return sw_val_int(-1);
        }
        _repl_sqlite_db[slot] = db;
        return sw_val_int(slot);
    }
    if (strcmp(fname, "db_close") == 0 && nargs >= 1 && args[0]->type == SW_VAL_INT) {
        int slot = (int)args[0]->v.i;
        if (slot < 0 || slot >= _REPL_SQLITE_MAX || !_repl_sqlite_db[slot])
            return sw_val_atom("error");
        sqlite3_close(_repl_sqlite_db[slot]);
        _repl_sqlite_db[slot] = NULL;
        return sw_val_atom("ok");
    }
    if (strcmp(fname, "db_exec") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_INT && args[1]->type == SW_VAL_STRING) {
        int slot = (int)args[0]->v.i;
        if (slot < 0 || slot >= _REPL_SQLITE_MAX || !_repl_sqlite_db[slot])
            return sw_val_atom("error");
        /* 3-arg overload: db_exec(h, sql, [args]) — prepare + bind + step,
         * no rows returned (parity with codegen). */
        if (nargs >= 3 && args[2]->type == SW_VAL_LIST) {
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(_repl_sqlite_db[slot], args[1]->v.str, -1, &stmt, NULL) != SQLITE_OK)
                return sw_val_string(sqlite3_errmsg(_repl_sqlite_db[slot]));
            for (int i = 0; i < args[2]->v.tuple.count; i++) {
                sw_val_t *p = args[2]->v.tuple.items[i];
                if (p->type == SW_VAL_INT)         sqlite3_bind_int64(stmt, i + 1, p->v.i);
                else if (p->type == SW_VAL_FLOAT)  sqlite3_bind_double(stmt, i + 1, p->v.f);
                else if (p->type == SW_VAL_STRING) sqlite3_bind_text(stmt, i + 1, p->v.str, -1, SQLITE_TRANSIENT);
                else if (p->type == SW_VAL_ATOM)   sqlite3_bind_text(stmt, i + 1, p->v.str, -1, SQLITE_TRANSIENT);
                else if (p->type == SW_VAL_NIL)    sqlite3_bind_null(stmt, i + 1);
            }
            int rc;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { /* drain */ }
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE && rc != SQLITE_OK)
                return sw_val_string(sqlite3_errmsg(_repl_sqlite_db[slot]));
            return sw_val_atom("ok");
        }
        char *err = NULL;
        int rc = sqlite3_exec(_repl_sqlite_db[slot], args[1]->v.str, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            sw_val_t *r = sw_val_string(err ? err : "sqlite error");
            if (err) sqlite3_free(err);
            return r;
        }
        return sw_val_atom("ok");
    }
    if (strcmp(fname, "db_query") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_INT && args[1]->type == SW_VAL_STRING) {
        int slot = (int)args[0]->v.i;
        if (slot < 0 || slot >= _REPL_SQLITE_MAX || !_repl_sqlite_db[slot])
            return sw_val_list(NULL, 0);
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(_repl_sqlite_db[slot], args[1]->v.str, -1, &stmt, NULL) != SQLITE_OK)
            return sw_val_list(NULL, 0);
        if (nargs >= 3 && args[2]->type == SW_VAL_LIST) {
            for (int i = 0; i < args[2]->v.tuple.count; i++) {
                sw_val_t *p = args[2]->v.tuple.items[i];
                if (p->type == SW_VAL_INT)         sqlite3_bind_int64(stmt, i + 1, p->v.i);
                else if (p->type == SW_VAL_FLOAT)  sqlite3_bind_double(stmt, i + 1, p->v.f);
                else if (p->type == SW_VAL_STRING) sqlite3_bind_text(stmt, i + 1, p->v.str, -1, SQLITE_TRANSIENT);
                else if (p->type == SW_VAL_NIL)    sqlite3_bind_null(stmt, i + 1);
            }
        }
        sw_val_t *rows[1024]; int nrows = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && nrows < 1024) {
            int ncols = sqlite3_column_count(stmt);
            sw_val_t **keys = (sw_val_t **)malloc(sizeof(sw_val_t *) * ncols);
            sw_val_t **vals = (sw_val_t **)malloc(sizeof(sw_val_t *) * ncols);
            for (int c = 0; c < ncols; c++) {
                keys[c] = sw_val_string(sqlite3_column_name(stmt, c));
                int type = sqlite3_column_type(stmt, c);
                switch (type) {
                case SQLITE_INTEGER: vals[c] = sw_val_int(sqlite3_column_int64(stmt, c)); break;
                case SQLITE_FLOAT:   vals[c] = sw_val_float(sqlite3_column_double(stmt, c)); break;
                case SQLITE_TEXT:    vals[c] = sw_val_string((const char *)sqlite3_column_text(stmt, c)); break;
                case SQLITE_NULL:    vals[c] = sw_val_nil(); break;
                default:             vals[c] = sw_val_nil();
                }
            }
            rows[nrows++] = sw_val_map_new(keys, vals, ncols);
            free(keys); free(vals);
        }
        sqlite3_finalize(stmt);
        return sw_val_list(rows, nrows);
    }

    /* === ETS (value-aware hash tables) ========================= */
    if (strcmp(fname, "ets_new") == 0) {
        pthread_mutex_lock(&_interp_vets_meta);
        int id = _interp_vets_next_id++;
        if (id >= _INTERP_VETS_MAX_TABLES) {
            pthread_mutex_unlock(&_interp_vets_meta);
            return sw_val_nil();
        }
        memset(&_interp_vets_tables[id], 0, sizeof(_interp_vets_table_t));
        pthread_rwlock_init(&_interp_vets_tables[id].lock, NULL);
        _interp_vets_tables[id].active = 1;
        pthread_mutex_unlock(&_interp_vets_meta);
        return sw_val_int((int64_t)id);
    }
    if (strcmp(fname, "ets_put") == 0 && nargs >= 3 && args[0]->type == SW_VAL_INT) {
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_atom("error");
        _interp_vets_table_t *t = &_interp_vets_tables[id];
        uint32_t bucket = _interp_vets_hash(args[1]);
        pthread_rwlock_wrlock(&t->lock);
        _interp_vets_entry_t *e = t->buckets[bucket];
        while (e) {
            if (_interp_vets_key_eq(e->key, args[1])) {
                e->value = args[2];
                pthread_rwlock_unlock(&t->lock);
                return sw_val_atom("ok");
            }
            e = e->next;
        }
        _interp_vets_entry_t *ne = (_interp_vets_entry_t *)malloc(sizeof(_interp_vets_entry_t));
        ne->key = args[1]; ne->value = args[2]; ne->next = t->buckets[bucket];
        t->buckets[bucket] = ne;
        pthread_rwlock_unlock(&t->lock);
        return sw_val_atom("ok");
    }
    if (strcmp(fname, "ets_get") == 0 && nargs >= 2 && args[0]->type == SW_VAL_INT) {
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_nil();
        _interp_vets_table_t *t = &_interp_vets_tables[id];
        uint32_t bucket = _interp_vets_hash(args[1]);
        pthread_rwlock_rdlock(&t->lock);
        _interp_vets_entry_t *e = t->buckets[bucket];
        while (e) {
            if (_interp_vets_key_eq(e->key, args[1])) {
                sw_val_t *v = e->value;
                pthread_rwlock_unlock(&t->lock);
                return v ? v : sw_val_nil();
            }
            e = e->next;
        }
        pthread_rwlock_unlock(&t->lock);
        return sw_val_nil();
    }
    if (strcmp(fname, "ets_delete") == 0 && nargs >= 2 && args[0]->type == SW_VAL_INT) {
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_atom("error");
        _interp_vets_table_t *t = &_interp_vets_tables[id];
        uint32_t bucket = _interp_vets_hash(args[1]);
        pthread_rwlock_wrlock(&t->lock);
        _interp_vets_entry_t **pp = &t->buckets[bucket];
        while (*pp) {
            if (_interp_vets_key_eq((*pp)->key, args[1])) {
                _interp_vets_entry_t *dead = *pp; *pp = dead->next; free(dead);
                pthread_rwlock_unlock(&t->lock);
                return sw_val_atom("ok");
            }
            pp = &(*pp)->next;
        }
        pthread_rwlock_unlock(&t->lock);
        return sw_val_atom("ok");
    }
    if (strcmp(fname, "ets_take") == 0 && nargs >= 2 && args[0]->type == SW_VAL_INT) {
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_nil();
        _interp_vets_table_t *t = &_interp_vets_tables[id];
        uint32_t bucket = _interp_vets_hash(args[1]);
        pthread_rwlock_wrlock(&t->lock);
        _interp_vets_entry_t **pp = &t->buckets[bucket];
        while (*pp) {
            if (_interp_vets_key_eq((*pp)->key, args[1])) {
                _interp_vets_entry_t *dead = *pp;
                sw_val_t *val = dead->value;
                *pp = dead->next; free(dead);
                pthread_rwlock_unlock(&t->lock);
                return val ? val : sw_val_nil();
            }
            pp = &(*pp)->next;
        }
        pthread_rwlock_unlock(&t->lock);
        return sw_val_nil();
    }
    if (strcmp(fname, "ets_update_counter") == 0 && nargs >= 4 &&
        args[0]->type == SW_VAL_INT && args[2]->type == SW_VAL_INT && args[3]->type == SW_VAL_INT) {
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_atom("error");
        _interp_vets_table_t *t = &_interp_vets_tables[id];
        uint32_t bucket = _interp_vets_hash(args[1]);
        int64_t delta = args[2]->v.i, initial = args[3]->v.i;
        pthread_rwlock_wrlock(&t->lock);
        _interp_vets_entry_t *e = t->buckets[bucket];
        while (e) {
            if (_interp_vets_key_eq(e->key, args[1])) {
                if (e->value && e->value->type == SW_VAL_INT) {
                    int64_t result = e->value->v.i + delta;
                    e->value = sw_val_int(result);
                    pthread_rwlock_unlock(&t->lock);
                    return sw_val_int(result);
                }
                pthread_rwlock_unlock(&t->lock);
                return sw_val_atom("error");
            }
            e = e->next;
        }
        int64_t seeded = initial + delta;
        sw_val_t *stored = sw_val_int(seeded);
        _interp_vets_entry_t *ne = (_interp_vets_entry_t *)malloc(sizeof(_interp_vets_entry_t));
        ne->key = args[1]; ne->value = stored; ne->next = t->buckets[bucket];
        t->buckets[bucket] = ne;
        pthread_rwlock_unlock(&t->lock);
        return sw_val_int(seeded);
    }
    if (strcmp(fname, "ets_cas") == 0 && nargs >= 4 && args[0]->type == SW_VAL_INT) {
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_atom("false");
        _interp_vets_table_t *t = &_interp_vets_tables[id];
        uint32_t bucket = _interp_vets_hash(args[1]);
        pthread_rwlock_wrlock(&t->lock);
        _interp_vets_entry_t *e = t->buckets[bucket];
        while (e) {
            if (_interp_vets_key_eq(e->key, args[1])) {
                if (_interp_vets_key_eq(e->value, args[2])) {
                    e->value = args[3];
                    pthread_rwlock_unlock(&t->lock);
                    return sw_val_atom("true");
                }
                pthread_rwlock_unlock(&t->lock);
                return sw_val_atom("false");
            }
            e = e->next;
        }
        pthread_rwlock_unlock(&t->lock);
        return sw_val_atom("false");
    }
    /* ets_update(t, k, fun) — atomically apply a 1-arg sw lambda to the
     * current value and write the result back.  The lambda is called
     * without holding the rwlock (to allow arbitrary eval depth); the
     * write-back re-acquires it and re-scans to handle the dropped-lock
     * window.  Returns the new value, or 'error' if the key is missing or
     * the third argument is not a callable lambda. */
    if (strcmp(fname, "ets_update") == 0 && nargs >= 3 && args[0]->type == SW_VAL_INT) {
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_atom("error");
        /* Third argument must be a closure with an AST body (interpreter lambda). */
        sw_val_t *fn_val = args[2];
        if (!fn_val || fn_val->type != SW_VAL_FUN || !fn_val->v.fun.body)
            return sw_val_atom("error");

        _interp_vets_table_t *t = &_interp_vets_tables[id];
        uint32_t bucket = _interp_vets_hash(args[1]);

        /* Read the current value under the read lock. */
        pthread_rwlock_rdlock(&t->lock);
        sw_val_t *old_val = NULL;
        _interp_vets_entry_t *e = t->buckets[bucket];
        while (e) {
            if (_interp_vets_key_eq(e->key, args[1])) { old_val = e->value; break; }
            e = e->next;
        }
        pthread_rwlock_unlock(&t->lock);

        if (!old_val) return sw_val_atom("error");  /* key not found */

        /* Invoke the lambda without holding any lock. */
        node_t *fn_node = (node_t *)fn_val->v.fun.body;
        sw_env_t *fenv = env_new(fn_val->v.fun.closure_env
                                  ? fn_val->v.fun.closure_env
                                  : interp->global_env);
        if (fn_node->v.fun.nparams >= 1)
            env_set(fenv, fn_node->v.fun.params[0], old_val);
        sw_val_t *new_val = eval(interp, fn_node->v.fun.body, fenv);
        env_free(fenv);

        if (interp->error) return sw_val_nil();

        /* Write the result back under the write lock (re-scan: lock was
         * dropped during eval, concurrent ets_put may have changed things). */
        pthread_rwlock_wrlock(&t->lock);
        e = t->buckets[bucket];
        while (e) {
            if (_interp_vets_key_eq(e->key, args[1])) {
                e->value = new_val;
                pthread_rwlock_unlock(&t->lock);
                return new_val;
            }
            e = e->next;
        }
        pthread_rwlock_unlock(&t->lock);
        return sw_val_atom("error");  /* key disappeared between read and write */
    }
    if (strcmp(fname, "ets_count") == 0 && nargs >= 1 && args[0]->type == SW_VAL_INT) {
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_int(-1);
        _interp_vets_table_t *t = &_interp_vets_tables[id];
        pthread_rwlock_rdlock(&t->lock);
        int count = 0;
        for (int bi = 0; bi < _INTERP_VETS_BUCKETS; bi++) {
            for (_interp_vets_entry_t *e = t->buckets[bi]; e; e = e->next) count++;
        }
        pthread_rwlock_unlock(&t->lock);
        return sw_val_int(count);
    }
    if (strcmp(fname, "ets_list") == 0) {
        /* ets_list(table_id) → list of {key, value} tuples — match the
         * docs (docs/SW_LANGUAGE.md) and the compiled path. Previously
         * this ignored its argument and returned the list of active
         * table IDs, a third divergent behavior. */
        if (nargs < 1 || !args[0] || args[0]->type != SW_VAL_INT)
            return sw_val_list(NULL, 0);
        int id = (int)args[0]->v.i;
        if (id < 0 || id >= _INTERP_VETS_MAX_TABLES || !_interp_vets_tables[id].active)
            return sw_val_list(NULL, 0);
        _interp_vets_table_t *t = &_interp_vets_tables[id];

        int cap = 64, cnt = 0;
        sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
        pthread_rwlock_rdlock(&t->lock);
        for (int bi = 0; bi < _INTERP_VETS_BUCKETS; bi++) {
            for (_interp_vets_entry_t *e = t->buckets[bi]; e; e = e->next) {
                if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
                sw_val_t *pair[2] = { e->key, e->value };
                items[cnt++] = sw_val_tuple(pair, 2);
            }
        }
        pthread_rwlock_unlock(&t->lock);
        sw_val_t *r = sw_val_list(items, cnt);
        free(items);
        return r;
    }

    /* === base64 (string-oriented, parity with codegen path) =====
     * NOTE: like the compiled _builtin_base64_*, these treat values as
     * NUL-terminated C strings, so decoded binary truncates at the first
     * NUL. This is the documented limitation the voice plan locks in. */
    if (strcmp(fname, "base64_encode") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        char *b64 = _sw_audio_b64_encode((const uint8_t *)args[0]->v.str,
                                         strlen(args[0]->v.str));
        if (!b64) return sw_val_string("");
        sw_val_t *r = sw_val_string(b64); free(b64); return r;
    }
    if (strcmp(fname, "base64_decode") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        size_t dlen = 0;
        uint8_t *raw = _sw_audio_b64_decode(args[0]->v.str, &dlen);
        if (!raw) return sw_val_nil();
        /* NUL-terminate; string truncates at first NUL (documented). */
        char *s = (char *)malloc(dlen + 1);
        if (!s) { free(raw); return sw_val_nil(); }
        memcpy(s, raw, dlen); s[dlen] = 0; free(raw);
        sw_val_t *r = sw_val_string(s); free(s); return r;
    }

    /* === ed25519_verify (TLS-gated, openssl EVP) — REPL/codegen parity ===
     * Pure function, so it runs identically in the interpreter. Delegates to
     * the same _builtin_ed25519_verify used by compiled code: 'true'|'false'
     * under TLS, nil + stderr note without. */
    if (strcmp(fname, "ed25519_verify") == 0 && nargs >= 3) {
        return _sw_lang_ed25519_verify(args, nargs);
    }

    /* === Audio codecs (base64 in/out, fully binary-safe) ======== */
    if (strcmp(fname, "audio_ulaw_to_pcm16") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        size_t inlen = 0; uint8_t *ulaw = _sw_audio_b64_decode(args[0]->v.str, &inlen);
        if (!ulaw) return sw_val_nil();
        size_t pcmlen = 0; uint8_t *pcm = _sw_ulaw_to_pcm16(ulaw, inlen, &pcmlen);
        free(ulaw); if (!pcm) return sw_val_nil();
        char *b64 = _sw_audio_b64_encode(pcm, pcmlen); free(pcm);
        if (!b64) return sw_val_nil();
        sw_val_t *r = sw_val_string(b64); free(b64); return r;
    }
    if (strcmp(fname, "audio_pcm16_to_ulaw") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        size_t inlen = 0; uint8_t *pcm = _sw_audio_b64_decode(args[0]->v.str, &inlen);
        if (!pcm) return sw_val_nil();
        size_t ulen = 0; uint8_t *ulaw = _sw_pcm16_to_ulaw(pcm, inlen, &ulen);
        free(pcm); if (!ulaw) return sw_val_nil();
        char *b64 = _sw_audio_b64_encode(ulaw, ulen); free(ulaw);
        if (!b64) return sw_val_nil();
        sw_val_t *r = sw_val_string(b64); free(b64); return r;
    }
    if (strcmp(fname, "audio_resample") == 0 && nargs >= 3 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_INT && args[2]->type == SW_VAL_INT) {
        size_t inlen = 0; uint8_t *pcm = _sw_audio_b64_decode(args[0]->v.str, &inlen);
        if (!pcm) return sw_val_nil();
        size_t outlen = 0;
        uint8_t *out = _sw_pcm16_resample(pcm, inlen, (int)args[1]->v.i, (int)args[2]->v.i, &outlen);
        free(pcm); if (!out) return sw_val_nil();
        char *b64 = _sw_audio_b64_encode(out, outlen); free(out);
        if (!b64) return sw_val_nil();
        sw_val_t *r = sw_val_string(b64); free(b64); return r;
    }

    /* === SW_VAL_BYTES builtins (NUL-safe; parity with codegen path) === */
    if (strcmp(fname, "bytes_from_base64") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        size_t dlen = 0; uint8_t *raw = _sw_audio_b64_decode(args[0]->v.str, &dlen);
        if (!raw) return sw_val_nil();
        sw_val_t *r = sw_val_bytes(raw, dlen); free(raw); return r;
    }
    if (strcmp(fname, "bytes_to_base64") == 0 && nargs >= 1 && args[0]->type == SW_VAL_BYTES) {
        char *b64 = _sw_audio_b64_encode(args[0]->v.bytes.data, args[0]->v.bytes.len);
        if (!b64) return sw_val_string("");
        sw_val_t *r = sw_val_string(b64); free(b64); return r;
    }
    if (strcmp(fname, "byte_size") == 0 && nargs >= 1) {
        if (args[0]->type != SW_VAL_BYTES) return sw_val_int(0);
        return sw_val_int((int64_t)args[0]->v.bytes.len);
    }
    if (strcmp(fname, "byte_at") == 0 && nargs >= 2) {
        if (args[0]->type != SW_VAL_BYTES || args[1]->type != SW_VAL_INT) {
            interp_raise_panic(interp, line, "byte_at: expected (bytes, int)");
            return sw_val_nil();
        }
        int64_t i = args[1]->v.i;
        if (i < 0 || (size_t)i >= args[0]->v.bytes.len) {
            interp_raise_panic(interp, line, "byte_at: index out of range (got %lld, size %zu)",
                               (long long)i, args[0]->v.bytes.len);
            return sw_val_nil();
        }
        return sw_val_int((int64_t)args[0]->v.bytes.data[i]);
    }
    if (strcmp(fname, "byte_slice") == 0 && nargs >= 3 &&
        args[0]->type == SW_VAL_BYTES && args[1]->type == SW_VAL_INT && args[2]->type == SW_VAL_INT) {
        size_t total = args[0]->v.bytes.len;
        int64_t start = args[1]->v.i, want = args[2]->v.i;
        if (start < 0 || want < 0 || (size_t)start >= total) return sw_val_bytes(NULL, 0);
        size_t avail = total - (size_t)start;
        size_t take = ((size_t)want < avail) ? (size_t)want : avail;
        return sw_val_bytes(args[0]->v.bytes.data + start, take);
    }
    if (strcmp(fname, "bytes_concat") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_BYTES && args[1]->type == SW_VAL_BYTES) {
        size_t la = args[0]->v.bytes.len, lb = args[1]->v.bytes.len, tot = la + lb;
        uint8_t *tmp = (uint8_t *)malloc(tot ? tot : 1);
        if (la) memcpy(tmp, args[0]->v.bytes.data, la);
        if (lb) memcpy(tmp + la, args[1]->v.bytes.data, lb);
        sw_val_t *r = sw_val_bytes(tmp, tot); free(tmp); return r;
    }
    if (strcmp(fname, "string_to_bytes") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        return sw_val_bytes((const uint8_t *)args[0]->v.str, strlen(args[0]->v.str));
    }
    if (strcmp(fname, "string_chars") == 0 && nargs >= 1) {
        /* Codepoint-aware split — twin of _builtin_string_chars (studio.h):
         * list of single-codepoint strings via UTF-8 lead-byte lengths;
         * invalid leads degrade to one byte. Conform gate t04 holds the
         * two paths identical. */
        if (args[0]->type != SW_VAL_STRING) return sw_val_list(NULL, 0);
        const char *s = args[0]->v.str ? args[0]->v.str : "";
        size_t len = strlen(s);
        sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * (len + 1));
        int cnt = 0;
        size_t i = 0;
        while (i < len) {
            unsigned char c = (unsigned char)s[i];
            int sl = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                     ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
            if (i + (size_t)sl > len) sl = 1;
            char buf[8];
            memcpy(buf, s + i, sl);
            buf[sl] = 0;
            items[cnt++] = sw_val_string(buf);
            i += (size_t)sl;
        }
        sw_val_t *r = sw_val_list(items, cnt);
        free(items);
        return r;
    }
    if (strcmp(fname, "bytes_to_string") == 0 && nargs >= 1 && args[0]->type == SW_VAL_BYTES) {
        size_t len = args[0]->v.bytes.len;
        char *tmp = (char *)malloc(len + 1);
        if (len) memcpy(tmp, args[0]->v.bytes.data, len);
        tmp[len] = 0;
        sw_val_t *r = sw_val_string(tmp); free(tmp); return r;
    }
    /* bytes_from_ints([n0, n1, ...]) → bytes (each masked 0..255). */
    if (strcmp(fname, "bytes_from_ints") == 0 && nargs >= 1 && args[0]->type == SW_VAL_LIST) {
        int cnt = args[0]->v.tuple.count;
        uint8_t *buf = (uint8_t *)malloc(cnt ? (size_t)cnt : 1);
        for (int i = 0; i < cnt; i++) {
            sw_val_t *e = args[0]->v.tuple.items[i];
            buf[i] = (e && e->type == SW_VAL_INT) ? (uint8_t)(e->v.i & 0xFF) : 0;
        }
        sw_val_t *r = sw_val_bytes(buf, (size_t)cnt); free(buf); return r;
    }
    /* byte(n) → bytes of length 1 holding n & 0xFF. */
    if (strcmp(fname, "byte") == 0 && nargs >= 1 && args[0]->type == SW_VAL_INT) {
        uint8_t b = (uint8_t)(args[0]->v.i & 0xFF);
        return sw_val_bytes(&b, 1);
    }

    /* === char codes (parity with codegen) ====================== */
    if (strcmp(fname, "ord") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        const unsigned char *s = (const unsigned char *)args[0]->v.str;
        return sw_val_int(s[0] ? (int64_t)s[0] : -1);
    }
    if (strcmp(fname, "codepoint_at") == 0 && nargs >= 2 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_INT) {
        const unsigned char *s = (const unsigned char *)args[0]->v.str;
        int64_t i = args[1]->v.i;
        size_t len = strlen((const char *)s);
        if (i < 0 || (size_t)i >= len) return sw_val_int(-1);
        return sw_val_int((int64_t)s[i]);
    }

    /* to_int / to_float (parse strings too) + uuid + now_iso — shared helpers
     * so interp and compiled agree. (Caught BEFORE the math block so to_float
     * here handles strings, not just numbers.) */
    if (strcmp(fname, "to_int") == 0 && nargs >= 1)   return sw_coerce_int(args[0]);
    if (strcmp(fname, "to_float") == 0 && nargs >= 1) return sw_coerce_float(args[0]);
    if (strcmp(fname, "uuid") == 0)                   return sw_make_uuid();
    if (strcmp(fname, "now_iso") == 0)                return sw_now_iso();

    /* === Math (libm-backed; parity with codegen) =============== */
    if ((strcmp(fname, "to_float") == 0 || strcmp(fname, "math_sqrt") == 0 ||
         strcmp(fname, "math_sin") == 0 || strcmp(fname, "math_cos") == 0 ||
         strcmp(fname, "math_exp") == 0 || strcmp(fname, "math_log") == 0 ||
         strcmp(fname, "math_floor") == 0 || strcmp(fname, "math_ceil") == 0 ||
         strcmp(fname, "math_round") == 0) && nargs >= 1 &&
        (args[0]->type == SW_VAL_INT || args[0]->type == SW_VAL_FLOAT)) {
        double x = (args[0]->type == SW_VAL_INT) ? (double)args[0]->v.i : args[0]->v.f;
        if (strcmp(fname, "to_float") == 0)   return sw_val_float(x);
        if (strcmp(fname, "math_sqrt") == 0)  return sw_val_float(sqrt(x));
        if (strcmp(fname, "math_sin") == 0)   return sw_val_float(sin(x));
        if (strcmp(fname, "math_cos") == 0)   return sw_val_float(cos(x));
        if (strcmp(fname, "math_exp") == 0)   return sw_val_float(exp(x));
        if (strcmp(fname, "math_log") == 0)   return sw_val_float(log(x));
        if (strcmp(fname, "math_floor") == 0) return sw_val_int((int64_t)floor(x));
        if (strcmp(fname, "math_ceil") == 0)  return sw_val_int((int64_t)ceil(x));
        if (strcmp(fname, "math_round") == 0) return sw_val_int((int64_t)llround(x));
    }
    if (strcmp(fname, "math_pow") == 0 && nargs >= 2 &&
        (args[0]->type == SW_VAL_INT || args[0]->type == SW_VAL_FLOAT) &&
        (args[1]->type == SW_VAL_INT || args[1]->type == SW_VAL_FLOAT)) {
        double base = (args[0]->type == SW_VAL_INT) ? (double)args[0]->v.i : args[0]->v.f;
        double ex   = (args[1]->type == SW_VAL_INT) ? (double)args[1]->v.i : args[1]->v.f;
        return sw_val_float(pow(base, ex));
    }

    /* === Bytes-native audio codec twins (NUL-safe; parity) ====== */
    if (strcmp(fname, "audio_ulaw_to_pcm16_b") == 0 && nargs >= 1 && args[0]->type == SW_VAL_BYTES) {
        size_t pcmlen = 0;
        uint8_t *pcm = _sw_ulaw_to_pcm16(args[0]->v.bytes.data, args[0]->v.bytes.len, &pcmlen);
        if (!pcm) return sw_val_nil();
        sw_val_t *r = sw_val_bytes(pcm, pcmlen); free(pcm); return r;
    }
    if (strcmp(fname, "audio_pcm16_to_ulaw_b") == 0 && nargs >= 1 && args[0]->type == SW_VAL_BYTES) {
        size_t ulen = 0;
        uint8_t *ulaw = _sw_pcm16_to_ulaw(args[0]->v.bytes.data, args[0]->v.bytes.len, &ulen);
        if (!ulaw) return sw_val_nil();
        sw_val_t *r = sw_val_bytes(ulaw, ulen); free(ulaw); return r;
    }
    if (strcmp(fname, "audio_resample_b") == 0 && nargs >= 3 &&
        args[0]->type == SW_VAL_BYTES && args[1]->type == SW_VAL_INT && args[2]->type == SW_VAL_INT) {
        size_t outlen = 0;
        uint8_t *out = _sw_pcm16_resample(args[0]->v.bytes.data, args[0]->v.bytes.len,
                                          (int)args[1]->v.i, (int)args[2]->v.i, &outlen);
        if (!out) return sw_val_nil();
        sw_val_t *r = sw_val_bytes(out, outlen); free(out); return r;
    }

    /* === Process-scheduler primitives — WIRED to native scheduler =====
     * These plain BUILTIN calls now run on real interpreter fibers with a
     * live scheduler + real pids (spawn/self/send already work), so we wire
     * them directly to the native sw_* primitives — exact arg/return
     * marshalling replicated from the compiled-path _builtin_* wrappers in
     * swarmrt_builtins_studio.h (which is NOT included here; bodies inlined).
     * NB: PID args guard SW_VAL_PID only (these primitives are local-only;
     * a SW_VAL_REMOTE_PID's union .pid would be garbage). */

    /* link/unlink/trap_exit/exit_proc — {'EXIT',from,reason} delivery */
    if (strcmp(fname, "link") == 0) {
        if (nargs < 1 || args[0]->type != SW_VAL_PID) return sw_val_atom("error");
        sw_link(args[0]->v.pid);
        return sw_val_atom("ok");
    }
    if (strcmp(fname, "unlink") == 0) {
        if (nargs < 1 || args[0]->type != SW_VAL_PID) return sw_val_atom("error");
        return sw_val_atom(sw_unlink(args[0]->v.pid) == 0 ? "ok" : "error");
    }
    if (strcmp(fname, "trap_exit") == 0) {
        if (nargs < 1 || !args[0]) return sw_val_atom("error");
        int v = 0;
        if (args[0]->type == SW_VAL_ATOM && args[0]->v.str && strcmp(args[0]->v.str, "true") == 0) v = 1;
        else if (args[0]->type == SW_VAL_INT && args[0]->v.i != 0) v = 1;
        sw_process_flag(SW_FLAG_TRAP_EXIT, v);
        return sw_val_atom("ok");
    }
    if (strcmp(fname, "exit_proc") == 0) {
        if (nargs < 1 || args[0]->type != SW_VAL_PID) return sw_val_atom("error");
        int reason = 2;  /* default: 'killed' */
        if (nargs >= 2 && args[1] && args[1]->type == SW_VAL_ATOM && args[1]->v.str) {
            if (strcmp(args[1]->v.str, "normal") == 0) reason = 0;
            else if (strcmp(args[1]->v.str, "killed") == 0) reason = 2;
            else reason = 3;
        }
        sw_process_kill(args[0]->v.pid, reason);
        return sw_val_atom("ok");
    }

    /* monitor/demonitor — same machinery spawn_monitor already uses */
    if (strcmp(fname, "monitor") == 0) {
        if (nargs < 1 || args[0]->type != SW_VAL_PID) return sw_val_nil();
        return sw_val_int((int64_t)sw_monitor(args[0]->v.pid));
    }
    if (strcmp(fname, "demonitor") == 0) {
        if (nargs < 1 || args[0]->type != SW_VAL_INT) return sw_val_atom("error");
        return sw_val_atom(sw_demonitor((uint64_t)args[0]->v.i) == 0 ? "ok" : "error");
    }

    /* register/whereis — name arg accepts STRING or ATOM (both use .str) */
    if (strcmp(fname, "register") == 0) {
        if (nargs < 2 || !args[0]->v.str ||
            (args[0]->type != SW_VAL_STRING && args[0]->type != SW_VAL_ATOM) ||
            args[1]->type != SW_VAL_PID)
            return sw_val_atom("error");
        return sw_val_atom(sw_register(args[0]->v.str, args[1]->v.pid) == 0 ? "ok" : "error");
    }
    if (strcmp(fname, "whereis") == 0) {
        if (nargs < 1 || !args[0]->v.str ||
            (args[0]->type != SW_VAL_STRING && args[0]->type != SW_VAL_ATOM))
            return sw_val_nil();
        sw_process_t *p = sw_whereis(args[0]->v.str);
        return p ? sw_val_pid(p) : sw_val_nil();
    }

    /* process_info / process_list / registered — pure reads */
    if (strcmp(fname, "process_info") == 0) {
        if (nargs < 1 || args[0]->type != SW_VAL_PID || !args[0]->v.pid)
            return sw_val_nil();
        sw_process_t *proc = args[0]->v.pid;
        sw_val_t *keys[12], *vals[12];
        int c = 0;
        keys[c] = sw_val_atom("pid");     vals[c] = sw_val_int((int64_t)proc->pid); c++;
        keys[c] = sw_val_atom("status");
        /* Load the _Atomic state into a plain enum before the switch — some
         * clang versions reject switching directly on an _Atomic-qualified type
         * ("statement requires expression of integer type"). */
        sw_proc_state_t pi_state = proc->state;
        switch (pi_state) {
            case SW_PROC_RUNNING:  vals[c] = sw_val_atom("running"); break;
            case SW_PROC_RUNNABLE: vals[c] = sw_val_atom("runnable"); break;
            case SW_PROC_WAITING:  vals[c] = sw_val_atom("waiting"); break;
            case SW_PROC_EXITING:  vals[c] = sw_val_atom("exiting"); break;
            default:               vals[c] = sw_val_atom("unknown"); break;
        }
        c++;
        keys[c] = sw_val_atom("reductions"); vals[c] = sw_val_int((int64_t)proc->reductions_done); c++;
        keys[c] = sw_val_atom("messages");   vals[c] = sw_val_int((int64_t)proc->mailbox.count); c++;
        keys[c] = sw_val_atom("heap_used");  vals[c] = sw_val_int((int64_t)(proc->heap.top - proc->heap.start)); c++;
        keys[c] = sw_val_atom("heap_size");  vals[c] = sw_val_int((int64_t)proc->heap.size); c++;
        /* memory / mailbox_len / messages_sent / messages_recv — same
         * fields and same read discipline as the compiled builtin
         * (_builtin_process_info in swarmrt_builtins_studio.h): varena
         * bytes under link_lock (process_destroy detaches it under that
         * lock), the rest best-effort. */
        {
            int64_t mem = 0;
            extern sw_swarm_t *g_swarm;
            if (g_swarm) {
                pthread_mutex_lock(&g_swarm->link_lock);
                struct sw_value_arena *va = proc->varena;
                if (va) mem = (int64_t)va->total_bytes;
                pthread_mutex_unlock(&g_swarm->link_lock);
            }
            keys[c] = sw_val_atom("memory"); vals[c] = sw_val_int(mem); c++;
        }
        keys[c] = sw_val_atom("mailbox_len");
        vals[c] = sw_val_int((int64_t)atomic_load_explicit(&proc->mb_len, memory_order_relaxed)); c++;
        keys[c] = sw_val_atom("messages_sent"); vals[c] = sw_val_int((int64_t)proc->messages_sent); c++;
        keys[c] = sw_val_atom("messages_recv"); vals[c] = sw_val_int((int64_t)proc->messages_recv); c++;
        if (proc->parent)
            { keys[c] = sw_val_atom("parent"); vals[c] = sw_val_int((int64_t)proc->parent->pid); c++; }
        {
            char _nm[SW_REG_NAME_MAX];
            int _have = 0;
            extern sw_swarm_t *g_swarm;
            if (g_swarm) {
                pthread_rwlock_rdlock(&g_swarm->registry.lock);
                sw_reg_entry_t *re = atomic_load_explicit(&proc->reg_entry, memory_order_acquire);
                if (re) { strncpy(_nm, re->name, SW_REG_NAME_MAX - 1); _nm[SW_REG_NAME_MAX - 1] = '\0'; _have = 1; }
                pthread_rwlock_unlock(&g_swarm->registry.lock);
            }
            if (_have) { keys[c] = sw_val_atom("name"); vals[c] = sw_val_string(_nm); c++; }
        }
        return sw_val_map_new(keys, vals, c);
    }
    if (strcmp(fname, "process_list") == 0) {
        extern sw_swarm_t *g_swarm;
        if (!g_swarm) return sw_val_list(NULL, 0);
        int cap = 256, cnt = 0;
        sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
        sw_process_t *slab = (sw_process_t *)g_swarm->arena.proc_slab;
        for (uint32_t i = 0; i < g_swarm->arena.proc_capacity; i++) {
            if (slab[i].state != SW_PROC_FREE && slab[i].state != SW_PROC_EXITING) {
                if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
                items[cnt++] = sw_val_pid(&slab[i]);
            }
        }
        sw_val_t *r = sw_val_list(items, cnt);
        free(items);
        return r;
    }
    if (strcmp(fname, "registered") == 0) {
        extern sw_swarm_t *g_swarm;
        if (!g_swarm || !g_swarm->registry.buckets) return sw_val_list(NULL, 0);
        int cap = 64, cnt = 0;
        sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
        pthread_rwlock_rdlock(&g_swarm->registry.lock);
        for (uint32_t i = 0; i < g_swarm->registry.num_buckets; i++) {
            sw_reg_entry_t *e = g_swarm->registry.buckets[i];
            while (e) {
                if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
                sw_val_t **pair = (sw_val_t **)malloc(sizeof(sw_val_t *) * 2);
                pair[0] = sw_val_string(e->name);
                pair[1] = sw_val_pid(e->proc);
                items[cnt++] = sw_val_tuple(pair, 2);
                free(pair);
                e = e->next;
            }
        }
        pthread_rwlock_unlock(&g_swarm->registry.lock);
        sw_val_t *r = sw_val_list(items, cnt);
        free(items);
        return r;
    }

    /* swarm_stats() — node-level runtime metrics map; pure read, same
     * fields as the compiled _builtin_swarm_stats (see the field-by-field
     * doc in swarmrt_builtins_studio.h). nil when no runtime is booted. */
    if (strcmp(fname, "swarm_stats") == 0) {
        extern sw_swarm_t *g_swarm;
        if (!g_swarm) return sw_val_nil();

        int64_t live = 0;
        sw_process_t *slab = (sw_process_t *)g_swarm->arena.proc_slab;
        for (uint32_t i = 0; i < g_swarm->arena.proc_capacity; i++) {
            sw_proc_state_t st = slab[i].state;
            if (st != SW_PROC_FREE && st != SW_PROC_EXITING) live++;
        }

        int64_t overflow;
        pthread_mutex_lock(&g_swarm->overflow_rq.lock);
        overflow = (int64_t)g_swarm->overflow_rq.count;
        pthread_mutex_unlock(&g_swarm->overflow_rq.lock);

        uint32_t nsched = g_swarm->num_schedulers;
        sw_val_t **scheds = (sw_val_t **)malloc(sizeof(sw_val_t *) * (nsched ? nsched : 1));
        for (uint32_t i = 0; i < nsched; i++) {
            sw_scheduler_t *sc = g_swarm->schedulers[i];
            sw_val_t *sk[5], *sv[5];
            sk[0] = sw_val_atom("id");         sv[0] = sw_val_int((int64_t)sc->id);
            sk[1] = sw_val_atom("procs_run");  sv[1] = sw_val_int((int64_t)sc->procs_run);
            sk[2] = sw_val_atom("loop_iters"); sv[2] = sw_val_int((int64_t)sc->loop_iters);
            sk[3] = sw_val_atom("idle_waits"); sv[3] = sw_val_int((int64_t)sc->idle_waits);
            sk[4] = sw_val_atom("steals");     sv[4] = sw_val_int((int64_t)sc->steal_attempts);
            scheds[i] = sw_val_map_new(sk, sv, 5);
        }
        sw_val_t *sched_list = sw_val_list(scheds, (int)nsched);
        free(scheds);

        sw_val_t *keys[11], *vals[11];
        int c = 0;
        keys[c] = sw_val_atom("processes");       vals[c] = sw_val_int(live); c++;
        keys[c] = sw_val_atom("schedulers");      vals[c] = sw_val_int((int64_t)nsched); c++;
        keys[c] = sw_val_atom("spawns");          vals[c] = sw_val_int((int64_t)atomic_load_explicit(&g_swarm->total_spawns, memory_order_relaxed)); c++;
        keys[c] = sw_val_atom("crashes");         vals[c] = sw_val_int((int64_t)sw_proc_crashes()); c++;
        keys[c] = sw_val_atom("restarts");        vals[c] = sw_val_int((int64_t)sw_restarts_total()); c++;
        keys[c] = sw_val_atom("mailbox_dropped"); vals[c] = sw_val_int((int64_t)sw_mailbox_dropped()); c++;
        keys[c] = sw_val_atom("msgsize_dropped"); vals[c] = sw_val_int((int64_t)sw_msgsize_dropped()); c++;
        keys[c] = sw_val_atom("overflow_queue");  vals[c] = sw_val_int(overflow); c++;
        keys[c] = sw_val_atom("draining");        vals[c] = sw_val_atom(sw_is_draining() ? "true" : "false"); c++;
        keys[c] = sw_val_atom("scheduler_stats"); vals[c] = sched_list; c++;
        return sw_val_map_new(keys, vals, c);
    }

    /* supervise(strategy_atom, children_list) -> supervisor pid | nil
     * children: [{name, fun() {...}, restart_atom}, ...]
     * Reuses the native supervisor (restart strategy + circuit breaker +
     * registration) with an interpreter-native child entry (above). Matches the
     * compiled _builtin_supervise's surface: strategy one_for_one|one_for_all|
     * rest_for_one, restart permanent|temporary|transient (default permanent),
     * max_restarts=3 / max_seconds=5. Children register under their name, so
     * whereis(name) resolves — exactly as supervisor.sw expects. */
    if (strcmp(fname, "supervise") == 0) {
        if (nargs < 2) return sw_val_nil();
        sw_val_t *strat_val = args[0];
        sw_val_t *children = args[1];
        if (!children || children->type != SW_VAL_LIST) return sw_val_nil();

        sw_restart_strategy_t strat = SW_ONE_FOR_ONE;
        if (strat_val && strat_val->type == SW_VAL_ATOM && strat_val->v.str) {
            if (strcmp(strat_val->v.str, "one_for_all") == 0) strat = SW_ONE_FOR_ALL;
            else if (strcmp(strat_val->v.str, "rest_for_one") == 0) strat = SW_REST_FOR_ONE;
        }

        int nchildren = children->v.tuple.count;
        if (nchildren <= 0 || nchildren > SW_MAX_CHILDREN) return sw_val_nil();
        sw_child_spec_t *specs = (sw_child_spec_t *)calloc(nchildren, sizeof(sw_child_spec_t));
        int valid = 0;
        for (int i = 0; i < nchildren; i++) {
            sw_val_t *child = children->v.tuple.items[i];
            if (!child || child->type != SW_VAL_TUPLE || child->v.tuple.count < 3) continue;
            sw_val_t *name_v    = child->v.tuple.items[0];
            sw_val_t *fn_v      = child->v.tuple.items[1];
            sw_val_t *restart_v = child->v.tuple.items[2];
            if (!fn_v || fn_v->type != SW_VAL_FUN || !fn_v->v.fun.body) continue;

            if (name_v && (name_v->type == SW_VAL_ATOM || name_v->type == SW_VAL_STRING) && name_v->v.str)
                strncpy(specs[valid].name, name_v->v.str, SW_REG_NAME_MAX - 1);
            else
                snprintf(specs[valid].name, SW_REG_NAME_MAX - 1, "child_%d", i);

            sup_interp_child_t *c = (sup_interp_child_t *)malloc(sizeof(sup_interp_child_t));
            c->interp      = interp;
            c->fn_node     = (node_t *)fn_v->v.fun.body;
            c->closure_env = fn_v->v.fun.closure_env;
            specs[valid].start_func     = _sup_interp_entry;
            specs[valid].start_arg      = c;                  /* MASTER */
            specs[valid].copy_start_arg = _sup_interp_copy;   /* per-child copy */
            specs[valid].free_start_arg = _sup_interp_free;
            specs[valid].restart        = SW_PERMANENT;
            if (restart_v && restart_v->type == SW_VAL_ATOM && restart_v->v.str) {
                if (strcmp(restart_v->v.str, "temporary") == 0) specs[valid].restart = SW_TEMPORARY;
                else if (strcmp(restart_v->v.str, "transient") == 0) specs[valid].restart = SW_TRANSIENT;
            }
            valid++;
        }
        if (valid == 0) { free(specs); return sw_val_nil(); }

        sw_sup_spec_t sup_spec;
        memset(&sup_spec, 0, sizeof(sup_spec));
        sup_spec.strategy     = strat;
        sup_spec.max_restarts = 3;
        sup_spec.max_seconds  = 5;
        sup_spec.children     = specs;
        sup_spec.num_children = valid;
        sup_spec.owns_children = 1;   /* heap specs[]: supervisor frees post-copy */

        sw_process_t *sup = sw_supervisor_start("sw_sup", &sup_spec);
        if (!sup) free(specs);
        return sup ? sw_val_pid(sup) : sw_val_nil();
    }

    /* === Process-scheduler primitives — warn + degrade =========
     * dyn_supervisor/sup_* stay stubbed: the dynamic supervisor's start_child
     * path also dispatches the child closure via sw_val_apply (cfunc-ONLY),
     * but interpreter lambdas carry an AST .body + .closure_env with no cfunc,
     * so a supervised worker would never run. Wiring needs an interpreter-aware
     * child trampoline + closure_env rebind — out of scope for call-the-native. */
    static const char *scheduler_names[] = {
        "send",
        "dyn_supervisor", "sup_start_child", "sup_terminate_child", "sup_count_children",
        "tool_define", "tool_call", "tool_list", "tool_rollback", "tool_history",
        "http_listen", "http_respond", "ws_send", "ws_close",
        "ws_set_handler", "ws_send_binary", "ws_request_headers", "ws_request_path",
        "telemetry_emit", "telemetry_subscribe",
        "pubsub_broadcast", "pubsub_subscribe", "chrome_launch",
        "wsc_connect", "wsc_connect_tls", "wsc_send", "wsc_recv",
        "wsc_set_handler", "wsc_close",
        NULL
    };
    for (int i = 0; scheduler_names[i]; i++) {
        if (strcmp(fname, scheduler_names[i]) == 0) {
            _repl_warn_scheduler(fname);
            return sw_val_nil();
        }
    }

    return NULL;  /* not handled — fall through to user-fn lookup */
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
        const char *op = n->v.binop.op;

        /* Short-circuiting logical operators: evaluate the LEFT operand
         * first, and only touch the RIGHT when the result is not already
         * determined.  Matches the compiled path, where `&&`/`||` lower to
         * C `&&`/`||` and never eagerly evaluate both sides — so
         * `false && panic(...)` and `true || panic(...)` do NOT fire the
         * panic. Evaluating both up front (the old behaviour) broke that. */
        if (strcmp(op, "&&") == 0) {
            sw_val_t *l = eval(interp, n->v.binop.left, env);
            if (interp->error) return sw_val_nil();
            if (!sw_val_is_truthy(l)) return sw_val_atom("false");
            sw_val_t *r = eval(interp, n->v.binop.right, env);
            if (interp->error) return sw_val_nil();
            return sw_val_atom(sw_val_is_truthy(r) ? "true" : "false");
        }
        if (strcmp(op, "||") == 0) {
            sw_val_t *l = eval(interp, n->v.binop.left, env);
            if (interp->error) return sw_val_nil();
            if (sw_val_is_truthy(l)) return sw_val_atom("true");
            sw_val_t *r = eval(interp, n->v.binop.right, env);
            if (interp->error) return sw_val_nil();
            return sw_val_atom(sw_val_is_truthy(r) ? "true" : "false");
        }

        sw_val_t *left = eval(interp, n->v.binop.left, env);
        sw_val_t *right = eval(interp, n->v.binop.right, env);

        /* `++` is polymorphic, mirroring _op_concat in the compiled
         * runtime: list ++ list → concatenated list (Erlang-style), and
         * otherwise string-concat with auto-coercion of int/float/atom/
         * bool/nil to their printed form. The old version only handled
         * string+int and silently dropped everything else (turning
         * list++list into "") — a silent-wrong divergence. */
        if (strcmp(op, "++") == 0) {
            /* List ++ list — preserve list semantics. */
            if (left && right &&
                left->type == SW_VAL_LIST && right->type == SW_VAL_LIST) {
                int la = left->v.tuple.count, lb = right->v.tuple.count;
                if (la == 0) return right;
                if (lb == 0) return left;
                sw_val_t **items = malloc(sizeof(sw_val_t *) * (la + lb));
                for (int i = 0; i < la; i++) items[i] = left->v.tuple.items[i];
                for (int i = 0; i < lb; i++) items[la + i] = right->v.tuple.items[i];
                sw_val_t *r = sw_val_list(items, la + lb);
                free(items);
                return r;
            }
            /* String concat with scalar auto-coercion. Atoms render as
             * their bare text (no leading ':'), bool/nil as true/false/nil,
             * floats via %g — identical to _op_concat. */
            char buf[4096];
            size_t pos = 0;
            buf[0] = '\0';
            sw_val_t *operands[2] = { left, right };
            for (int k = 0; k < 2; k++) {
                sw_val_t *o = operands[k];
                size_t avail = sizeof(buf) - pos - 1;
                if (!o) continue;
                if (o->type == SW_VAL_STRING && o->v.str) {
                    size_t n = strlen(o->v.str);
                    if (n > avail) n = avail;
                    memcpy(buf + pos, o->v.str, n); pos += n;
                } else if (o->type == SW_VAL_ATOM && o->v.str) {
                    size_t n = strlen(o->v.str);
                    if (n > avail) n = avail;
                    memcpy(buf + pos, o->v.str, n); pos += n;
                } else if (o->type == SW_VAL_INT) {
                    pos += (size_t)snprintf(buf + pos, avail + 1, "%lld", (long long)o->v.i);
                } else if (o->type == SW_VAL_FLOAT) {
                    pos += (size_t)snprintf(buf + pos, avail + 1, "%g", o->v.f);
                } else if (o->type == SW_VAL_NIL) {
                    pos += (size_t)snprintf(buf + pos, avail + 1, "nil");
                }
                if (pos > sizeof(buf) - 1) pos = sizeof(buf) - 1;
            }
            buf[pos] = '\0';
            return sw_val_string(buf);
        }

        /* Gleam-style loud error for the Python/JS reflex `"a" + "b"`.
         * Previously this fell all the way through to `return sw_val_nil()`
         * — a silent-wrong result. Now it raises a catchable interp error
         * pointing at the ++ fix, matching the compiled path's panic. */
        if (strcmp(op, "+") == 0 &&
            (left->type == SW_VAL_STRING || right->type == SW_VAL_STRING)) {
            snprintf(interp->error_msg, sizeof(interp->error_msg),
                     "line %d: cannot use + on text \xe2\x80\x94 use ++ to concatenate "
                     "(+ is numeric addition; ++ joins strings and lists)", n->line);
            interp->error = 1;
            return sw_val_nil();
        }

        /* Arithmetic on ints */
        if (left->type == SW_VAL_INT && right->type == SW_VAL_INT) {
            int64_t a = left->v.i, b = right->v.i;
            if (strcmp(op, "+") == 0)  return sw_val_int(a + b);
            if (strcmp(op, "-") == 0)  return sw_val_int(a - b);
            if (strcmp(op, "*") == 0)  return sw_val_int(a * b);
            if (strcmp(op, "/") == 0) {
                /* Strict: panics on zero divisor, matching _op_div (was
                 * silently returning nil → spurious nil cascades). */
                if (b == 0) { interp_raise_panic(interp, n->line, "division by zero"); return sw_val_nil(); }
                return sw_val_int(a / b);
            }
            if (strcmp(op, "%") == 0) {
                if (b == 0) { interp_raise_panic(interp, n->line, "modulo by zero"); return sw_val_nil(); }
                return sw_val_int(a % b);
            }
            if (strcmp(op, "==") == 0) return sw_val_atom(a == b ? "true" : "false");
            if (strcmp(op, "!=") == 0) return sw_val_atom(a != b ? "true" : "false");
            if (strcmp(op, "<") == 0)  return sw_val_atom(a < b ? "true" : "false");
            if (strcmp(op, ">") == 0)  return sw_val_atom(a > b ? "true" : "false");
            if (strcmp(op, "<=") == 0) return sw_val_atom(a <= b ? "true" : "false");
            if (strcmp(op, ">=") == 0) return sw_val_atom(a >= b ? "true" : "false");
        }

        /* String lexicographic comparison — parity with compiled _op_cmp.
         * (Full BEAM total-order across mixed/atom/list types is deferred;
         * this closes the common string<string case that returned nil.) */
        if (left->type == SW_VAL_STRING && right->type == SW_VAL_STRING) {
            int c = strcmp(left->v.str, right->v.str);
            if (strcmp(op, "<") == 0)  return sw_val_atom(c <  0 ? "true" : "false");
            if (strcmp(op, ">") == 0)  return sw_val_atom(c >  0 ? "true" : "false");
            if (strcmp(op, "<=") == 0) return sw_val_atom(c <= 0 ? "true" : "false");
            if (strcmp(op, ">=") == 0) return sw_val_atom(c >= 0 ? "true" : "false");
        }

        /* Arithmetic on floats */
        if ((left->type == SW_VAL_FLOAT || left->type == SW_VAL_INT) &&
            (right->type == SW_VAL_FLOAT || right->type == SW_VAL_INT)) {
            double a = left->type == SW_VAL_INT ? (double)left->v.i : left->v.f;
            double b = right->type == SW_VAL_INT ? (double)right->v.i : right->v.f;
            if (strcmp(op, "+") == 0) return sw_val_float(a + b);
            if (strcmp(op, "-") == 0) return sw_val_float(a - b);
            if (strcmp(op, "*") == 0) return sw_val_float(a * b);
            if (strcmp(op, "/") == 0) {
                /* Strict: matches _op_div, which panics on a zero divisor
                 * (was silently returning nil). */
                if (b == 0.0) { interp_raise_panic(interp, n->line, "division by zero"); return sw_val_nil(); }
                return sw_val_float(a / b);
            }
            if (strcmp(op, "%") == 0) {
                if (b == 0.0) { interp_raise_panic(interp, n->line, "modulo by zero"); return sw_val_nil(); }
                return sw_val_float(fmod(a, b));
            }
            /* Ordering comparisons on float/mixed-numeric operands. The
             * old block only handled + - * / and let < > <= >= fall through
             * to `return sw_val_nil()` — a silent-wrong divergence from
             * _op_cmp, which coerces both to double and compares. */
            if (strcmp(op, "<") == 0)  return sw_val_atom(a < b  ? "true" : "false");
            if (strcmp(op, ">") == 0)  return sw_val_atom(a > b  ? "true" : "false");
            if (strcmp(op, "<=") == 0) return sw_val_atom(a <= b ? "true" : "false");
            if (strcmp(op, ">=") == 0) return sw_val_atom(a >= b ? "true" : "false");
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

    case N_CASE: {
        /* case <subject> { pat -> body ; pat when g -> body ; ... } —
         * try each clause in order. A pattern match binds variables
         * into a child env; if the optional guard then evaluates
         * truthy, run the body and return. Guard failure falls through
         * to the next clause (matching the codegen's behaviour). */
        sw_val_t *subj = eval(interp, n->v.casex.subject, env);
        for (int i = 0; i < n->v.casex.nclauses; i++) {
            node_t *cl = n->v.casex.clauses[i];
            sw_env_t *child = env_new(env);
            if (pattern_match(cl->v.clause.pattern, subj, child)) {
                if (cl->v.clause.guard) {
                    sw_val_t *g = eval(interp, cl->v.clause.guard, child);
                    if (!sw_val_is_truthy(g)) {
                        env_free(child);
                        continue;
                    }
                }
                sw_val_t *r = eval(interp, cl->v.clause.body, child);
                env_free(child);
                return r;
            }
            env_free(child);
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
        /* val |> func  →  func(val, ...extra) with the SAME dispatch chain
         * as N_CALL: module functions, the primary builtins, the
         * extra-builtin bridge (map/filter/reduce, strings, files, json,
         * Std-support, ...), then lambda-valued variables. The previous
         * whitelist (print/length/asserts only) silently returned the piped
         * value unchanged for everything else — `xs |> filter(f)` was a
         * NO-OP under `swc run` while working compiled (dual-path gate
         * tests/sw/conform/t06 holds this closed). */
        sw_val_t *val = eval(interp, n->v.pipe.val, env);
        if (interp->error) return sw_val_nil();
        if (n->v.pipe.func->type == N_CALL) {
            node_t *call = n->v.pipe.func;
            const char *fname = call->v.call.func->v.sval;

            sw_val_t *args[17];
            args[0] = val;
            int nargs = 1;
            for (int i = 0; i < call->v.call.nargs && nargs < 16; i++) {
                args[nargs++] = eval(interp, call->v.call.args[i], env);
                if (interp->error) return sw_val_nil();
            }

            /* Module function (covers Module.qualified names too). */
            node_t *fn = find_fun(interp->module_ast, fname);
            if (fn) {
                if (interp_recursion_blocked(interp)) return sw_val_nil();
                sw_env_t *fenv = env_new(interp->global_env);
                for (int i = 0; i < fn->v.fun.nparams && i < nargs; i++)
                    env_set(fenv, fn->v.fun.params[i], args[i]);
                interp->call_depth++;
                sw_val_t *r = eval(interp, fn->v.fun.body, fenv);
                interp->call_depth--;
                env_free(fenv);
                return r;
            }
            /* Primary builtins (N_CALL's head chain). */
            if (strcmp(fname, "print") == 0) return builtin_print(args, nargs);
            if (strcmp(fname, "length") == 0) return builtin_length(args, nargs);
            if (strcmp(fname, "assert") == 0) return builtin_assert(interp, args, nargs, n->line);
            if (strcmp(fname, "assert_eq") == 0) return builtin_assert_eq(interp, args, nargs, n->line);
            if (strcmp(fname, "assert_ne") == 0) return builtin_assert_ne(interp, args, nargs, n->line);
            if (strcmp(fname, "assert_raises") == 0) return builtin_assert_raises(interp, args, nargs, n->line);
            /* The codegen-surface bridge: map/filter/reduce + the rest. */
            {
                sw_val_t *xr = interp_extra_builtin(interp, fname, args, nargs, n->line);
                if (xr) return xr;
            }
            /* Lambda held in a variable: x |> f(...) */
            sw_val_t *fv = env_get(env, fname);
            if (fv && fv->type == SW_VAL_FUN && fv->v.fun.body)
                return interp_apply_fn(interp, fv, args, nargs);
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
        /* An error/panic raised while evaluating an argument must abort
         * this call, not feed nil into it. Without this, a panic inside
         * an f-string interpolation was laundered into the string "nil"
         * by the desugared to_string(...) and the program sailed on. */
        if (interp->error) return sw_val_nil();

        /* Built-ins */
        if (strcmp(fname, "print") == 0) return builtin_print(args, nargs);
        if (strcmp(fname, "length") == 0) return builtin_length(args, nargs);
        if (strcmp(fname, "hd") == 0) return builtin_hd(interp, args, nargs);
        if (strcmp(fname, "tl") == 0) return builtin_tl(interp, args, nargs);
        if (strcmp(fname, "elem") == 0) return builtin_elem(interp, args, nargs);
        if (strcmp(fname, "abs") == 0 && nargs >= 1) {
            /* abs handles both int and float (was int-only, so abs(-2.5)
             * fell through to "undefined function"). */
            if (args[0]->type == SW_VAL_INT)
                return sw_val_int(args[0]->v.i < 0 ? -args[0]->v.i : args[0]->v.i);
            if (args[0]->type == SW_VAL_FLOAT)
                return sw_val_float(args[0]->v.f < 0 ? -args[0]->v.f : args[0]->v.f);
        }
        if (strcmp(fname, "to_string") == 0 && nargs >= 1) {
            switch (args[0]->type) {
            case SW_VAL_STRING: return args[0];
            case SW_VAL_ATOM:   return sw_val_string(args[0]->v.str);
            case SW_VAL_NIL:    return sw_val_string("nil");
            case SW_VAL_INT: { char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)args[0]->v.i); return sw_val_string(buf); }
            case SW_VAL_FLOAT: { char buf[32]; snprintf(buf, sizeof(buf), "%g", args[0]->v.f); return sw_val_string(buf); }
            default: {
                /* Composite — render via the shared formatter so REPL
                 * output matches print()/codegen output. */
                char *buf = NULL;
                size_t blen = 0;
                FILE *m = open_memstream(&buf, &blen);
                if (!m) return sw_val_string("?");
                sw_val_format(m, args[0]);
                fclose(m);
                sw_val_t *r = sw_val_string(buf ? buf : "");
                free(buf);
                return r;
            }
            }
        }
        if (strcmp(fname, "format") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
            const char *tpl = args[0]->v.str;
            char *buf = NULL;
            size_t blen = 0;
            FILE *m = open_memstream(&buf, &blen);
            if (!m) return sw_val_string("");
            int arg_idx = 1;
            for (const char *p = tpl; *p; ) {
                if (p[0] == '{' && p[1] == '}') {
                    if (arg_idx < nargs) sw_val_format(m, args[arg_idx++]);
                    else fputs("{}", m);
                    p += 2;
                } else if (p[0] == '{' && p[1] == '{') { fputc('{', m); p += 2; }
                else if (p[0] == '}' && p[1] == '}') { fputc('}', m); p += 2; }
                else { fputc(*p, m); p++; }
            }
            fclose(m);
            sw_val_t *r = sw_val_string(buf ? buf : "");
            free(buf);
            return r;
        }
        /* Common string ops — kept minimal so the REPL is useful for
         * exploring sw without dragging in the full studio builtin
         * surface (HTTP, WS, browser, etc. live in the codegen path). */
        if (strcmp(fname, "string_split") == 0 && nargs >= 2 &&
            args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_STRING) {
            const char *s = args[0]->v.str;
            const char *sep = args[1]->v.str;
            int seplen = (int)strlen(sep);
            int cap = 16, count = 0;
            sw_val_t **items = malloc(sizeof(sw_val_t*) * cap);
            const char *cur = s;
            if (seplen == 0) {
                /* Empty separator: whole string as one element (compiled parity). */
                items[count++] = sw_val_string(s);
            } else {
                while (1) {
                    const char *hit = strstr(cur, sep);
                    if (!hit) break;
                    if (count >= cap) { cap *= 2; items = realloc(items, sizeof(sw_val_t*) * cap); }
                    int len = (int)(hit - cur);
                    char *piece = malloc(len + 1);
                    memcpy(piece, cur, len); piece[len] = '\0';
                    items[count++] = sw_val_string(piece);
                    free(piece);
                    cur = hit + seplen;
                }
                if (count >= cap) { cap *= 2; items = realloc(items, sizeof(sw_val_t*) * cap); }
                items[count++] = sw_val_string(cur);
            }
            sw_val_t *r = sw_val_list(items, count);
            free(items);
            return r;
        }
        if (strcmp(fname, "string_contains") == 0 && nargs >= 2 &&
            args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_STRING)
            return sw_val_atom(strstr(args[0]->v.str, args[1]->v.str) ? "true" : "false");
        if (strcmp(fname, "string_starts_with") == 0 && nargs >= 2 &&
            args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_STRING)
            return sw_val_atom(strncmp(args[0]->v.str, args[1]->v.str, strlen(args[1]->v.str)) == 0 ? "true" : "false");
        if (strcmp(fname, "string_ends_with") == 0 && nargs >= 2 &&
            args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_STRING) {
            size_t la = strlen(args[0]->v.str), lb = strlen(args[1]->v.str);
            return sw_val_atom(la >= lb && strcmp(args[0]->v.str + la - lb, args[1]->v.str) == 0 ? "true" : "false");
        }
        if (strcmp(fname, "string_index_of") == 0 && nargs >= 2 &&
            args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_STRING) {
            const char *hit = strstr(args[0]->v.str, args[1]->v.str);
            return sw_val_int(hit ? (int64_t)(hit - args[0]->v.str) : -1);
        }
        if (strcmp(fname, "string_upper") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
            char *r = strdup(args[0]->v.str);
            for (char *p = r; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
            sw_val_t *v = sw_val_string(r); free(r); return v;
        }
        if (strcmp(fname, "string_lower") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
            char *r = strdup(args[0]->v.str);
            for (char *p = r; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            sw_val_t *v = sw_val_string(r); free(r); return v;
        }
        if (strcmp(fname, "string_trim") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
            const char *s = args[0]->v.str;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            const char *end = s + strlen(s);
            while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
            int len = (int)(end - s);
            char *r = malloc(len + 1); memcpy(r, s, len); r[len] = '\0';
            sw_val_t *v = sw_val_string(r); free(r); return v;
        }
        if (strcmp(fname, "string_length") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING)
            return sw_val_int((int64_t)strlen(args[0]->v.str));
        if (strcmp(fname, "json_encode") == 0) {
            /* Real JSON, matching the compiled runtime's _json_encode_val
             * exactly (see interp_json_encode_val). The old stub emitted
             * the value debug-repr (e.g. %{:name: alice}) — invalid JSON. */
            if (nargs < 1) return sw_val_string("null");
            char *buf = (char *)malloc(256);
            size_t cap = 256, pos = 0;
            interp_json_encode_val(args[0], &buf, &cap, &pos);
            buf[pos] = '\0';
            sw_val_t *r = sw_val_string(buf);
            free(buf);
            return r;
        }
        if (strcmp(fname, "json_decode") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
            return sw_lang_json_decode(args[0]->v.str);
        }
        /* Map ops the REPL is likely to want for shape checks. */
        if (strcmp(fname, "map_size") == 0 && nargs >= 1 && args[0]->type == SW_VAL_MAP)
            return sw_val_int(args[0]->v.map.count);
        if (strcmp(fname, "map_has_key") == 0 && nargs >= 2 && args[0]->type == SW_VAL_MAP) {
            /* Delegate to sw_val_map_get so the atom-vs-string fallback matches:
             * the bare sw_val_equal loop diverged from map_get (and from the
             * compiled _builtin_map_has_key) on json_decode'd maps, where a
             * string-keyed query missed an atom key that map_get still found.
             * Now byte-for-byte identical to the compiled backend. */
            sw_val_t *v = sw_val_map_get(args[0], args[1]);
            return sw_val_atom((v && v->type != SW_VAL_NIL) ? "true" : "false");
        }
        if (strcmp(fname, "timestamp") == 0) {
            struct timeval tv; gettimeofday(&tv, NULL);
            return sw_val_int((int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000);
        }
        if (strcmp(fname, "map_get") == 0 && nargs >= 2) {
            sw_val_t *mv = sw_val_map_get(args[0], args[1]);
            /* 3-arg overload: map_get(m, k, default). */
            if (nargs >= 3 && (!mv || mv->type == SW_VAL_NIL)) return args[2];
            return mv;
        }
        if (strcmp(fname, "map_put") == 0 && nargs >= 3) return sw_val_map_put(args[0], args[1], args[2]);
        if (strcmp(fname, "map_new") == 0) return sw_val_map_new(NULL, NULL, 0);
        if (strcmp(fname, "map_keys") == 0 && nargs >= 1 && args[0]->type == SW_VAL_MAP) {
            return sw_val_list(args[0]->v.map.keys, args[0]->v.map.count);
        }
        if (strcmp(fname, "map_values") == 0 && nargs >= 1 && args[0]->type == SW_VAL_MAP) {
            return sw_val_list(args[0]->v.map.vals, args[0]->v.map.count);
        }
        if (strcmp(fname, "typeof") == 0 && nargs >= 1) {
            /* Switch (not a positional array) so it can't drift if the enum
             * is reordered, and matches the compiled _builtin_typeof exactly. */
            switch (args[0]->type) {
            case SW_VAL_NIL:        return sw_val_string("nil");
            case SW_VAL_INT:        return sw_val_string("int");
            case SW_VAL_FLOAT:      return sw_val_string("float");
            case SW_VAL_STRING:     return sw_val_string("string");
            case SW_VAL_ATOM:       return sw_val_string("atom");
            case SW_VAL_PID:        return sw_val_string("pid");
            case SW_VAL_REMOTE_PID: return sw_val_string("rpid");
            case SW_VAL_TUPLE:      return sw_val_string("tuple");
            case SW_VAL_LIST:       return sw_val_string("list");
            case SW_VAL_FUN:        return sw_val_string("fun");
            case SW_VAL_MAP:        return sw_val_string("map");
            case SW_VAL_BYTES:      return sw_val_string("bytes");
            default:                return sw_val_string("unknown");
            }
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

        /* Test assertion builtins — but let user-defined functions with the
         * same name take precedence. This allows test files to define their
         * own 3-arg assert_eq(name, actual, expected) helpers without the
         * 2-arg builtin intercepting the call. */
        if ((strcmp(fname, "assert") == 0 ||
             strcmp(fname, "assert_eq") == 0 ||
             strcmp(fname, "assert_ne") == 0 ||
             strcmp(fname, "assert_raises") == 0) &&
            !find_fun(interp->module_ast, fname)) {
            if (strcmp(fname, "assert") == 0) return builtin_assert(interp, args, nargs, n->line);
            if (strcmp(fname, "assert_eq") == 0) return builtin_assert_eq(interp, args, nargs, n->line);
            if (strcmp(fname, "assert_ne") == 0) return builtin_assert_ne(interp, args, nargs, n->line);
            if (strcmp(fname, "assert_raises") == 0) return builtin_assert_raises(interp, args, nargs, n->line);
        }

        /* Extra REPL builtins — bridges the gap to the codegen surface
         * (file_*, db_*, shell, panic, expect, error, etc.). See
         * interp_extra_builtin() above. */
        {
            sw_val_t *xr = interp_extra_builtin(interp, fname, args, nargs, n->line);
            if (xr) return xr;
        }

        /* User-defined function in module */
        node_t *fn = find_fun(interp->module_ast, fname);
        if (fn) {
            if (interp_recursion_blocked(interp)) return sw_val_nil();
            sw_env_t *fenv = env_new(interp->global_env);
            for (int i = 0; i < fn->v.fun.nparams; i++) {
                if (i < nargs)
                    env_set(fenv, fn->v.fun.params[i], args[i]);
                else if (fn->v.fun.defaults[i])
                    env_set(fenv, fn->v.fun.params[i], eval(interp, fn->v.fun.defaults[i], fenv));
                else
                    env_set(fenv, fn->v.fun.params[i], sw_val_nil());
            }
            interp->call_depth++;
            sw_val_t *r = eval(interp, fn->v.fun.body, fenv);
            interp->call_depth--;
            env_free(fenv);
            return r;
        }

        /* Dynamic dispatch: variable holds a closure */
        sw_val_t *fn_val = env_get(env, fname);
        if (fn_val && fn_val->type == SW_VAL_FUN && fn_val->v.fun.body) {
            if (interp_recursion_blocked(interp)) return sw_val_nil();
            node_t *fn_node = (node_t *)fn_val->v.fun.body;
            sw_env_t *fenv = env_new(fn_val->v.fun.closure_env ? fn_val->v.fun.closure_env : interp->global_env);
            for (int i = 0; i < fn_node->v.fun.nparams && i < nargs; i++)
                env_set(fenv, fn_node->v.fun.params[i], args[i]);
            interp->call_depth++;
            sw_val_t *r = eval(interp, fn_node->v.fun.body, fenv);
            interp->call_depth--;
            env_free(fenv);
            return r;
        }

        snprintf(interp->error_msg, sizeof(interp->error_msg),
                 "undefined function: %s/%d", fname, nargs);
        interp->error = 1;
        return sw_val_nil();
    }

    case N_SPAWN: {
        /* Spawn a REAL child fiber that re-enters the interpreter, instead of
         * running the body inline in the parent (which made monitor/link/
         * ping-pong fail and spawn-based examples HANG: the parent received a
         * message its inlined "child" never actually sent from a child
         * context). Safe because swc run forces nsched=1 (single OS thread →
         * cooperative fibers → no concurrent interp/env mutation).
         *
         * Two AST shapes (see parser par_expr):
         *  (a) N_CALL — spawn(worker(a,b)): resolve the fn by name, evaluate
         *      ONLY the args in the PARENT env now (matching the compiled
         *      deep-copy-of-args-at-spawn semantics), and run the body on a
         *      new fiber. We must NOT eval the N_CALL (that would run the body
         *      inline).
         *  (b) non-N_CALL — spawn(f) / spawn(fn(){...}): eval yields a
         *      SW_VAL_FUN value; run its body with 0 args on a new fiber. */
        node_t *se = n->v.spawn.expr;
        spawn_boot_t *b = calloc(1, sizeof(*b));
        b->interp = interp;

        if (se && se->type == N_CALL && se->v.call.func &&
            se->v.call.func->type == N_IDENT) {
            const char *fname = se->v.call.func->v.sval;
            node_t *fn = find_fun((node_t *)interp->module_ast, fname);
            if (!fn) {
                /* Fall back: a closure-valued local var? */
                sw_val_t *fv = env_get(env, fname);
                if (fv && fv->type == SW_VAL_FUN && fv->v.fun.body) {
                    fn = (node_t *)fv->v.fun.body;
                    b->closure_env = fv->v.fun.closure_env;
                }
            }
            if (!fn) {
                free(b);
                snprintf(interp->error_msg, sizeof(interp->error_msg),
                         "spawn: undefined function: %s", fname);
                interp->error = 1;
                return sw_val_nil();
            }
            b->fn_node = fn;
            int na = se->v.call.nargs;
            if (na > 16) na = 16;
            b->nargs = na;
            if (na) {
                b->args = malloc(sizeof(sw_val_t *) * na);
                for (int i = 0; i < na; i++)
                    b->args[i] = eval(interp, se->v.call.args[i], env);
                if (interp->error) { free(b->args); free(b); return sw_val_nil(); }
            }
        } else {
            /* fn-value / inline-lambda form — eval to a SW_VAL_FUN, 0 args. */
            sw_val_t *inner = eval(interp, se, env);
            if (interp->error) { free(b); return sw_val_nil(); }
            if (!inner || inner->type != SW_VAL_FUN || !inner->v.fun.body) {
                free(b);
                snprintf(interp->error_msg, sizeof(interp->error_msg),
                         "spawn: expression is not callable");
                interp->error = 1;
                return sw_val_nil();
            }
            b->fn_node = (node_t *)inner->v.fun.body;
            b->closure_env = inner->v.fun.closure_env;
            b->nargs = 0;
        }

        sw_process_t *child = sw_spawn(spawn_trampoline, b);
        if (!child) {
            if (b->args) free(b->args);
            free(b);
            snprintf(interp->error_msg, sizeof(interp->error_msg),
                     "spawn failed: process table full");
            interp->error = 1;
            return sw_val_nil();
        }
        sw_val_t *pid = sw_val_pid(child);   /* REAL pid */
        if (n->v.spawn.monitor) {
            /* spawn_monitor -> {pid, ref}: install a real monitor so DOWN
             * signals route back, matching the compiled spawn_monitor. */
            uint64_t ref = sw_monitor(child);
            sw_val_t *items[2] = { pid, sw_val_int((int64_t)ref) };
            return sw_val_tuple(items, 2);
        }
        return pid;
    }

    case N_SEND: {
        sw_val_t *to = eval(interp, n->v.send.to, env);
        sw_val_t *msg = eval(interp, n->v.send.msg, env);
        /* Structure-preserving send: identical to the compiled path
         * (emit_send → sw_send_dispatch → sw_send_value, SW_TAG_NONE).
         * Deep-copies the full sw_val_t graph into the receiver's mailbox
         * (SW_PK_VALUE region), and handles SW_VAL_REMOTE_PID for free.
         * Tag is SW_TAG_NONE (was SW_TAG_CAST) to match compiled; receive
         * pattern-matches the payload and ignores the tag, so this is safe. */
        if (to && (to->type == SW_VAL_PID || to->type == SW_VAL_REMOTE_PID))
            sw_send_dispatch(to, msg);
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

    case N_LIST_COMP: {
        /* [body for var in iter] or [body for var in iter when guard]
         * Evaluates iter, iterates over it, optionally filters by guard,
         * and collects body results into a new list. */
        sw_val_t *iter = eval(interp, n->v.lcomp.iter, env);
        int src_count = (iter->type == SW_VAL_LIST) ? iter->v.tuple.count : 0;
        sw_val_t **items = malloc(sizeof(sw_val_t*) * (src_count > 0 ? src_count : 1));
        int out_count = 0;
        if (iter->type == SW_VAL_LIST) {
            for (int i = 0; i < src_count && !interp->error; i++) {
                sw_env_t *iter_env = env_new(env);
                env_set(iter_env, n->v.lcomp.var, iter->v.tuple.items[i]);
                if (n->v.lcomp.guard) {
                    sw_val_t *gv = eval(interp, n->v.lcomp.guard, iter_env);
                    if (!sw_val_is_truthy(gv)) {
                        env_free(iter_env);
                        continue;
                    }
                }
                items[out_count++] = eval(interp, n->v.lcomp.body, iter_env);
                env_free(iter_env);
            }
        }
        sw_val_t *result = sw_val_list(items, out_count);
        free(items);
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
        /* try_depth gates error()'s outside-a-try silent path; see the
         * error builtin. Decrement BEFORE running the catch body so an
         * error() raised inside catch propagates to the ENCLOSING try
         * (compiled-path parity), not back into this one. */
        interp->try_depth++;
        sw_val_t *result = eval(interp, n->v.trycatch.body, env);
        interp->try_depth--;
        /* A panic is uncatchable: leave both flags set so every enclosing
         * eval keeps short-circuiting and the top level exits 1. Only
         * error() lands in catch — same split as the compiled path. */
        if (interp->error && !interp->panicking) {
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
        sw_val_t *v = val_alloc(sizeof(sw_val_t));
        v->type = SW_VAL_FUN;
        v->v.fun.name = strdup(n->v.fun.name);
        v->v.fun.num_params = n->v.fun.nparams;
        v->v.fun.body = n;
        v->v.fun.closure_env = env;
        env_retain(env);   /* closure escapes its frame: keep captured env alive */
        return v;
    }

    case N_RECEIVE: {
        /* True selective receive — mirrors the compiled emit_receive. Scan the
         * WHOLE mailbox in arrival order; for each queued message try every
         * clause (pattern + guard); consume ONLY the first message that matches
         * some clause (leaving all others queued in order); run that clause body
         * and return. If nothing matches, block for new messages within the
         * remaining `after` budget; on timeout run after_body (or return nil).
         *
         * Safe under the single-scheduler contract (swc run forces nsched=1):
         * no concurrent sender mutates this mailbox, so the peek/remove/wait
         * dance needs none of the compiled lock-free care. */
        uint64_t budget = n->v.recv.after_ms >= 0 ? (uint64_t)n->v.recv.after_ms
                                                  : (uint64_t)-1;  /* forever (compiled parity) */
        uint64_t start = recv_now_ms();

        for (;;) {
            /* (1) Scan the currently-queued messages. */
            sw_mailbox_drain_signals();
            sw_msg_t *cur = sw_mailbox_peek();
            while (cur) {
                sw_msg_t *next = cur->next;
                sw_val_t *msg = recv_msg_to_val(cur);
                for (int i = 0; i < n->v.recv.nclauses; i++) {
                    node_t *cl = n->v.recv.clauses[i];
                    sw_env_t *cl_env = env_new(env);
                    if (pattern_match(cl->v.clause.pattern, msg, cl_env)) {
                        if (cl->v.clause.guard) {
                            sw_val_t *gv = eval(interp, cl->v.clause.guard, cl_env);
                            if (!sw_val_is_truthy(gv)) { env_free(cl_env); continue; }
                        }
                        /* Matched: consume only this message. Adopt-splice its
                         * value region into the receiver's arena so the bound
                         * payload aliases live in the receiver's lifecycle
                         * (mirrors compiled). RAW/signal payloads and the
                         * GC-off path (no receiver arena) skip the adopt. */
                        sw_mailbox_remove_msg(cur);
                        {
                            sw_value_arena_t *rv = sw_self_varena();
                            if (cur->pkind == SW_PK_VALUE && cur->region && rv) {
                                sw_varena_adopt(rv, cur->region);
                                cur->region = NULL;
                            }
                        }
                        sw_msg_release(cur);
                        sw_val_t *r = eval(interp, cl->v.clause.body, cl_env);
                        env_free(cl_env);
                        return r;
                    }
                    env_free(cl_env);
                }
                cur = next;
            }

            /* (2) No queued message matched — wait for new ones, respecting the
             * remaining budget. */
            uint64_t elapsed = recv_now_ms() - start;
            if (elapsed >= budget) break;          /* timed out */
            uint64_t remaining = (budget == (uint64_t)-1) ? (uint64_t)-1
                                                          : budget - elapsed;
            /* No process identity (tool eval thread, sw_self()==NULL): no sender
             * can ever deliver here, so a blocking receive must DEGRADE to its
             * after-clause/nil instead of spinning forever. Mirrors the no-proc
             * short-circuit in sw_receive_any / sw_mailbox_wait_new. */
            if (!sw_self()) break;
            sw_mailbox_wait_new(remaining);
            /* Loop back and re-scan (a spurious wake just rescans harmlessly). */
        }

        /* Timeout: run the after clause if present, else nil. */
        if (n->v.recv.after_body)
            return eval(interp, n->v.recv.after_body, env);
        return sw_val_nil();
    }

    default:
        return sw_val_nil();
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* Free an AST returned by sw_lang_parse. Public so callers that parse
 * but never interpret (fuzz harness, linters, one-shot tooling) can
 * reclaim it — node_free is otherwise file-static. */
void sw_lang_free_ast(void *ast) {
    if (ast) node_free((node_t *)ast);
}

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
    /* Populate module-level globals into global_env so every function
     * env (child of global_env) sees them via the parent chain. */
    if (module_ast) {
        node_t *mod = (node_t *)module_ast;
        for (int i = 0; i < mod->v.mod.nglobals; i++) {
            node_t *vn = mod->v.mod.globals[i].val;
            /* eval() handles N_INT/N_FLOAT/N_STRING/N_ATOM/N_MAP/N_LIST/N_TUPLE.
             * Pure literal maps and lists need no env lookup so they work
             * correctly with an empty global_env. Variable references return nil.
             * Note: module functions are findable via module_ast at this point,
             * so function-call globals would execute at load time — unsupported. */
            sw_val_t *sv = eval(interp, vn, interp->global_env);
            env_set(interp->global_env, mod->v.mod.globals[i].name, sv);
        }
    }
    return interp;
}

/* === Tool-registry admission lint ======================================= *
 * interp_is_known_builtin mirrors the builtins the INTERPRETER actually
 * implements (eval's N_CALL prelude + interp_extra_builtin) — derived from the
 * real dispatch, NOT codegen's is_builtin (which is compiler-only and lists
 * compiled-only builtins like http_get/pmap that a tool's interpreter can't run,
 * so admitting them would re-introduce the silent-nil). tests/sw/
 * test_tool_registry.sw locks the pure set in so a dropped name fails CI. */
static const char *k_interp_builtins[] = {
    /* eval N_CALL prelude */
    "abs","assert","assert_eq","assert_ne","assert_raises","elem","format","hd",
    "json_decode","json_encode","length","list_append","map_get","map_has_key",
    "map_keys","map_new","map_put","map_size","map_values","print",
    "string_contains","string_ends_with","string_index_of","string_length",
    "string_lower","string_split","string_starts_with","string_trim",
    "string_upper","timestamp","tl","to_string","typeof",
    /* interp_extra_builtin */
    "audio_pcm16_to_ulaw","audio_pcm16_to_ulaw_b","audio_resample",
    "audio_resample_b","audio_ulaw_to_pcm16","audio_ulaw_to_pcm16_b",
    "base64_decode","base64_encode","byte","byte_at","byte_size","byte_slice",
    "bytes_concat","bytes_from_base64","bytes_from_ints","bytes_to_base64",
    "bytes_to_string","codepoint_at","db_close","db_exec","db_open","db_query",
    "ed25519_verify","error","ets_cas","ets_count","ets_delete","ets_get",
    "ets_list","ets_new","ets_put","ets_take","ets_update","ets_update_counter",
    "exec_argv","expect","file_append","file_delete","file_exists","file_list",
    "file_mkdir","file_read","file_read_bytes","file_write","file_write_bytes",
    "filter","getenv","json_escape","json_get","map","map_merge","map_remove",
    "math_ceil","math_cos","math_exp","math_floor","math_log","math_pow",
    "math_round","math_sin","math_sqrt","ord","os_args","panic","print_above",
    "random_int","read_key","reduce","shell","shell_managed","shell_sandboxed","sleep","string_replace",
    "string_sub","string_to_bytes","string_chars","string_truncate","sys_exit","to_float",
    "to_int","uuid","now_iso",
    NULL
};
int interp_is_known_builtin(const char *name) {
    if (!name) return 0;
    for (int i = 0; k_interp_builtins[i]; i++)
        if (strcmp(k_interp_builtins[i], name) == 0) return 1;
    return 0;
}

/* Capability a gated builtin requires, or NULL for an always-allowed pure one. */
static const char *lint_required_cap(const char *name) {
    if (strncmp(name, "file_", 5) == 0) return "file";
    if (strncmp(name, "db_", 3) == 0)   return "db";
    if (strcmp(name, "shell") == 0 || strcmp(name, "shell_sandboxed") == 0 ||
        strcmp(name, "shell_managed") == 0 ||
        strcmp(name, "exec_argv") == 0) return "shell";
    return NULL;
}

/* 1 if the caps list/tuple of atoms contains `cap`. */
static int lint_caps_has(sw_val_t *caps, const char *cap) {
    if (!caps || (caps->type != SW_VAL_LIST && caps->type != SW_VAL_TUPLE)) return 0;
    for (int i = 0; i < caps->v.tuple.count; i++) {
        sw_val_t *c = caps->v.tuple.items[i];
        if (c && (c->type == SW_VAL_ATOM || c->type == SW_VAL_STRING) &&
            c->v.str && strcmp(c->v.str, cap) == 0) return 1;
    }
    return 0;
}

/* Bound names (params + local `x = ...` assignments + loop vars) seen so far —
 * a call to one of these is a local variable (e.g. a lambda/callback), not a
 * builtin, so it must NOT be flagged as unknown. Flat module-wide set: a lint
 * is for catching typo'd builtins and ungranted caps, not enforcing scoping,
 * so over-permissiveness on local-var calls is fine. */
typedef struct { const char *names[512]; int count; } lint_bound_t;
static void lint_bind(lint_bound_t *b, const char *nm) {
    if (!nm || !nm[0] || b->count >= 512) return;
    for (int i = 0; i < b->count; i++) if (strcmp(b->names[i], nm) == 0) return;
    b->names[b->count++] = nm;
}
static int lint_is_bound(lint_bound_t *b, const char *nm) {
    if (!nm) return 0;
    for (int i = 0; i < b->count; i++) if (strcmp(b->names[i], nm) == 0) return 1;
    return 0;
}

/* Vet one call target. Returns 1 (and fills err) if it must be rejected. */
static int lint_check_call(const char *name, node_t *mod, lint_bound_t *b,
                           sw_val_t *caps, char *err, unsigned long errlen) {
    if (!name) return 0;
    if (find_fun(mod, name)) return 0;            /* a fn this tool defines: ok */
    if (lint_is_bound(b, name)) return 0;         /* a local var (lambda/callback): ok */
    if (strcmp(name, "sys_exit") == 0) {          /* would kill the host process */
        snprintf(err, errlen, "`sys_exit` is not allowed inside a tool");
        return 1;
    }
    if (!interp_is_known_builtin(name)) {
        snprintf(err, errlen,
                 "unknown function `%s` (typo, or not available inside a tool)", name);
        return 1;
    }
    const char *cap = lint_required_cap(name);
    if (cap && !lint_caps_has(caps, cap)) {
        snprintf(err, errlen,
                 "builtin `%s` needs capability '%s' — pass it: tool_define(name, src, ['%s'])",
                 name, cap, cap);
        return 1;
    }
    return 0;
}

/* Recursive read-only AST walk (mirrors node_free's arms). 1 if rejected.
 * Binds params/locals into `b` as it descends so calls to them aren't flagged. */
static int lint_walk(node_t *n, node_t *mod, lint_bound_t *b, sw_val_t *caps,
                     char *err, unsigned long errlen) {
    if (!n) return 0;
    switch (n->type) {
    case N_CALL:
        if (n->v.call.func && n->v.call.func->type == N_IDENT &&
            lint_check_call(n->v.call.func->v.sval, mod, b, caps, err, errlen)) return 1;
        if (lint_walk(n->v.call.func, mod, b, caps, err, errlen)) return 1;
        for (int i = 0; i < n->v.call.nargs; i++)
            if (lint_walk(n->v.call.args[i], mod, b, caps, err, errlen)) return 1;
        return 0;
    case N_PIPE:
        if (n->v.pipe.func && n->v.pipe.func->type == N_IDENT &&
            lint_check_call(n->v.pipe.func->v.sval, mod, b, caps, err, errlen)) return 1;
        if (lint_walk(n->v.pipe.val, mod, b, caps, err, errlen)) return 1;
        return lint_walk(n->v.pipe.func, mod, b, caps, err, errlen);
    case N_FUN:
        for (int i = 0; i < n->v.fun.nparams; i++) lint_bind(b, n->v.fun.params[i]);
        return lint_walk(n->v.fun.body, mod, b, caps, err, errlen);
    case N_BLOCK:
        for (int i = 0; i < n->v.block.nstmts; i++)
            if (lint_walk(n->v.block.stmts[i], mod, b, caps, err, errlen)) return 1;
        return 0;
    case N_ASSIGN:
        lint_bind(b, n->v.assign.name);           /* `f = fun(){...}` → f is local */
        return lint_walk(n->v.assign.value, mod, b, caps, err, errlen);
    case N_SPAWN: return lint_walk(n->v.spawn.expr, mod, b, caps, err, errlen);
    case N_SEND:
        return lint_walk(n->v.send.to, mod, b, caps, err, errlen) ||
               lint_walk(n->v.send.msg, mod, b, caps, err, errlen);
    case N_RECEIVE:
        for (int i = 0; i < n->v.recv.nclauses; i++)
            if (lint_walk(n->v.recv.clauses[i], mod, b, caps, err, errlen)) return 1;
        return lint_walk(n->v.recv.after_body, mod, b, caps, err, errlen) ||
               lint_walk(n->v.recv.after_expr, mod, b, caps, err, errlen);
    case N_CLAUSE:
        return lint_walk(n->v.clause.guard, mod, b, caps, err, errlen) ||
               lint_walk(n->v.clause.body, mod, b, caps, err, errlen);
    case N_IF:
        return lint_walk(n->v.iff.cond, mod, b, caps, err, errlen) ||
               lint_walk(n->v.iff.then_b, mod, b, caps, err, errlen) ||
               lint_walk(n->v.iff.else_b, mod, b, caps, err, errlen);
    case N_BINOP:
        return lint_walk(n->v.binop.left, mod, b, caps, err, errlen) ||
               lint_walk(n->v.binop.right, mod, b, caps, err, errlen);
    case N_UNARY: return lint_walk(n->v.unary.operand, mod, b, caps, err, errlen);
    case N_TUPLE: case N_LIST:
        for (int i = 0; i < n->v.coll.count; i++)
            if (lint_walk(n->v.coll.items[i], mod, b, caps, err, errlen)) return 1;
        return 0;
    case N_MAP:
        for (int i = 0; i < n->v.map.count; i++)
            if (lint_walk(n->v.map.keys[i], mod, b, caps, err, errlen) ||
                lint_walk(n->v.map.vals[i], mod, b, caps, err, errlen)) return 1;
        return 0;
    case N_FOR:
        lint_bind(b, n->v.forloop.var);
        return lint_walk(n->v.forloop.iter, mod, b, caps, err, errlen) ||
               lint_walk(n->v.forloop.body, mod, b, caps, err, errlen);
    case N_LIST_COMP:
        lint_bind(b, n->v.lcomp.var);
        return lint_walk(n->v.lcomp.iter, mod, b, caps, err, errlen) ||
               lint_walk(n->v.lcomp.body, mod, b, caps, err, errlen) ||
               lint_walk(n->v.lcomp.guard, mod, b, caps, err, errlen);
    case N_RANGE:
        return lint_walk(n->v.range.from, mod, b, caps, err, errlen) ||
               lint_walk(n->v.range.to, mod, b, caps, err, errlen);
    case N_TRY:
        return lint_walk(n->v.trycatch.body, mod, b, caps, err, errlen) ||
               lint_walk(n->v.trycatch.catch_body, mod, b, caps, err, errlen);
    case N_LIST_CONS:
        return lint_walk(n->v.cons.head, mod, b, caps, err, errlen) ||
               lint_walk(n->v.cons.tail, mod, b, caps, err, errlen);
    case N_CASE:
        if (lint_walk(n->v.casex.subject, mod, b, caps, err, errlen)) return 1;
        for (int i = 0; i < n->v.casex.nclauses; i++)
            if (lint_walk(n->v.casex.clauses[i], mod, b, caps, err, errlen)) return 1;
        return 0;
    default: return 0;  /* leaves: N_INT/FLOAT/STRING/ATOM/IDENT/SELF */
    }
}

int sw_lang_lint_tool(void *module_ast, sw_val_t *caps, char *err, unsigned long errlen) {
    node_t *mod = (node_t *)module_ast;
    if (!mod || mod->type != N_MODULE) return 0;
    lint_bound_t b; b.count = 0;
    /* Pre-bind every top-level fn's params (forward refs across fns). */
    for (int i = 0; i < mod->v.mod.nfuns; i++)
        for (int j = 0; j < mod->v.mod.funs[i]->v.fun.nparams; j++)
            lint_bind(&b, mod->v.mod.funs[i]->v.fun.params[j]);
    for (int i = 0; i < mod->v.mod.nfuns; i++)
        if (lint_walk(mod->v.mod.funs[i], mod, &b, caps, err, errlen)) return 1;
    /* module-level `let x = ...` globals run before run(), so lint them too
     * (else `let p = shell(...)` evades caps). */
    for (int i = 0; i < mod->v.mod.nglobals; i++)
        if (lint_walk(mod->v.mod.globals[i].val, mod, &b, caps, err, errlen)) return 1;
    return 0;
}

int sw_lang_has_fun(void *module_ast, const char *name) {
    return module_ast && find_fun((node_t *)module_ast, name) != NULL;
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

sw_val_t *sw_lang_call_fresh(void *module_ast, const char *func_name,
                             sw_val_t **args, int num_args) {
    if (!module_ast) return sw_val_nil();
    /* Per-call interpreter: its own global_env + error/call_depth, off the
     * shared read-only AST. Two threads calling the same tool never touch each
     * other's state, and a fault doesn't outlive this call. (Mirrors the
     * module-globals seeding sw_lang_new does, but never frees the AST.) */
    sw_interp_t local;
    memset(&local, 0, sizeof(local));
    local.module_ast = module_ast;
    local.global_env = env_new(NULL);
    node_t *mod = (node_t *)module_ast;
    for (int i = 0; i < mod->v.mod.nglobals; i++) {
        sw_val_t *sv = eval(&local, mod->v.mod.globals[i].val, local.global_env);
        env_set(local.global_env, mod->v.mod.globals[i].name, sv);
    }
    sw_val_t *result = sw_lang_call(&local, func_name, args, num_args);
    env_free(local.global_env);
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

sw_val_t *sw_lang_eval_repl(sw_interp_t *interp, const char *expr_source) {
    /* Parse expression wrapped in a function */
    char wrapped[4096];
    snprintf(wrapped, sizeof(wrapped),
             "module _Eval\nfun _eval() {\n%s\n}\n", expr_source);

    par_t p;
    par_init(&p, wrapped);
    node_t *mod = par_module(&p);
    if (p.err) {
        node_free(mod);
        snprintf(interp->error_msg, sizeof(interp->error_msg), "parse error");
        interp->error = 1;
        return sw_val_nil();
    }

    /* Find the _eval function body */
    node_t *fn = NULL;
    for (int i = 0; i < mod->v.mod.nfuns; i++) {
        if (strcmp(mod->v.mod.funs[i]->v.fun.name, "_eval") == 0) {
            fn = mod->v.mod.funs[i];
            break;
        }
    }
    if (!fn) { node_free(mod); return sw_val_nil(); }

    /* Evaluate body directly in global_env so assignments persist.
     * Keep original module_ast so loaded module functions are accessible. */
    sw_val_t *result = eval(interp, fn->v.fun.body, interp->global_env);
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
static sw_val_t *_jd_parse_inner(const char **pp);
/* Nesting-depth guard: deeply-nested JSON recurses one C frame per level and
 * SIGILLs on the fiber stack. Cap nesting; over the cap _jd_parse returns nil
 * without consuming (the callers' cnt caps bound the resulting spin). */
static __thread int g_jd_depth = 0;
#define SW_JD_MAX_DEPTH 256

static void _jd_skip_ws(const char **pp) {
    while (**pp == ' ' || **pp == '\t' || **pp == '\n' || **pp == '\r') (*pp)++;
}

/* Parse 4 hex digits at *pp into 16-bit codepoint, advancing *pp.
 * Returns -1 on malformed input. Caller positions past leading 'u'. */
static int _jd_hex4(const char **pp) {
    int cp = 0;
    for (int i = 0; i < 4; i++) {
        char c = **pp;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
        else return -1;
        cp = (cp << 4) | d;
        (*pp)++;
    }
    return cp;
}

/* UTF-8 encode codepoint cp into buf. Returns bytes written (1-4). */
static int _jd_utf8(unsigned int cp, char *buf) {
    if (cp < 0x80) { buf[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static sw_val_t *_jd_parse_string(const char **pp) {
    (*pp)++;
    char buf[8192];
    int len = 0;
    while (**pp && **pp != '"' && len < (int)sizeof(buf) - 5) {
        if (**pp == '\\') {
            (*pp)++;
            /* A backslash as the last byte before the NUL: stop. Otherwise
             * the switch's default/simple cases copy the NUL and (*pp)++
             * step PAST the terminator → heap-buffer-overflow on the next
             * loop read (found by fuzz-json). The while gate ends the loop. */
            if (**pp == '\0') break;
            switch (**pp) {
                case 'n': buf[len++] = '\n'; (*pp)++; break;
                case 't': buf[len++] = '\t'; (*pp)++; break;
                case 'r': buf[len++] = '\r'; (*pp)++; break;
                case 'b': buf[len++] = '\b'; (*pp)++; break;
                case 'f': buf[len++] = '\f'; (*pp)++; break;
                case '"': case '\\': case '/':
                    buf[len++] = **pp; (*pp)++; break;
                case 'u': {
                    /* \uXXXX — UTF-8 encode the codepoint. Combines
                     * surrogate pairs (\uD800..\uDBFF + \uDC00..\uDFFF)
                     * into 4-byte UTF-8 for emoji / SMP characters.
                     * Without this, model-emitted JSON containing
                     * special chars (& as &, > as >) was
                     * being decoded to literal "u0026" / "u003e",
                     * breaking shell composition in tool calls. */
                    (*pp)++;
                    int cp = _jd_hex4(pp);
                    if (cp < 0) { buf[len++] = 'u'; break; }
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        (*pp)[0] == '\\' && (*pp)[1] == 'u') {
                        const char *save = *pp;
                        (*pp) += 2;
                        int low = _jd_hex4(pp);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            unsigned int full = 0x10000 +
                                (((unsigned int)(cp - 0xD800)) << 10) +
                                (unsigned int)(low - 0xDC00);
                            len += _jd_utf8(full, buf + len);
                            break;
                        }
                        *pp = save;
                    }
                    len += _jd_utf8((unsigned int)cp, buf + len);
                    break;
                }
                default: buf[len++] = **pp; (*pp)++; break;
            }
        } else { buf[len++] = **pp; (*pp)++; }
    }
    if (**pp == '"') (*pp)++;
    buf[len] = 0;
    return sw_val_string(buf);
}

static sw_val_t *_jd_parse(const char **pp) {
    if (g_jd_depth >= SW_JD_MAX_DEPTH) return sw_val_nil();  /* too deep */
    g_jd_depth++;
    sw_val_t *r = _jd_parse_inner(pp);
    g_jd_depth--;
    return r;
}

static sw_val_t *_jd_parse_inner(const char **pp) {
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
    if (*pp == start) {
        /* Unparseable char where a value was expected. Force progress so
         * callers' loops can't spin — but NEVER past the NUL terminator:
         * at end-of-input (e.g. `{"a":` then EOF) stepping past `\0` is a
         * heap-buffer-overflow (found by fuzz-json). The enclosing array/
         * object loops gate on `**pp`, so leaving pp at the NUL ends them. */
        if (**pp != '\0') (*pp)++;
        return sw_val_nil();
    }
    char tmp[64];
    size_t numlen = *pp - start;
    if (numlen > 63) numlen = 63;
    memcpy(tmp, start, numlen); tmp[numlen] = 0;
    return is_float ? sw_val_float(strtod(tmp, NULL)) : sw_val_int(strtoll(tmp, NULL, 10));
}

sw_val_t *sw_lang_json_decode(const char *s) {
    if (!s) return sw_val_nil();
    const char *p = s;
    g_jd_depth = 0;   /* reset the per-decode depth guard */
    return _jd_parse(&p);
}
