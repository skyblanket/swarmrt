/*
 * coder/tools.c — Bash, Read, Write, Edit.
 *
 * We re-use the tiny JSON parser from llm.c for reading arguments. Since it
 * is static there, we keep this file standalone and ship a *second* micro
 * JSON reader that only extracts string fields from a flat object — that's
 * all the tool arguments we actually care about.
 */
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

/* ------------------------------------------------------------
 * Tiny field extractor: given a flat JSON object string, return
 * the unescaped string value of `key`, or NULL if missing/not a string.
 * Caller must free.
 * ------------------------------------------------------------ */
static char *json_get_str(const char *json, const char *key) {
    if (!json) return NULL;
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        /* Try to match "key" at p */
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 2 + klen;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q != ':') { p++; continue; }
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q != '"') return NULL; /* not a string value */
            q++;
            /* collect until unescaped closing quote */
            size_t cap = 128, len = 0;
            char *out = malloc(cap);
            while (*q) {
                if (*q == '\\' && *(q + 1)) {
                    char esc = *(q + 1);
                    char c = 0;
                    switch (esc) {
                        case '"': c = '"'; break;
                        case '\\': c = '\\'; break;
                        case '/': c = '/'; break;
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'f': c = '\f'; break;
                        case 'u': {
                            /* minimal: decode \uXXXX (BMP) → UTF-8 */
                            if (!isxdigit((unsigned char)q[2]) || !isxdigit((unsigned char)q[3]) ||
                                !isxdigit((unsigned char)q[4]) || !isxdigit((unsigned char)q[5])) {
                                free(out); return NULL;
                            }
                            unsigned cp = 0;
                            for (int i = 2; i < 6; i++) {
                                char h = q[i];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= h - '0';
                                else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            }
                            if (len + 4 >= cap) { cap *= 2; out = realloc(out, cap); }
                            if (cp < 0x80) out[len++] = (char)cp;
                            else if (cp < 0x800) {
                                out[len++] = (char)(0xC0 | (cp >> 6));
                                out[len++] = (char)(0x80 | (cp & 0x3F));
                            } else {
                                out[len++] = (char)(0xE0 | (cp >> 12));
                                out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                out[len++] = (char)(0x80 | (cp & 0x3F));
                            }
                            q += 6;
                            continue;
                        }
                        default: c = esc; break;
                    }
                    if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                    out[len++] = c;
                    q += 2;
                    continue;
                }
                if (*q == '"') {
                    out[len] = '\0';
                    return out;
                }
                if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                out[len++] = *q++;
            }
            free(out);
            return NULL;
        }
        p++;
    }
    return NULL;
}

/* Wrap a message as a safe tool-result string (plain text, not JSON). */
static char *mk_result(const char *fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* ------------------------------------------------------------
 * bash
 * ------------------------------------------------------------ */
static char *tool_bash(const char *args_json) {
    char *cmd = json_get_str(args_json, "command");
    if (!cmd) cmd = json_get_str(args_json, "cmd");
    if (!cmd) return mk_result("error: missing 'command' argument");

    /* Execute the command through /bin/sh, capturing combined stdout+stderr.
     * We redirect stderr into stdout via the shell so everything lands in popen. */
    size_t wrap_cap = strlen(cmd) + 64;
    char *wrap = malloc(wrap_cap);
    snprintf(wrap, wrap_cap, "%s 2>&1", cmd);

    FILE *pp = popen(wrap, "r");
    free(wrap);
    if (!pp) {
        char *r = mk_result("error: popen failed: %s", strerror(errno));
        free(cmd); return r;
    }

    size_t cap = 8192, len = 0;
    char *out = malloc(cap);
    char chunk[2048];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, pp)) > 0) {
        if (len + r + 1 > cap) {
            while (len + r + 1 > cap) cap *= 2;
            out = realloc(out, cap);
        }
        memcpy(out + len, chunk, r);
        len += r;
    }
    out[len] = '\0';

    /* Cap output so we don't blow the context. */
    const size_t MAX_OUT = 16000;
    if (len > MAX_OUT) {
        size_t head = MAX_OUT / 2;
        size_t tail_off = len - (MAX_OUT - head - 64);
        char *trimmed = malloc(MAX_OUT + 128);
        int w = snprintf(trimmed, MAX_OUT + 128,
                         "%.*s\n... [%zu bytes elided] ...\n%s",
                         (int)head, out, len - MAX_OUT, out + tail_off);
        (void)w;
        free(out);
        out = trimmed;
    }

    int rc = pclose(pp);
    int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

    /* Prepend exit code. */
    size_t final_cap = strlen(out) + 64;
    char *final = malloc(final_cap);
    snprintf(final, final_cap, "[exit %d]\n%s", exit_code, out);
    free(out);
    free(cmd);
    return final;
}

/* ------------------------------------------------------------
 * read
 * ------------------------------------------------------------ */
static char *tool_read(const char *args_json) {
    char *path = json_get_str(args_json, "path");
    if (!path) return mk_result("error: missing 'path' argument");

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char *r = mk_result("error: could not open %s: %s", path, strerror(errno));
        free(path);
        return r;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    const long MAX_READ = 200 * 1024; /* 200KB cap */
    long to_read = sz;
    int truncated = 0;
    if (to_read > MAX_READ) { to_read = MAX_READ; truncated = 1; }

    char *buf = malloc(to_read + 1);
    size_t got = fread(buf, 1, to_read, fp);
    buf[got] = '\0';
    fclose(fp);

    char *result;
    if (truncated) {
        size_t cap = got + 128;
        result = malloc(cap);
        snprintf(result, cap, "%s\n\n[truncated: file is %ld bytes, showed first %ld]",
                 buf, sz, to_read);
        free(buf);
    } else {
        result = buf;
    }
    free(path);
    return result;
}

