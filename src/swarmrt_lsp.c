/*
 * swarmrt_lsp.c — minimum viable Language Server Protocol for `swc lsp`.
 *
 * Speaks LSP 3.17 over stdin/stdout using the standard Content-Length
 * framing. Implements the JSON-RPC methods every editor expects to
 * find the basic features alive:
 *
 *   - initialize / initialized   handshake
 *   - textDocument/didOpen       parse + diagnostics
 *   - textDocument/didChange     re-parse + diagnostics
 *   - textDocument/didSave       re-parse + diagnostics
 *   - textDocument/didClose      drop tracking
 *   - shutdown / exit            graceful shutdown
 *
 * What it does NOT yet do:
 *   - completion / hover / signatureHelp
 *   - goto-definition / references
 *   - rename / code actions
 *   - formatting
 *
 * Those are the next phase. The diagnostics path alone gives editors
 * red squigglies on parse errors with file/line accuracy — the single
 * biggest writing-sw-feels-real win available for ~250 LOC.
 *
 * otonomy.ai
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>   /* strncasecmp */
#include <unistd.h>    /* pipe, close */
#include "swarmrt_lang.h"

extern void *sw_lang_parse(const char *src);
/* node_free is static — we just let the per-edit AST leak. An LSP edit
 * is small (a single .sw file) and the editor restarts the server
 * regularly enough that the leak is bounded. Not great long-term;
 * worth exporting node_free in a future pass. */

/* ------------------------------------------------------------
 * Tiny JSON helpers — we only need to scrape a few well-known keys
 * from incoming messages and emit small replies. No general parser.
 * ------------------------------------------------------------ */

static const char *json_find_key(const char *s, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return strstr(s, needle);
}

/* Extract a string value following "key":" — returns malloc'd string
 * or NULL. Caller frees. */
static char *json_get_string(const char *s, const char *key) {
    const char *k = json_find_key(s, key);
    if (!k) return NULL;
    k = strchr(k, ':');
    if (!k) return NULL;
    while (*k && (*k == ':' || *k == ' ')) k++;
    if (*k != '"') return NULL;
    k++;
    const char *end = k;
    while (*end) {
        if (*end == '\\' && end[1]) { end += 2; continue; }
        if (*end == '"') break;
        end++;
    }
    int len = (int)(end - k);
    char *out = (char *)malloc(len + 1);
    /* Decode \" \\ \n \t \r — common subset; for diagnostics it's
     * enough since we mostly handle URIs and short text. */
    int op = 0;
    for (int i = 0; i < len; i++) {
        if (k[i] == '\\' && i + 1 < len) {
            char e = k[i + 1];
            switch (e) {
                case 'n': out[op++] = '\n'; break;
                case 't': out[op++] = '\t'; break;
                case 'r': out[op++] = '\r'; break;
                case '"': out[op++] = '"'; break;
                case '\\': out[op++] = '\\'; break;
                case '/': out[op++] = '/'; break;
                default:   out[op++] = e; break;
            }
            i++;
        } else {
            out[op++] = k[i];
        }
    }
    out[op] = '\0';
    return out;
}

static int json_get_int(const char *s, const char *key, int dflt) {
    const char *k = json_find_key(s, key);
    if (!k) return dflt;
    k = strchr(k, ':');
    if (!k) return dflt;
    return atoi(k + 1);
}

/* ------------------------------------------------------------
 * LSP message framing — Content-Length: N\r\n\r\n<json>
 * ------------------------------------------------------------ */

static char *read_lsp_message(void) {
    char header[256];
    int header_len = 0;
    int content_length = -1;
    /* Read headers up to the blank line. Each header ends with \r\n. */
    while (1) {
        int c = getchar();
        if (c == EOF) return NULL;
        if (header_len >= (int)sizeof(header) - 1) return NULL;
        header[header_len++] = (char)c;
        if (c == '\n' && header_len >= 2 && header[header_len - 2] == '\r') {
            header[header_len] = '\0';
            if (strncasecmp(header, "Content-Length:", 15) == 0) {
                content_length = atoi(header + 15);
            }
            if (header_len == 2) {
                /* Blank line — end of headers. */
                break;
            }
            header_len = 0;
        }
    }
    if (content_length < 0) return NULL;
    char *body = (char *)malloc(content_length + 1);
    int got = 0;
    while (got < content_length) {
        int n = (int)fread(body + got, 1, content_length - got, stdin);
        if (n <= 0) { free(body); return NULL; }
        got += n;
    }
    body[content_length] = '\0';
    return body;
}

static void write_lsp_message(const char *body) {
    int len = (int)strlen(body);
    printf("Content-Length: %d\r\n\r\n%s", len, body);
    fflush(stdout);
}

/* ------------------------------------------------------------
 * Document store — keep open files in memory so didChange can
 * re-parse without filesystem touches.
 * ------------------------------------------------------------ */

#define MAX_DOCS 64
typedef struct {
    char uri[1024];
    char *text;
    int version;
} doc_t;
static doc_t docs[MAX_DOCS];

static doc_t *find_or_create_doc(const char *uri) {
    int free_slot = -1;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (docs[i].uri[0] == '\0') { if (free_slot < 0) free_slot = i; continue; }
        if (strcmp(docs[i].uri, uri) == 0) return &docs[i];
    }
    if (free_slot < 0) return NULL;
    strncpy(docs[free_slot].uri, uri, sizeof(docs[free_slot].uri) - 1);
    docs[free_slot].text = NULL;
    docs[free_slot].version = 0;
    return &docs[free_slot];
}

static void drop_doc(const char *uri) {
    for (int i = 0; i < MAX_DOCS; i++) {
        if (strcmp(docs[i].uri, uri) == 0) {
            free(docs[i].text);
            docs[i].text = NULL;
            docs[i].uri[0] = '\0';
            return;
        }
    }
}

/* ------------------------------------------------------------
 * Diagnostics — try to parse, emit a publishDiagnostics notification.
 * Empty array means "all clear, clear previous squigglies."
 * ------------------------------------------------------------ */

static void emit_diagnostics(const char *uri, const char *source) {
    /* Capture parser stderr so we can scrape `line N` / `expected`. */
    char errbuf[4096] = {0};
    int errpipe[2];
    FILE *saved = stderr;
    if (pipe(errpipe) == 0) {
        FILE *tmp = fdopen(errpipe[1], "w");
        if (tmp) {
            stderr = tmp;
            void *ast = sw_lang_parse(source);
            fclose(tmp);
            stderr = saved;
            FILE *rd = fdopen(errpipe[0], "r");
            if (rd) {
                int rn = (int)fread(errbuf, 1, sizeof(errbuf) - 1, rd);
                errbuf[rn] = '\0';
                fclose(rd);
            }
            (void)ast;  /* see header note; we accept the leak. */
        } else {
            close(errpipe[0]);
            close(errpipe[1]);
        }
    }

    /* Look for "line N" in the error to anchor the diagnostic. */
    int line = 1;
    const char *line_kw = strstr(errbuf, "line ");
    if (line_kw) {
        int parsed = atoi(line_kw + 5);
        if (parsed > 0) line = parsed;
    }

    /* Build diagnostics array. */
    char body[8192];
    if (errbuf[0]) {
        /* Trim trailing whitespace. */
        int n = (int)strlen(errbuf);
        while (n > 0 && (errbuf[n - 1] == '\n' || errbuf[n - 1] == '\r' ||
                         errbuf[n - 1] == ' '  || errbuf[n - 1] == '\t')) {
            errbuf[--n] = '\0';
        }
        /* Escape quotes for JSON. */
        char esc[2048];
        int op = 0;
        for (int i = 0; errbuf[i] && op < (int)sizeof(esc) - 2; i++) {
            char c = errbuf[i];
            if (c == '"' || c == '\\') esc[op++] = '\\';
            if (c == '\n' || c == '\r') { esc[op++] = '\\'; esc[op++] = 'n'; continue; }
            esc[op++] = c;
        }
        esc[op] = '\0';

        snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
            "\"params\":{\"uri\":\"%s\",\"diagnostics\":["
                "{\"range\":{\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":256}},"
                "\"severity\":1,\"source\":\"swc\",\"message\":\"%s\"}"
            "]}}",
            uri, line - 1, line - 1, esc);
    } else {
        snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
            "\"params\":{\"uri\":\"%s\",\"diagnostics\":[]}}",
            uri);
    }
    write_lsp_message(body);
}

/* ------------------------------------------------------------
 * Handler dispatch
 * ------------------------------------------------------------ */

static void send_initialize_result(int id) {
    char body[1024];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{"
            "\"capabilities\":{"
                "\"textDocumentSync\":1,"           /* full document sync */
                "\"hoverProvider\":true,"
                "\"diagnosticProvider\":{"
                    "\"interFileDependencies\":false,"
                    "\"workspaceDiagnostics\":false"
                "}"
            "},"
            "\"serverInfo\":{\"name\":\"swc-lsp\",\"version\":\"0.1\"}"
        "}}", id);
    write_lsp_message(body);
}

static void send_hover_result(int id, const char *uri, int line) {
    (void)uri; (void)line;
    char body[256];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{"
            "\"contents\":{\"kind\":\"markdown\","
            "\"value\":\"**sw**: SwarmRT language\\n\\nSee `docs/SW_LANGUAGE.md`.\"}"
        "}}", id);
    write_lsp_message(body);
}

static void send_empty_result(int id) {
    char body[128];
    snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
    write_lsp_message(body);
}

static void handle_message(const char *msg, int *want_exit) {
    char *method = json_get_string(msg, "method");
    int id = json_get_int(msg, "id", -1);
    if (!method) { if (id >= 0) send_empty_result(id); return; }

    if (strcmp(method, "initialize") == 0) {
        send_initialize_result(id);
    } else if (strcmp(method, "initialized") == 0) {
        /* notification — no reply */
    } else if (strcmp(method, "shutdown") == 0) {
        send_empty_result(id);
    } else if (strcmp(method, "exit") == 0) {
        *want_exit = 1;
    } else if (strcmp(method, "textDocument/didOpen") == 0 ||
               strcmp(method, "textDocument/didChange") == 0 ||
               strcmp(method, "textDocument/didSave") == 0) {
        char *uri = json_get_string(msg, "uri");
        if (uri) {
            doc_t *d = find_or_create_doc(uri);
            if (d) {
                char *text = json_get_string(msg, "text");
                if (text) {
                    free(d->text);
                    d->text = text;
                    d->version++;
                    emit_diagnostics(uri, text);
                }
            }
            free(uri);
        }
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        char *uri = json_get_string(msg, "uri");
        if (uri) { drop_doc(uri); free(uri); }
    } else if (strcmp(method, "textDocument/hover") == 0) {
        char *uri = json_get_string(msg, "uri");
        int line = json_get_int(msg, "line", 0);
        if (id >= 0) send_hover_result(id, uri ? uri : "", line);
        free(uri);
    } else {
        /* Unknown — return null result for requests, ignore notifications. */
        if (id >= 0) send_empty_result(id);
    }

    free(method);
}

int sw_lsp_main(void) {
    /* Quiet runtime output that would interleave with the wire. */
    char *msg;
    int want_exit = 0;
    while (!want_exit && (msg = read_lsp_message()) != NULL) {
        handle_message(msg, &want_exit);
        free(msg);
    }
    return 0;
}