/* ------------------------------------------------------------
 * write
 * ------------------------------------------------------------ */
static char *tool_write(const char *args_json) {
    char *path = json_get_str(args_json, "path");
    char *content = json_get_str(args_json, "content");
    if (!path || !content) {
        free(path); free(content);
        return mk_result("error: need both 'path' and 'content' arguments");
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        char *r = mk_result("error: could not open %s for writing: %s", path, strerror(errno));
        free(path); free(content);
        return r;
    }
    size_t clen = strlen(content);
    size_t w = fwrite(content, 1, clen, fp);
    fclose(fp);
    char *r;
    if (w != clen) r = mk_result("error: short write to %s (%zu of %zu)", path, w, clen);
    else r = mk_result("ok: wrote %zu bytes to %s", clen, path);
    free(path); free(content);
    return r;
}

/* ------------------------------------------------------------
 * edit — string replace (old_string → new_string, first occurrence)
 * ------------------------------------------------------------ */
static char *tool_edit(const char *args_json) {
    char *path = json_get_str(args_json, "path");
    char *old_s = json_get_str(args_json, "old_string");
    char *new_s = json_get_str(args_json, "new_string");
    if (!path || !old_s || !new_s) {
        free(path); free(old_s); free(new_s);
        return mk_result("error: need 'path', 'old_string', 'new_string'");
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char *r = mk_result("error: could not open %s: %s", path, strerror(errno));
        free(path); free(old_s); free(new_s);
        return r;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    char *hit = strstr(buf, old_s);
    if (!hit) {
        free(buf);
        char *r = mk_result("error: old_string not found in %s", path);
        free(path); free(old_s); free(new_s);
        return r;
    }
    /* Check uniqueness — refuse ambiguous edits. */
    if (strstr(hit + 1, old_s)) {
        free(buf);
        char *r = mk_result("error: old_string appears multiple times in %s; "
                            "make it more specific", path);
        free(path); free(old_s); free(new_s);
        return r;
    }

    size_t old_len = strlen(old_s);
    size_t new_len = strlen(new_s);
    size_t final_len = (size_t)sz - old_len + new_len;
    char *out = malloc(final_len + 1);
    size_t prefix = (size_t)(hit - buf);
    memcpy(out, buf, prefix);
    memcpy(out + prefix, new_s, new_len);
    memcpy(out + prefix + new_len, hit + old_len, (size_t)sz - prefix - old_len);
    out[final_len] = '\0';

    FILE *wp = fopen(path, "wb");
    if (!wp) {
        free(buf); free(out);
        char *r = mk_result("error: could not reopen %s for write: %s", path, strerror(errno));
        free(path); free(old_s); free(new_s);
        return r;
    }
    fwrite(out, 1, final_len, wp);
    fclose(wp);

    free(buf); free(out);
    char *r = mk_result("ok: replaced 1 occurrence in %s (%ld → %zu bytes)",
                        path, sz, final_len);
    free(path); free(old_s); free(new_s);
    return r;
}

/* ------------------------------------------------------------
 * Dispatch
 * ------------------------------------------------------------ */
char *coder_tool_exec(const char *name, const char *arguments_json) {
    if (!name) return mk_result("error: no tool name");
    if (strcmp(name, "bash") == 0) return tool_bash(arguments_json);
    if (strcmp(name, "read") == 0) return tool_read(arguments_json);
    if (strcmp(name, "write") == 0) return tool_write(arguments_json);
    if (strcmp(name, "edit") == 0) return tool_edit(arguments_json);
    return mk_result("error: unknown tool '%s'", name);
}

/* ------------------------------------------------------------
 * Tool schemas exposed to the model
 * ------------------------------------------------------------ */
const llm_tool_def_t CODER_TOOLS[] = {
    {
        .name = "bash",
        .description =
            "Run a shell command and return its combined stdout+stderr and exit code. "
            "Use this for executing code, running tests, inspecting the filesystem, "
            "grepping, or anything else that needs a shell. The command runs in the "
            "current working directory.",
        .params_schema_json =
            "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute.\"}"
                "},"
                "\"required\":[\"command\"]"
            "}"
    },
    {
        .name = "read",
        .description =
            "Read the contents of a file at the given path. Returns up to 200KB; "
            "larger files are truncated with a note.",
        .params_schema_json =
            "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"path\":{\"type\":\"string\",\"description\":\"Absolute or relative file path.\"}"
                "},"
                "\"required\":[\"path\"]"
            "}"
    },
    {
        .name = "write",
        .description =
            "Create a new file or overwrite an existing one with the given content. "
            "Be careful — this replaces the whole file.",
        .params_schema_json =
            "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"path\":{\"type\":\"string\"},"
                    "\"content\":{\"type\":\"string\"}"
                "},"
                "\"required\":[\"path\",\"content\"]"
            "}"
    },
    {
        .name = "edit",
        .description =
            "Edit an existing file by replacing one exact string with another. "
            "old_string must match exactly once in the file; otherwise the edit is "
            "refused (to force you to disambiguate).",
        .params_schema_json =
            "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"path\":{\"type\":\"string\"},"
                    "\"old_string\":{\"type\":\"string\"},"
                    "\"new_string\":{\"type\":\"string\"}"
                "},"
                "\"required\":[\"path\",\"old_string\",\"new_string\"]"
            "}"
    }
};

const int CODER_TOOLS_COUNT = (int)(sizeof(CODER_TOOLS) / sizeof(CODER_TOOLS[0]));
