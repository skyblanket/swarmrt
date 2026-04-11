/*
 * SwarmRT MCP Server — One MCP to Replace Them All
 *
 * Native MCP server for Claude Code backed by SwarmRT's:
 * - BM25 + fuzzy search (replaces Claude Context, Codebase Memory MCP)
 * - Persistent memory with search (replaces SuperMemory, Claude-Mem)
 * - Session context accumulation
 * - Process introspection
 *
 * Single binary, ~200KB, zero npm/pip dependencies.
 *
 * Protocol: MCP (JSON-RPC 2.0 over stdio, newline-delimited)
 * Build:    make mcp
 * Install:  ./mcp/install.sh
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include <fnmatch.h>
#include <math.h>
#include <pwd.h>
#include <regex.h>
#include <ctype.h>

#include "../src/swarmrt_search.h"

/* ================================================================
 * Section 1: Configuration
 * ================================================================ */

#define MCP_VERSION         "0.4.0"
#define MCP_NAME            "swarmrt-mcp"
#define MAX_LINE            (4 * 1024 * 1024)  /* 4MB max JSON-RPC message */
#define MAX_TOOLS           48
#define MAX_RESULTS         20
#define MAX_MEMORIES        8192
#define MAX_SESSION_EVENTS  4096
#define MAX_AUTOPILOT_STEPS 64
#define MEMORY_FILE         "memories.sws"
#define INDEX_FILE          "index.sws"
#define DATA_DIR_NAME       ".swarmrt"
#define AUTOPILOT_FILE      "autopilot.json"
#define WAKES_FILE          "wakes.json"
#define WAKE_QUEUE_FILE     "wake_queue.jsonl"
#define MAX_WAKES           64

/* Auto-indexer config */
#define INDEX_MAX_FILE_SIZE (256 * 1024)
#define INDEX_PREVIEW_SIZE  4096
#define INDEX_EXCLUDE_COUNT 32
#define INDEX_MAX_FILES     50000  /* bail if too many files (e.g. $HOME) */

static const char *DEFAULT_EXCLUDES[] = {
    "node_modules", ".git", ".svn", "__pycache__", ".cache",
    "target", "build", "dist", ".next", ".nuxt",
    "vendor", "deps", "_build", ".elixir_ls", ".zig-cache",
    "*.pyc", "*.o", "*.so", "*.dylib", "*.a",
    "*.jpg", "*.png", "*.gif", "*.ico", "*.woff*",
    "*.zip", "*.tar", "*.gz", "*.pdf",
    ".DS_Store", "Thumbs.db", "*.lock",
    NULL
};

/* Forward declaration */
static void mcp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* .gitignore support — merged into exclude list at startup */
#define MAX_GITIGNORE_PATTERNS 256
static char *g_gitignore_patterns[MAX_GITIGNORE_PATTERNS];
static int g_gitignore_count = 0;

static void load_gitignore(const char *project_root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/.gitignore", project_root);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f) && g_gitignore_count < MAX_GITIGNORE_PATTERNS) {
        /* Strip trailing newline/whitespace */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t'))
            line[--len] = '\0';

        /* Skip empty lines, comments, negation patterns */
        if (len == 0 || line[0] == '#' || line[0] == '!') continue;

        /* Strip trailing slash (directory marker) */
        if (len > 0 && line[len-1] == '/') line[--len] = '\0';
        if (len == 0) continue;

        /* Store pattern — if it has no slash, it matches basename (fnmatch)
         * If it has a leading slash, strip it (relative to root) */
        char *pat = line;
        if (pat[0] == '/') pat++;

        g_gitignore_patterns[g_gitignore_count++] = strdup(pat);
    }
    fclose(f);

    if (g_gitignore_count > 0)
        mcp_log("loaded %d patterns from .gitignore", g_gitignore_count);
}

static void free_gitignore(void) {
    for (int i = 0; i < g_gitignore_count; i++)
        free(g_gitignore_patterns[i]);
    g_gitignore_count = 0;
}

/* ================================================================
 * Section 2: Minimal JSON Parser
 * ================================================================ */

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_INT, JSON_FLOAT, JSON_STRING,
    JSON_ARRAY, JSON_OBJECT
} json_type_t;

typedef struct json_node {
    json_type_t type;
    char *key;              /* NULL for array elements and root */
    union {
        bool bval;
        int64_t ival;
        double fval;
        char *sval;         /* owned, malloc'd */
        struct {
            struct json_node **items;
            int count;
            int cap;
        } children;         /* for ARRAY and OBJECT */
    };
} json_node_t;

#define JSON_MAX_DEPTH 64
#define JSON_MAX_STRING (1 * 1024 * 1024) /* 1MB max string */

static json_node_t *json_parse_depth(const char **p, int depth);
static json_node_t *json_parse(const char **p) { return json_parse_depth(p, 0); }

static void json_free(json_node_t *n) {
    if (!n) return;
    free(n->key);
    if (n->type == JSON_STRING) free(n->sval);
    if (n->type == JSON_ARRAY || n->type == JSON_OBJECT) {
        for (int i = 0; i < n->children.count; i++)
            json_free(n->children.items[i]);
        free(n->children.items);
    }
    free(n);
}

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static json_node_t *json_new(json_type_t type) {
    json_node_t *n = calloc(1, sizeof(json_node_t));
    n->type = type;
    return n;
}

static void json_add_child(json_node_t *parent, json_node_t *child) {
    if (parent->children.count >= parent->children.cap) {
        parent->children.cap = parent->children.cap ? parent->children.cap * 2 : 8;
        parent->children.items = realloc(parent->children.items,
            parent->children.cap * sizeof(json_node_t *));
    }
    parent->children.items[parent->children.count++] = child;
}

static char *json_parse_string_raw(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    /* Find end, handling escapes */
    size_t len = 0;
    size_t cap = 256;
    char *buf = malloc(cap);
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            char c = **p;
            switch (c) {
                case '"': case '\\': case '/': buf[len++] = c; break;
                case 'n': buf[len++] = '\n'; break;
                case 't': buf[len++] = '\t'; break;
                case 'r': buf[len++] = '\r'; break;
                case 'b': buf[len++] = '\b'; break;
                case 'f': buf[len++] = '\f'; break;
                case 'u': { /* decode \uXXXX to UTF-8 */
                    uint32_t cp = 0;
                    for (int i = 0; i < 4 && (*p)[1]; i++) {
                        (*p)++;
                        char h = **p;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
                        else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
                    }
                    /* Encode as UTF-8 */
                    if (cp < 0x80) {
                        buf[len++] = (char)cp;
                    } else if (cp < 0x800) {
                        buf[len++] = (char)(0xC0 | (cp >> 6));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[len++] = (char)(0xE0 | (cp >> 12));
                        buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: buf[len++] = c; break;
            }
        } else {
            buf[len++] = **p;
        }
        if (len >= cap - 4) { /* -4 for multi-byte UTF-8 */
            if (cap >= JSON_MAX_STRING) break;
            cap *= 2;
            if (cap > JSON_MAX_STRING) cap = JSON_MAX_STRING;
            buf = realloc(buf, cap);
        }
        (*p)++;
    }
    if (**p == '"') (*p)++;
    buf[len] = '\0';
    return buf;
}

static json_node_t *json_parse_depth(const char **p, int depth) {
    if (depth > JSON_MAX_DEPTH) return NULL;
    skip_ws(p);
    if (!**p) return NULL;

    if (**p == '"') {
        json_node_t *n = json_new(JSON_STRING);
        n->sval = json_parse_string_raw(p);
        return n;
    }

    if (**p == '{') {
        (*p)++;
        json_node_t *n = json_new(JSON_OBJECT);
        skip_ws(p);
        if (**p == '}') { (*p)++; return n; }
        while (1) {
            skip_ws(p);
            char *key = json_parse_string_raw(p);
            skip_ws(p);
            if (**p == ':') (*p)++;
            json_node_t *val = json_parse_depth(p, depth + 1);
            if (val) { val->key = key; json_add_child(n, val); }
            else { free(key); }
            skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == '}') { (*p)++; break; }
            break; /* malformed */
        }
        return n;
    }

    if (**p == '[') {
        (*p)++;
        json_node_t *n = json_new(JSON_ARRAY);
        skip_ws(p);
        if (**p == ']') { (*p)++; return n; }
        while (1) {
            json_node_t *val = json_parse_depth(p, depth + 1);
            if (val) json_add_child(n, val);
            skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == ']') { (*p)++; break; }
            break;
        }
        return n;
    }

    /* true / false / null */
    if (strncmp(*p, "true", 4) == 0)  { *p += 4; json_node_t *n = json_new(JSON_BOOL); n->bval = true; return n; }
    if (strncmp(*p, "false", 5) == 0) { *p += 5; json_node_t *n = json_new(JSON_BOOL); n->bval = false; return n; }
    if (strncmp(*p, "null", 4) == 0)  { *p += 4; return json_new(JSON_NULL); }

    /* number */
    {
        char *end;
        int64_t iv = strtoll(*p, &end, 10);
        if (end != *p) {
            if (*end == '.' || *end == 'e' || *end == 'E') {
                double fv = strtod(*p, &end);
                json_node_t *n = json_new(JSON_FLOAT);
                n->fval = fv;
                *p = end;
                return n;
            }
            json_node_t *n = json_new(JSON_INT);
            n->ival = iv;
            *p = end;
            return n;
        }
    }

    return NULL;
}

static json_node_t *json_get(json_node_t *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->children.count; i++) {
        if (obj->children.items[i]->key && strcmp(obj->children.items[i]->key, key) == 0)
            return obj->children.items[i];
    }
    return NULL;
}

static const char *json_get_str(json_node_t *obj, const char *key) {
    json_node_t *n = json_get(obj, key);
    return (n && n->type == JSON_STRING) ? n->sval : NULL;
}

static int64_t json_get_int(json_node_t *obj, const char *key, int64_t def) {
    json_node_t *n = json_get(obj, key);
    return (n && n->type == JSON_INT) ? n->ival : def;
}

/* ================================================================
 * Section 3: JSON Response Builder
 * ================================================================ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} jbuf_t;

static void jb_init(jbuf_t *jb) {
    jb->cap = 4096;
    jb->buf = malloc(jb->cap);
    jb->len = 0;
    jb->buf[0] = '\0';
}

static void jb_append(jbuf_t *jb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    while (jb->len + needed + 1 > jb->cap) {
        jb->cap *= 2;
        jb->buf = realloc(jb->buf, jb->cap);
    }
    va_start(ap, fmt);
    jb->len += vsnprintf(jb->buf + jb->len, jb->cap - jb->len, fmt, ap);
    va_end(ap);
}

static void jb_append_escaped(jbuf_t *jb, const char *s) {
    jb_append(jb, "\"");
    for (; *s; s++) {
        switch (*s) {
            case '"':  jb_append(jb, "\\\""); break;
            case '\\': jb_append(jb, "\\\\"); break;
            case '\n': jb_append(jb, "\\n"); break;
            case '\r': jb_append(jb, "\\r"); break;
            case '\t': jb_append(jb, "\\t"); break;
            case '\b': jb_append(jb, "\\b"); break;
            case '\f': jb_append(jb, "\\f"); break;
            default:
                if ((unsigned char)*s < 0x20)
                    jb_append(jb, "\\u%04x", (unsigned char)*s);
                else
                    jb_append(jb, "%c", *s);
                break;
        }
    }
    jb_append(jb, "\"");
}

static void jb_free(jbuf_t *jb) { free(jb->buf); }

/* ================================================================
 * Section 4: Logging
 * ================================================================ */

static void mcp_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[swarmrt-mcp] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ================================================================
 * Section 5: Search Engine
 * ================================================================ */

static void init_data_dir(const char *project_root);  /* forward decl */

static sws_index_t *g_code_index = NULL;
static char g_data_dir[4096] = {0};
static char g_project_root[4096] = {0};
static uint32_t g_files_indexed = 0;
static double g_index_time_ms = 0;

/* FNV-1a 64-bit for path hashing */
static uint64_t fnv1a_64(const char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int is_binary_data(const char *buf, size_t len) {
    size_t check = len < 512 ? len : 512;
    for (size_t i = 0; i < check; i++)
        if (buf[i] == '\0') return 1;
    return 0;
}

static int is_excluded(const char *name) {
    for (int i = 0; DEFAULT_EXCLUDES[i]; i++) {
        if (fnmatch(DEFAULT_EXCLUDES[i], name, 0) == 0) return 1;
    }
    /* Check .gitignore patterns against basename */
    for (int i = 0; i < g_gitignore_count; i++) {
        if (fnmatch(g_gitignore_patterns[i], name, 0) == 0) return 1;
    }
    return 0;
}

/* Path-based exclude: checks gitignore patterns against relative path too */
static int is_path_excluded(const char *relpath) {
    /* Check basename against default + gitignore */
    const char *basename = strrchr(relpath, '/');
    basename = basename ? basename + 1 : relpath;
    if (is_excluded(basename)) return 1;

    /* Check full relative path against gitignore glob patterns */
    for (int i = 0; i < g_gitignore_count; i++) {
        /* Pattern with wildcard or path separator → match full path */
        if (strchr(g_gitignore_patterns[i], '/') ||
            strchr(g_gitignore_patterns[i], '*')) {
            if (fnmatch(g_gitignore_patterns[i], relpath,
                        FNM_PATHNAME) == 0) return 1;
        }
        /* Also check if any path component matches the pattern */
        if (fnmatch(g_gitignore_patterns[i], relpath, 0) == 0) return 1;
    }
    return 0;
}

/* Iterative directory walker + indexer */
static void index_directory(sws_index_t *idx, const char *root) {
    typedef struct dir_entry { char *path; struct dir_entry *next; } dir_entry_t;
    dir_entry_t *stack = calloc(1, sizeof(dir_entry_t));
    stack->path = strdup(root);

    char pathbuf[4096];
    while (stack) {
        dir_entry_t *top = stack;
        stack = top->next;
        char *dir = top->path;
        free(top);

        DIR *d = opendir(dir);
        if (!d) { free(dir); continue; }

        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (g_files_indexed >= INDEX_MAX_FILES) break;
            if (ent->d_name[0] == '.') continue; /* skip hidden */
            if (is_excluded(ent->d_name)) continue;

            snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dir, ent->d_name);
            struct stat st;
            if (lstat(pathbuf, &st) != 0) continue;

            /* Compute relative path for gitignore matching */
            const char *rel = pathbuf;
            size_t rootlen_dir = strlen(root);
            if (strncmp(pathbuf, root, rootlen_dir) == 0 && pathbuf[rootlen_dir] == '/')
                rel = pathbuf + rootlen_dir + 1;

            if (S_ISDIR(st.st_mode)) {
                /* Check gitignore for directories too */
                if (is_path_excluded(rel)) continue;
                dir_entry_t *e = calloc(1, sizeof(dir_entry_t));
                e->path = strdup(pathbuf);
                e->next = stack;
                stack = e;
            } else if (S_ISREG(st.st_mode) && (size_t)st.st_size <= INDEX_MAX_FILE_SIZE) {
                /* Check gitignore for full relative path */
                if (is_path_excluded(rel)) continue;

                FILE *f = fopen(pathbuf, "r");
                if (!f) continue;
                char *buf = malloc(INDEX_PREVIEW_SIZE + 1);
                size_t nr = fread(buf, 1, INDEX_PREVIEW_SIZE, f);
                fclose(f);
                buf[nr] = '\0';

                if (is_binary_data(buf, nr)) { free(buf); continue; }

                /* Use pre-computed relative path */
                const char *relpath = rel;

                /* Document = "relpath\ncontent" */
                size_t rlen = strlen(relpath);
                size_t dlen = rlen + 1 + nr;
                char *doc = malloc(dlen + 1);
                memcpy(doc, relpath, rlen);
                doc[rlen] = '\n';
                memcpy(doc + rlen + 1, buf, nr);
                doc[dlen] = '\0';
                free(buf);

                uint64_t id = fnv1a_64(relpath, rlen);
                sws_add(idx, id, doc, NULL, 0);
                free(doc);
                g_files_indexed++;
            }
        }
        closedir(d);
        free(dir);
    }
}

static bool g_search_ready = false;

/* Lazy init: index on first search call, not startup */
static void ensure_search_ready(void) {
    if (g_search_ready) return;
    g_search_ready = true;

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    g_code_index = sws_new(0); /* text-only, no vectors */

    /* Try loading persisted index */
    char idx_path[4096];
    snprintf(idx_path, sizeof(idx_path), "%s/%s", g_data_dir, INDEX_FILE);
    sws_index_t *loaded = sws_load(idx_path);
    if (loaded) {
        sws_free(g_code_index);
        g_code_index = loaded;
        sws_info_t info;
        sws_info(g_code_index, &info);
        g_files_indexed = info.doc_count;
        mcp_log("loaded persisted index: %u files", g_files_indexed);
    } else {
        /* Index from scratch */
        mcp_log("indexing %s ...", g_project_root);
        index_directory(g_code_index, g_project_root);
        /* Persist */
        sws_save(g_code_index, idx_path);
        mcp_log("indexed %u files, saved to %s", g_files_indexed, idx_path);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    g_index_time_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                      (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
    mcp_log("search ready in %.1fms (%u files)", g_index_time_ms, g_files_indexed);
}

static void init_search(const char *project_root) {
    strncpy(g_project_root, project_root, sizeof(g_project_root) - 1);
    mcp_log("search will lazy-index: %s", project_root);
}

/* Extract file path from doc text (everything before first \n) */
static const char *extract_path(const char *text, char *buf, size_t bufsz) {
    const char *nl = strchr(text, '\n');
    size_t len = nl ? (size_t)(nl - text) : strlen(text);
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';
    return buf;
}

/* Extract content snippet around query match, return line number */
static int extract_snippet(const char *text, const char *query, char *buf, size_t bufsz) {
    const char *nl = strchr(text, '\n');
    if (!nl) { buf[0] = '\0'; return 1; }
    const char *content = nl + 1;
    size_t clen = strlen(content);
    size_t snippet_len = bufsz - 1;
    if (snippet_len > 300) snippet_len = 300;

    /* Find query in content (case-insensitive first word) */
    size_t qlen = strlen(query);
    char word[128];
    size_t wlen = 0;
    for (size_t i = 0; i < qlen && i < sizeof(word) - 1; i++) {
        if (query[i] == ' ') break;
        word[wlen++] = query[i];
    }
    word[wlen] = '\0';

    const char *match = NULL;
    if (wlen > 0) {
        for (size_t i = 0; i + wlen <= clen; i++) {
            int ok = 1;
            for (size_t j = 0; j < wlen; j++) {
                char a = content[i+j], b = word[j];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { ok = 0; break; }
            }
            if (ok) { match = content + i; break; }
        }
    }

    /* Calculate line number of match */
    int line_num = 1;
    const char *scan_to = match ? match : content;
    for (const char *p = content; p < scan_to; p++) {
        if (*p == '\n') line_num++;
    }

    size_t offset = 0;
    if (match) {
        size_t moff = (size_t)(match - content);
        offset = moff > snippet_len / 2 ? moff - snippet_len / 2 : 0;
    }
    size_t avail = clen - offset;
    if (avail > snippet_len) avail = snippet_len;
    for (size_t i = 0; i < avail; i++)
        buf[i] = (content[offset + i] == '\n' || content[offset + i] == '\r') ? ' ' : content[offset + i];
    buf[avail] = '\0';

    return line_num;
}

/* Path-boost: re-rank search results when query matches filename/path.
 * This fixes the #1 fuzzy search issue: content-only matches ranking
 * above exact filename matches. */
static void rerank_with_path_boost(sws_result_t *results, int n,
                                    const char *query) {
    if (n <= 1 || !query || !query[0]) return;

    /* Lowercase query for comparison */
    char q_lower[256];
    int qlen = 0;
    for (int i = 0; query[i] && i < 255; i++)
        q_lower[qlen++] = (query[i] >= 'A' && query[i] <= 'Z')
                          ? query[i] + 32 : query[i];
    q_lower[qlen] = '\0';

    /* Boost scores for path matches */
    for (int i = 0; i < n; i++) {
        char pathbuf[4096];
        extract_path(results[i].text, pathbuf, sizeof(pathbuf));

        /* Lowercase path for comparison */
        char p_lower[4096];
        int plen = 0;
        for (int j = 0; pathbuf[j] && j < 4095; j++)
            p_lower[plen++] = (pathbuf[j] >= 'A' && pathbuf[j] <= 'Z')
                              ? pathbuf[j] + 32 : pathbuf[j];
        p_lower[plen] = '\0';

        /* Extract basename */
        const char *basename = strrchr(p_lower, '/');
        basename = basename ? basename + 1 : p_lower;

        /* Exact basename match → huge boost */
        if (strstr(basename, q_lower)) {
            results[i].score += 10.0f;
        }
        /* Path component match → medium boost */
        else if (strstr(p_lower, q_lower)) {
            results[i].score += 5.0f;
        }
        /* Check individual query words against path */
        else {
            char words[256];
            memcpy(words, q_lower, qlen + 1);
            char *tok = strtok(words, " _-./");
            while (tok) {
                if (strlen(tok) >= 3 && strstr(p_lower, tok)) {
                    results[i].score += 2.0f;
                    break;
                }
                tok = strtok(NULL, " _-./");
            }
        }
    }

    /* Re-sort by boosted score (insertion sort, small n) */
    for (int i = 1; i < n; i++) {
        sws_result_t tmp = results[i];
        int j = i - 1;
        while (j >= 0 && results[j].score < tmp.score) {
            results[j + 1] = results[j];
            j--;
        }
        results[j + 1] = tmp;
    }
}

/* ================================================================
 * Section 6: Memory Engine
 * ================================================================ */

typedef struct {
    char *key;
    char *value;
    char *tags;       /* comma-separated */
    int64_t timestamp;
} memory_entry_t;

static sws_index_t *g_memory_index = NULL;
static memory_entry_t g_memories[MAX_MEMORIES];
static int g_memory_count = 0;
static pthread_mutex_t g_memory_lock = PTHREAD_MUTEX_INITIALIZER;

static void memory_save(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/memories.jsonl", g_data_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_memory_count; i++) {
        /* Simple JSONL: one JSON object per line */
        jbuf_t jb; jb_init(&jb);
        jb_append(&jb, "{\"key\":");
        jb_append_escaped(&jb, g_memories[i].key);
        jb_append(&jb, ",\"value\":");
        jb_append_escaped(&jb, g_memories[i].value);
        jb_append(&jb, ",\"tags\":");
        jb_append_escaped(&jb, g_memories[i].tags ? g_memories[i].tags : "");
        jb_append(&jb, ",\"ts\":%lld}", (long long)g_memories[i].timestamp);
        fprintf(f, "%s\n", jb.buf);
        jb_free(&jb);
    }
    fclose(f);
}

static void memory_load(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/memories.jsonl", g_data_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[65536];
    while (fgets(line, sizeof(line), f) && g_memory_count < MAX_MEMORIES) {
        const char *p = line;
        json_node_t *obj = json_parse(&p);
        if (!obj) continue;
        const char *key = json_get_str(obj, "key");
        const char *val = json_get_str(obj, "value");
        const char *tags = json_get_str(obj, "tags");
        int64_t ts = json_get_int(obj, "ts", 0);
        if (key && val) {
            memory_entry_t *m = &g_memories[g_memory_count];
            m->key = strdup(key);
            m->value = strdup(val);
            m->tags = tags ? strdup(tags) : strdup("");
            m->timestamp = ts;

            /* Add to search index */
            char doc[65536];
            snprintf(doc, sizeof(doc), "%s\n%s\n%s", key, val, m->tags);
            sws_add(g_memory_index, fnv1a_64(key, strlen(key)), doc, NULL, 0);

            g_memory_count++;
        }
        json_free(obj);
    }
    fclose(f);
    mcp_log("loaded %d memories", g_memory_count);
}

static void init_memory(void) {
    g_memory_index = sws_new(0);
    memory_load();
}

/* ================================================================
 * Section 7: Session Context
 * ================================================================ */

typedef struct {
    char *type;     /* "tool_call", "file_read", "search", "edit", "note" */
    char *content;
    int64_t timestamp;
} session_event_t;

static session_event_t g_session[MAX_SESSION_EVENTS];
static int g_session_count = 0;
static sws_index_t *g_session_index = NULL;

static void init_session(void) {
    g_session_index = sws_new(0);
}

static void session_add(const char *type, const char *content) {
    if (g_session_count >= MAX_SESSION_EVENTS) return;
    session_event_t *e = &g_session[g_session_count];
    e->type = strdup(type);
    e->content = strdup(content);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    e->timestamp = ts.tv_sec;

    char doc[65536];
    snprintf(doc, sizeof(doc), "%s\n%s", type, content);
    sws_add(g_session_index, (uint64_t)g_session_count, doc, NULL, 0);
    g_session_count++;
}

/* ================================================================
 * Section 7.5: Autopilot Engine (Ralph Wiggum)
 *
 * Autonomous task loop: set a goal + steps, MCP tracks progress,
 * external hook re-feeds Claude when it tries to stop.
 * State persisted to .swarmrt/autopilot.json
 * ================================================================ */

typedef struct {
    char *text;
    bool done;
} autopilot_step_t;

typedef struct {
    bool active;
    bool paused;
    char *goal;
    autopilot_step_t steps[MAX_AUTOPILOT_STEPS];
    int step_count;
    int current_step;    /* 0-based index of next step to do */
    int64_t started_at;
    int iterations;      /* how many times the loop has run */
} autopilot_state_t;

static autopilot_state_t g_autopilot = {0};
static pthread_mutex_t g_autopilot_lock = PTHREAD_MUTEX_INITIALIZER;

static void autopilot_save(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", g_data_dir, AUTOPILOT_FILE);

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\"active\":%s,\"paused\":%s,\"goal\":",
            g_autopilot.active ? "true" : "false",
            g_autopilot.paused ? "true" : "false");

    /* Write escaped goal */
    fputc('"', f);
    if (g_autopilot.goal) {
        for (const char *p = g_autopilot.goal; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', f);
            if (*p == '\n') { fputc('\\', f); fputc('n', f); continue; }
            if (*p == '\t') { fputc('\\', f); fputc('t', f); continue; }
            fputc(*p, f);
        }
    }
    fputc('"', f);

    fprintf(f, ",\"current_step\":%d,\"iterations\":%d,\"started_at\":%lld,\"steps\":[",
            g_autopilot.current_step, g_autopilot.iterations,
            (long long)g_autopilot.started_at);

    for (int i = 0; i < g_autopilot.step_count; i++) {
        if (i > 0) fputc(',', f);
        fprintf(f, "{\"text\":");
        fputc('"', f);
        for (const char *p = g_autopilot.steps[i].text; p && *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', f);
            if (*p == '\n') { fputc('\\', f); fputc('n', f); continue; }
            fputc(*p, f);
        }
        fputc('"', f);
        fprintf(f, ",\"done\":%s}", g_autopilot.steps[i].done ? "true" : "false");
    }

    fprintf(f, "]}");
    fclose(f);
}

static void autopilot_load(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", g_data_dir, AUTOPILOT_FILE);

    FILE *f = fopen(path, "r");
    if (!f) return;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) { fclose(f); return; }

    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    /* Parse */
    const char *p = buf;
    json_node_t *root = json_parse(&p);
    if (!root) { free(buf); return; }

    json_node_t *active_n = json_get(root, "active");
    if (active_n && active_n->type == JSON_BOOL)
        g_autopilot.active = active_n->bval;

    json_node_t *paused_n = json_get(root, "paused");
    if (paused_n && paused_n->type == JSON_BOOL)
        g_autopilot.paused = paused_n->bval;

    const char *goal = json_get_str(root, "goal");
    if (goal) g_autopilot.goal = strdup(goal);

    g_autopilot.current_step = (int)json_get_int(root, "current_step", 0);
    g_autopilot.iterations = (int)json_get_int(root, "iterations", 0);
    g_autopilot.started_at = json_get_int(root, "started_at", 0);

    json_node_t *steps = json_get(root, "steps");
    if (steps && steps->type == JSON_ARRAY) {
        for (int i = 0; i < steps->children.count && i < MAX_AUTOPILOT_STEPS; i++) {
            json_node_t *s = steps->children.items[i];
            const char *text = json_get_str(s, "text");
            json_node_t *done_n = json_get(s, "done");
            if (text) {
                g_autopilot.steps[g_autopilot.step_count].text = strdup(text);
                g_autopilot.steps[g_autopilot.step_count].done =
                    (done_n && done_n->type == JSON_BOOL) ? done_n->bval : false;
                g_autopilot.step_count++;
            }
        }
    }

    json_free(root);
    free(buf);

    if (g_autopilot.active)
        mcp_log("autopilot active: \"%s\" (step %d/%d, iter %d)",
                g_autopilot.goal, g_autopilot.current_step + 1,
                g_autopilot.step_count, g_autopilot.iterations);
}

static void autopilot_clear(void) {
    free(g_autopilot.goal);
    for (int i = 0; i < g_autopilot.step_count; i++)
        free(g_autopilot.steps[i].text);
    memset(&g_autopilot, 0, sizeof(g_autopilot));
}

/* ================================================================
 * Section 7.6: Wake Engine (time-triggered prompts)
 *
 * Durable cron schedule for injecting prompts into the active
 * Claude Code session. State in .swarmrt/wakes.json — source of
 * truth, re-read live by the swarmrt-wrap PTY daemon, which polls
 * the file on a 5-second tick and writes due prompts to the pty
 * master as if the user had typed them.
 *
 * Why this exists: Claude Code's built-in CronCreate only fires
 * while the REPL is idle and mid-turn, and /schedule requires a
 * cloud environment + claude.ai OAuth + 1-hour minimum. This
 * subsystem has no such constraints. Wakes persist across session
 * restarts and --resume because the state lives in the project's
 * .swarmrt/ directory, not in any Claude session.
 * ================================================================ */

typedef struct {
    uint64_t minute;   /* bits 0..59 */
    uint64_t hour;     /* bits 0..23 */
    uint32_t dom;      /* bits 1..31 */
    uint32_t month;    /* bits 1..12 */
    uint8_t  dow;      /* bits 0..6, 0=Sun */
    bool     dom_star; /* original field was "*" */
    bool     dow_star; /* original field was "*" */
} cron_t;

static bool cron_parse_field(const char *s, int lo_min, int hi_max, uint64_t *mask) {
    *mask = 0;
    char buf[256];
    size_t len = strlen(s);
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, s, len + 1);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int step = 1;
        char *slash = strchr(tok, '/');
        if (slash) {
            *slash = 0;
            step = atoi(slash + 1);
            if (step < 1) return false;
        }
        int lo, hi;
        if (strcmp(tok, "*") == 0) {
            lo = lo_min; hi = hi_max;
        } else {
            char *dash = strchr(tok, '-');
            if (dash) {
                *dash = 0;
                lo = atoi(tok);
                hi = atoi(dash + 1);
            } else {
                lo = atoi(tok);
                hi = lo;
            }
        }
        if (lo < lo_min || hi > hi_max || lo > hi) return false;
        for (int i = lo; i <= hi; i += step) {
            *mask |= (1ULL << i);
        }
    }
    return *mask != 0;
}

static bool cron_parse(const char *expr, cron_t *c) {
    memset(c, 0, sizeof(*c));
    char buf[512];
    size_t len = strlen(expr);
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, expr, len + 1);

    char *fields[5] = {0};
    int fcount = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok && fcount < 5;
         tok = strtok_r(NULL, " \t", &save)) {
        fields[fcount++] = tok;
    }
    if (fcount != 5) return false;

    /* Capture star-ness BEFORE parsing (the parser mutates tokens). */
    c->dom_star = (strcmp(fields[2], "*") == 0);
    c->dow_star = (strcmp(fields[4], "*") == 0);

    uint64_t mask;
    if (!cron_parse_field(fields[0], 0, 59, &mask)) return false;
    c->minute = mask;
    if (!cron_parse_field(fields[1], 0, 23, &mask)) return false;
    c->hour = mask;
    if (!cron_parse_field(fields[2], 1, 31, &mask)) return false;
    c->dom = (uint32_t)mask;
    if (!cron_parse_field(fields[3], 1, 12, &mask)) return false;
    c->month = (uint32_t)mask;
    if (!cron_parse_field(fields[4], 0, 6, &mask)) return false;
    c->dow = (uint8_t)mask;
    return true;
}

/* Compute next fire strictly after `from`. 0 on failure. */
static time_t cron_next_fire(const cron_t *c, time_t from) {
    time_t t = from + 60 - (from % 60);
    if (t <= from) t = from + 60;

    struct tm tm;
    for (int guard = 0; guard < 4 * 366 * 1440; guard++) {
        localtime_r(&t, &tm);

        if (!(c->month & (1U << (tm.tm_mon + 1)))) {
            tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
            tm.tm_mon++;
            if (tm.tm_mon > 11) { tm.tm_mon = 0; tm.tm_year++; }
            t = mktime(&tm);
            if (t == -1) return 0;
            continue;
        }

        bool dom_match = (c->dom & (1U << tm.tm_mday)) != 0;
        bool dow_match = (c->dow & (1U << tm.tm_wday)) != 0;
        bool day_ok;
        if (c->dom_star && c->dow_star)      day_ok = true;
        else if (c->dom_star)                day_ok = dow_match;
        else if (c->dow_star)                day_ok = dom_match;
        else                                  day_ok = dom_match || dow_match; /* Vixie OR */
        if (!day_ok) {
            tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
            tm.tm_mday++;
            t = mktime(&tm);
            if (t == -1) return 0;
            continue;
        }

        if (!(c->hour & (1ULL << tm.tm_hour))) {
            tm.tm_min = 0; tm.tm_sec = 0;
            tm.tm_hour++;
            t = mktime(&tm);
            if (t == -1) return 0;
            continue;
        }

        if (!(c->minute & (1ULL << tm.tm_min))) {
            tm.tm_sec = 0;
            tm.tm_min++;
            t = mktime(&tm);
            if (t == -1) return 0;
            continue;
        }
        return t;
    }
    return 0;
}

typedef struct {
    int       id;
    char     *name;        /* optional */
    char     *cron_expr;
    char     *prompt;
    cron_t    cron;
    bool      enabled;
    int64_t   created_at;
    int64_t   last_fired_at;
    int       fire_count;
} wake_t;

static wake_t g_wakes[MAX_WAKES];
static int    g_wake_count   = 0;
static int    g_wake_next_id = 1;
static pthread_mutex_t g_wakes_lock = PTHREAD_MUTEX_INITIALIZER;

static void wake_free(wake_t *w) {
    free(w->name); w->name = NULL;
    free(w->cron_expr); w->cron_expr = NULL;
    free(w->prompt); w->prompt = NULL;
}

static void wakes_clear(void) {
    for (int i = 0; i < g_wake_count; i++) wake_free(&g_wakes[i]);
    g_wake_count = 0;
    g_wake_next_id = 1;
}

/* Write a JSON-escaped string directly to a FILE*. Mirrors jb_append_escaped. */
static void json_fwrite_escaped(FILE *f, const char *s) {
    fputc('"', f);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
                case '"':  fputs("\\\"", f); break;
                case '\\': fputs("\\\\", f); break;
                case '\n': fputs("\\n", f);  break;
                case '\r': fputs("\\r", f);  break;
                case '\t': fputs("\\t", f);  break;
                case '\b': fputs("\\b", f);  break;
                case '\f': fputs("\\f", f);  break;
                default:
                    if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                    else fputc(*p, f);
            }
        }
    }
    fputc('"', f);
}

static void wakes_save(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", g_data_dir, WAKES_FILE);

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\"next_id\":%d,\"wakes\":[", g_wake_next_id);
    for (int i = 0; i < g_wake_count; i++) {
        wake_t *w = &g_wakes[i];
        if (i > 0) fputc(',', f);
        fprintf(f, "{\"id\":%d,\"name\":", w->id);
        if (w->name) json_fwrite_escaped(f, w->name);
        else         fputs("null", f);
        fprintf(f, ",\"cron\":");
        json_fwrite_escaped(f, w->cron_expr);
        fprintf(f, ",\"prompt\":");
        json_fwrite_escaped(f, w->prompt);
        fprintf(f, ",\"enabled\":%s,\"created_at\":%lld,\"last_fired_at\":%lld,\"fire_count\":%d}",
                w->enabled ? "true" : "false",
                (long long)w->created_at,
                (long long)w->last_fired_at,
                w->fire_count);
    }
    fprintf(f, "]}");
    fclose(f);
}

static void wakes_load(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", g_data_dir, WAKES_FILE);

    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); return; }

    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    const char *p = buf;
    json_node_t *root = json_parse(&p);
    if (!root) { free(buf); return; }

    int next_id = (int)json_get_int(root, "next_id", 1);
    json_node_t *arr = json_get(root, "wakes");
    if (arr && arr->type == JSON_ARRAY) {
        for (int i = 0; i < arr->children.count && g_wake_count < MAX_WAKES; i++) {
            json_node_t *w_n = arr->children.items[i];
            const char *cron_s = json_get_str(w_n, "cron");
            const char *prompt = json_get_str(w_n, "prompt");
            if (!cron_s || !prompt) continue;
            wake_t *w = &g_wakes[g_wake_count];
            memset(w, 0, sizeof(*w));
            w->id            = (int)json_get_int(w_n, "id", g_wake_count + 1);
            const char *name = json_get_str(w_n, "name");
            w->name          = (name && name[0]) ? strdup(name) : NULL;
            w->cron_expr     = strdup(cron_s);
            w->prompt        = strdup(prompt);
            json_node_t *en  = json_get(w_n, "enabled");
            w->enabled       = (en && en->type == JSON_BOOL) ? en->bval : true;
            w->created_at    = json_get_int(w_n, "created_at", 0);
            w->last_fired_at = json_get_int(w_n, "last_fired_at", 0);
            w->fire_count    = (int)json_get_int(w_n, "fire_count", 0);
            if (!cron_parse(w->cron_expr, &w->cron)) {
                mcp_log("wakes_load: bad cron \"%s\", skipping", w->cron_expr);
                wake_free(w);
                continue;
            }
            g_wake_count++;
        }
    }
    if (next_id > g_wake_next_id) g_wake_next_id = next_id;

    json_free(root);
    free(buf);
    if (g_wake_count > 0)
        mcp_log("loaded %d wake(s)", g_wake_count);
}

static wake_t *wake_find(json_node_t *args) {
    int id = (int)json_get_int(args, "id", -1);
    const char *name = json_get_str(args, "name");
    for (int i = 0; i < g_wake_count; i++) {
        if (id > 0 && g_wakes[i].id == id) return &g_wakes[i];
        if (name && g_wakes[i].name && strcmp(g_wakes[i].name, name) == 0)
            return &g_wakes[i];
    }
    return NULL;
}

static int wake_index_of(wake_t *w) {
    for (int i = 0; i < g_wake_count; i++)
        if (&g_wakes[i] == w) return i;
    return -1;
}

/* --- wake_* tool handlers --- */

static void tool_wake_create(json_node_t *args, jbuf_t *out) {
    const char *expr   = json_get_str(args, "cron_expression");
    const char *prompt = json_get_str(args, "prompt");
    const char *name   = json_get_str(args, "name");

    if (!expr) {
        jb_append(out, "{\"error\":\"cron_expression required (e.g. \\\"*/15 * * * *\\\")\"}");
        return;
    }
    if (!prompt) { jb_append(out, "{\"error\":\"prompt required\"}"); return; }

    cron_t parsed;
    if (!cron_parse(expr, &parsed)) {
        jb_append(out, "{\"error\":\"invalid cron expression\",\"hint\":\"5 fields: minute hour dom month dow. Examples: '*/15 * * * *', '0 */3 * * *', '0 9,13,17 * * 1-5'\"}");
        return;
    }

    pthread_mutex_lock(&g_wakes_lock);

    if (g_wake_count >= MAX_WAKES) {
        pthread_mutex_unlock(&g_wakes_lock);
        jb_append(out, "{\"error\":\"max wakes reached\",\"max\":%d}", MAX_WAKES);
        return;
    }

    if (name) {
        for (int i = 0; i < g_wake_count; i++) {
            if (g_wakes[i].name && strcmp(g_wakes[i].name, name) == 0) {
                pthread_mutex_unlock(&g_wakes_lock);
                jb_append(out, "{\"error\":\"wake with that name already exists\"}");
                return;
            }
        }
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    wake_t *w = &g_wakes[g_wake_count++];
    memset(w, 0, sizeof(*w));
    w->id            = g_wake_next_id++;
    w->name          = name ? strdup(name) : NULL;
    w->cron_expr     = strdup(expr);
    w->prompt        = strdup(prompt);
    w->cron          = parsed;
    w->enabled       = true;
    w->created_at    = ts.tv_sec;
    w->last_fired_at = 0;
    w->fire_count    = 0;

    wakes_save();
    int id = w->id;
    time_t next = cron_next_fire(&w->cron, ts.tv_sec);
    pthread_mutex_unlock(&g_wakes_lock);

    mcp_log("wake created: id=%d cron=\"%s\"", id, expr);
    session_add("wake_create", expr);

    jb_append(out, "{\"id\":%d,\"cron\":", id);
    jb_append_escaped(out, expr);
    jb_append(out, ",\"prompt\":");
    jb_append_escaped(out, prompt);
    jb_append(out, ",\"enabled\":true,\"next_fire_at\":%lld,\"message\":\"Wake created. The swarmrt-wrap PTY daemon will inject the prompt on schedule. Make sure Claude Code was launched via 'swarmrt-wrap claude'.\"}",
              (long long)next);
}

static void tool_wake_list(json_node_t *args, jbuf_t *out) {
    (void)args;
    pthread_mutex_lock(&g_wakes_lock);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    jb_append(out, "{\"wakes\":[");
    for (int i = 0; i < g_wake_count; i++) {
        wake_t *w = &g_wakes[i];
        if (i > 0) jb_append(out, ",");
        jb_append(out, "{\"id\":%d,\"name\":", w->id);
        if (w->name) jb_append_escaped(out, w->name);
        else         jb_append(out, "null");
        jb_append(out, ",\"cron\":");
        jb_append_escaped(out, w->cron_expr);
        jb_append(out, ",\"prompt\":");
        jb_append_escaped(out, w->prompt);
        time_t next = w->enabled ? cron_next_fire(&w->cron, ts.tv_sec) : 0;
        jb_append(out, ",\"enabled\":%s,\"next_fire_at\":%lld,\"last_fired_at\":%lld,\"fire_count\":%d}",
                  w->enabled ? "true" : "false",
                  (long long)next,
                  (long long)w->last_fired_at,
                  w->fire_count);
    }
    jb_append(out, "],\"count\":%d,\"now\":%lld}",
              g_wake_count, (long long)ts.tv_sec);

    pthread_mutex_unlock(&g_wakes_lock);
}

static void tool_wake_delete(json_node_t *args, jbuf_t *out) {
    pthread_mutex_lock(&g_wakes_lock);
    wake_t *w = wake_find(args);
    if (!w) {
        pthread_mutex_unlock(&g_wakes_lock);
        jb_append(out, "{\"error\":\"wake not found\"}");
        return;
    }
    int id = w->id;
    int idx = wake_index_of(w);
    wake_free(w);
    for (int i = idx; i < g_wake_count - 1; i++)
        g_wakes[i] = g_wakes[i + 1];
    g_wake_count--;
    wakes_save();
    pthread_mutex_unlock(&g_wakes_lock);

    mcp_log("wake deleted: id=%d", id);
    jb_append(out, "{\"deleted\":true,\"id\":%d}", id);
}

static void tool_wake_enable(json_node_t *args, jbuf_t *out) {
    json_node_t *en = json_get(args, "enabled");
    if (!en || en->type != JSON_BOOL) {
        jb_append(out, "{\"error\":\"'enabled' boolean required\"}");
        return;
    }
    pthread_mutex_lock(&g_wakes_lock);
    wake_t *w = wake_find(args);
    if (!w) {
        pthread_mutex_unlock(&g_wakes_lock);
        jb_append(out, "{\"error\":\"wake not found\"}");
        return;
    }
    w->enabled = en->bval;
    int id = w->id;
    bool val = w->enabled;
    wakes_save();
    pthread_mutex_unlock(&g_wakes_lock);

    jb_append(out, "{\"id\":%d,\"enabled\":%s}", id, val ? "true" : "false");
}

static void tool_wake_fire_now(json_node_t *args, jbuf_t *out) {
    pthread_mutex_lock(&g_wakes_lock);
    wake_t *w = wake_find(args);
    if (!w) {
        pthread_mutex_unlock(&g_wakes_lock);
        jb_append(out, "{\"error\":\"wake not found\"}");
        return;
    }

    /* Append to wake_queue.jsonl — swarmrt-wrap drains this on its next tick. */
    char qpath[4096];
    snprintf(qpath, sizeof(qpath), "%s/%s", g_data_dir, WAKE_QUEUE_FILE);
    FILE *f = fopen(qpath, "a");
    if (!f) {
        pthread_mutex_unlock(&g_wakes_lock);
        jb_append(out, "{\"error\":\"could not open wake queue\"}");
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "{\"id\":%d,\"queued_at\":%lld,\"prompt\":",
            w->id, (long long)ts.tv_sec);
    json_fwrite_escaped(f, w->prompt);
    fprintf(f, "}\n");
    fclose(f);
    int id = w->id;
    pthread_mutex_unlock(&g_wakes_lock);

    mcp_log("wake queued for immediate fire: id=%d", id);
    jb_append(out, "{\"queued\":true,\"id\":%d,\"message\":\"Queued for swarmrt-wrap's next 5s tick.\"}", id);
}

/* ================================================================
 * Section 8: Tool Definitions
 * ================================================================ */

typedef struct {
    const char *name;
    const char *description;
    const char *schema_json; /* JSON Schema for arguments */
    void (*handler)(json_node_t *args, jbuf_t *out);
} mcp_tool_t;

/* Forward declarations */
static void tool_codebase_search(json_node_t *args, jbuf_t *out);
static void tool_codebase_fuzzy(json_node_t *args, jbuf_t *out);
static void tool_codebase_status(json_node_t *args, jbuf_t *out);
static void tool_codebase_reindex(json_node_t *args, jbuf_t *out);
static void tool_memory_store(json_node_t *args, jbuf_t *out);
static void tool_memory_recall(json_node_t *args, jbuf_t *out);
static void tool_memory_list(json_node_t *args, jbuf_t *out);
static void tool_memory_forget(json_node_t *args, jbuf_t *out);
static void tool_session_log(json_node_t *args, jbuf_t *out);
static void tool_session_context(json_node_t *args, jbuf_t *out);
static int shell_arg_safe(const char *s);
static void tool_process_stats(json_node_t *args, jbuf_t *out);
static void tool_autopilot_start(json_node_t *args, jbuf_t *out);
static void tool_autopilot_status(json_node_t *args, jbuf_t *out);
static void tool_autopilot_step(json_node_t *args, jbuf_t *out);
static void tool_autopilot_stop(json_node_t *args, jbuf_t *out);
static void tool_set_project(json_node_t *args, jbuf_t *out);
static void tool_memory_update(json_node_t *args, jbuf_t *out);
static void tool_autopilot_pause(json_node_t *args, jbuf_t *out);
static void tool_codebase_grep(json_node_t *args, jbuf_t *out);
static void tool_git_diff(json_node_t *args, jbuf_t *out);
static void tool_git_log(json_node_t *args, jbuf_t *out);
static void tool_codebase_overview(json_node_t *args, jbuf_t *out);
static void tool_workspace_create(json_node_t *args, jbuf_t *out);
static void tool_workspace_list(json_node_t *args, jbuf_t *out);
static void tool_workspace_archive(json_node_t *args, jbuf_t *out);
static void tool_checkpoint_save(json_node_t *args, jbuf_t *out);
static void tool_checkpoint_restore(json_node_t *args, jbuf_t *out);
static void tool_workspace_diff(json_node_t *args, jbuf_t *out);
static void tool_wake_create(json_node_t *args, jbuf_t *out);
static void tool_wake_list(json_node_t *args, jbuf_t *out);
static void tool_wake_delete(json_node_t *args, jbuf_t *out);
static void tool_wake_enable(json_node_t *args, jbuf_t *out);
static void tool_wake_fire_now(json_node_t *args, jbuf_t *out);

static mcp_tool_t TOOLS[] = {
    {
        "codebase_search",
        "BM25 keyword search over the indexed codebase. Returns ranked file matches with relevance scores and content snippets. Use for finding specific code, functions, classes, or patterns.",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query (keywords, function names, code patterns)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max results (default 10)\"}},\"required\":[\"query\"]}",
        tool_codebase_search
    },
    {
        "codebase_fuzzy",
        "Fuzzy/typo-tolerant search over the codebase using trigram matching. Use when you're unsure of exact spelling or want approximate matches.",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Fuzzy search query (typos OK)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max results (default 10)\"}},\"required\":[\"query\"]}",
        tool_codebase_fuzzy
    },
    {
        "codebase_status",
        "Get codebase index status: file count, token count, memory usage, index age.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_codebase_status
    },
    {
        "codebase_reindex",
        "Force a full re-index of the codebase. Use after major file changes.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_codebase_reindex
    },
    {
        "codebase_grep",
        "Regex search over indexed files. Returns matching lines with file:line locations. Use for exact pattern matching (function defs, imports, error strings).",
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"POSIX extended regex pattern\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max results (default 20)\"}},\"required\":[\"pattern\"]}",
        tool_codebase_grep
    },
    {
        "memory_store",
        "Store a persistent memory that survives across sessions. Use for project facts, decisions, patterns, user preferences, error solutions.",
        "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Unique key for this memory\"},\"value\":{\"type\":\"string\",\"description\":\"The content to remember\"},\"tags\":{\"type\":\"string\",\"description\":\"Comma-separated tags for categorization\"}},\"required\":[\"key\",\"value\"]}",
        tool_memory_store
    },
    {
        "memory_recall",
        "Search persistent memories by query. Returns relevant stored memories ranked by BM25 relevance.",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query to find relevant memories\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max results (default 5)\"}},\"required\":[\"query\"]}",
        tool_memory_recall
    },
    {
        "memory_list",
        "List all stored memories, optionally filtered by tag.",
        "{\"type\":\"object\",\"properties\":{\"tag\":{\"type\":\"string\",\"description\":\"Filter by tag (optional)\"}}}",
        tool_memory_list
    },
    {
        "memory_forget",
        "Delete a stored memory by key.",
        "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Key of memory to delete\"}},\"required\":[\"key\"]}",
        tool_memory_forget
    },
    {
        "memory_update",
        "Append to an existing memory's value. Creates the memory if it doesn't exist. Use for accumulating notes, logs, or evolving context under a single key.",
        "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Key of memory to update\"},\"append\":{\"type\":\"string\",\"description\":\"Text to append to existing value\"},\"tags\":{\"type\":\"string\",\"description\":\"Comma-separated tags (merged with existing)\"}},\"required\":[\"key\",\"append\"]}",
        tool_memory_update
    },
    {
        "session_log",
        "Log an event to the session context. Builds a searchable history of what happened this session.",
        "{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"description\":\"Event type (note, decision, error, discovery)\"},\"content\":{\"type\":\"string\",\"description\":\"Event content\"}},\"required\":[\"type\",\"content\"]}",
        tool_session_log
    },
    {
        "session_context",
        "Search session history for relevant context. Returns events matching the query, most relevant first.",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"What context are you looking for?\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max results (default 10)\"}},\"required\":[\"query\"]}",
        tool_session_context
    },
    {
        "process_stats",
        "Get SwarmRT runtime statistics: uptime, memory, index sizes, session events, stored memories.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_process_stats
    },
    {
        "autopilot_start",
        "Start autonomous mode. Give a goal and a list of steps. Claude will keep running until all steps are complete. The external hook re-feeds Claude when it tries to stop.",
        "{\"type\":\"object\",\"properties\":{\"goal\":{\"type\":\"string\",\"description\":\"The high-level goal to accomplish\"},\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Ordered list of steps to complete\"}},\"required\":[\"goal\",\"steps\"]}",
        tool_autopilot_start
    },
    {
        "autopilot_status",
        "Check autopilot status: active goal, current step, completed steps, iteration count. Call this at the start of each turn to know what to do next.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_autopilot_status
    },
    {
        "autopilot_step",
        "Mark the current autopilot step as complete and advance to the next one. Include a summary of what was done.",
        "{\"type\":\"object\",\"properties\":{\"summary\":{\"type\":\"string\",\"description\":\"Brief summary of what was accomplished in this step\"}},\"required\":[\"summary\"]}",
        tool_autopilot_step
    },
    {
        "autopilot_stop",
        "Stop autonomous mode. Use when the goal is fully achieved or when you need to abort.",
        "{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\",\"description\":\"Why autopilot is stopping (completed, aborted, blocked)\"}},\"required\":[\"reason\"]}",
        tool_autopilot_stop
    },
    {
        "autopilot_pause",
        "Toggle autopilot pause/resume. When paused, the hook won't re-feed Claude. Call again to resume.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_autopilot_pause
    },
    {
        "set_project",
        "Switch the MCP to a different project directory. Resets search index, reloads memories from the new project's .swarmrt/ dir. Call this first when working in a specific repo.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path to project root (e.g. /Users/sky/myrepo)\"}},\"required\":[\"path\"]}",
        tool_set_project
    },
    {
        "git_diff",
        "Show git changes in the project. Returns changed files with stats (insertions/deletions). Optionally diff against a specific ref (branch, commit, HEAD~N).",
        "{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\",\"description\":\"Git ref to diff against (default: staged + unstaged changes). Examples: HEAD~3, main, abc1234\"}},\"required\":[]}",
        tool_git_diff
    },
    {
        "git_log",
        "Show recent git commits with hash, author, date, and message. Essential context for understanding what changed recently.",
        "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\",\"description\":\"Number of commits (default 10)\"},\"path\":{\"type\":\"string\",\"description\":\"Filter to specific file/dir path\"}},\"required\":[]}",
        tool_git_log
    },
    {
        "codebase_overview",
        "Get a structural overview of the codebase: languages detected, file counts by extension, directory structure (top 2 levels), total lines of code. Fast architectural context.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_codebase_overview
    },
    /* --- Conductor-inspired workspace tools --- */
    {
        "workspace_create",
        "Create an isolated workspace (git worktree) for parallel development. Each workspace gets its own branch and directory. Auto-assigns a city name if none provided. Use for running multiple agents on different tasks simultaneously.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Workspace name (auto-assigned city name if omitted)\"},\"task\":{\"type\":\"string\",\"description\":\"Description of what this workspace is for\"}},\"required\":[]}",
        tool_workspace_create
    },
    {
        "workspace_list",
        "List all active workspaces with their branches, paths, and change status (files changed, insertions, deletions).",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_workspace_list
    },
    {
        "workspace_archive",
        "Archive (delete) a workspace. Removes the git worktree and branch.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Name of workspace to archive\"}},\"required\":[\"name\"]}",
        tool_workspace_archive
    },
    {
        "checkpoint_save",
        "Save a checkpoint (snapshot) of a workspace's current state. Commits all changes and creates a restorable reference. Like a save point you can revert to.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Workspace name\"},\"label\":{\"type\":\"string\",\"description\":\"Label for this checkpoint (default: auto)\"}},\"required\":[\"name\"]}",
        tool_checkpoint_save
    },
    {
        "checkpoint_restore",
        "Restore a workspace to a previous checkpoint. Without a ref, lists available checkpoints. With a ref (commit hash), hard-resets to that state.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Workspace name\"},\"ref\":{\"type\":\"string\",\"description\":\"Commit hash to restore to (omit to list available checkpoints)\"}},\"required\":[\"name\"]}",
        tool_checkpoint_restore
    },
    {
        "workspace_diff",
        "Show all changes in a workspace compared to the base branch (main). Returns file stats, commit count, and diff content. Use before creating a PR to review changes.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Workspace name\"}},\"required\":[\"name\"]}",
        tool_workspace_diff
    },
    /* --- Wake engine: time-triggered prompt injection --- */
    {
        "wake_create",
        "Schedule a prompt to be injected into the current Claude Code session on a cron schedule. Persists across session restarts and --resume (stored in .swarmrt/wakes.json at project root). Requires Claude Code to be running under 'swarmrt-wrap claude' for prompts to actually fire. Cron is 5-field local-timezone: minute hour dom month dow. Examples: '*/15 * * * *' (every 15 min), '0 */3 * * *' (every 3 hours), '0 9,13,17 * * 1-5' (9am/1pm/5pm weekdays), '0 */2 9-17 * 1-5' (every 2h during work hours weekdays). Minimum granularity is 1 minute. No feature gates, no 1-hour floor, no cloud required.",
        "{\"type\":\"object\",\"properties\":{\"cron_expression\":{\"type\":\"string\",\"description\":\"5-field cron expression in local time (e.g. '0 */3 * * *')\"},\"prompt\":{\"type\":\"string\",\"description\":\"The text to inject as if the user typed it. Can be a slash command ('/babysit-prs') or plain instructions ('check deploy status and summarize').\"},\"name\":{\"type\":\"string\",\"description\":\"Optional short name for the wake (for easy reference in wake_delete / wake_enable)\"}},\"required\":[\"cron_expression\",\"prompt\"]}",
        tool_wake_create
    },
    {
        "wake_list",
        "List all scheduled wakes in this project. Returns id, name, cron, prompt, enabled, next_fire_at (unix ts), last_fired_at, and fire_count for each.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_wake_list
    },
    {
        "wake_delete",
        "Delete a scheduled wake by id or name.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Wake id\"},\"name\":{\"type\":\"string\",\"description\":\"Wake name\"}}}",
        tool_wake_delete
    },
    {
        "wake_enable",
        "Enable or disable a wake without deleting it. Disabled wakes keep their state but are skipped by the PTY daemon.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"name\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"enabled\"]}",
        tool_wake_enable
    },
    {
        "wake_fire_now",
        "Manually queue a wake to fire on swarmrt-wrap's next 5-second tick, bypassing its cron schedule. Appends to .swarmrt/wake_queue.jsonl. Use for testing or for on-demand triggering of a prepared prompt.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"name\":{\"type\":\"string\"}}}",
        tool_wake_fire_now
    },
    {NULL, NULL, NULL, NULL}
};

/* ================================================================
 * Section 9: Tool Implementations
 * ================================================================ */

static void tool_codebase_search(json_node_t *args, jbuf_t *out) {
    const char *query = json_get_str(args, "query");
    int limit = (int)json_get_int(args, "limit", 10);
    if (!query) { jb_append(out, "{\"error\":\"query is required\"}"); return; }
    if (limit > MAX_RESULTS) limit = MAX_RESULTS;
    ensure_search_ready();
    if (!g_code_index) { jb_append(out, "{\"error\":\"index not initialized\"}"); return; }

    session_add("search", query);

    sws_result_t results[MAX_RESULTS];
    int n = sws_bm25_search(g_code_index, query, results, limit);
    rerank_with_path_boost(results, n, query);

    jb_append(out, "[");
    char pathbuf[4096], snippet[512];
    for (int i = 0; i < n; i++) {
        extract_path(results[i].text, pathbuf, sizeof(pathbuf));
        int line = extract_snippet(results[i].text, query, snippet, sizeof(snippet));
        if (i > 0) jb_append(out, ",");
        jb_append(out, "{\"file\":");
        jb_append_escaped(out, pathbuf);
        jb_append(out, ",\"line\":%d,\"score\":%.4f,\"snippet\":", line, results[i].score);
        jb_append_escaped(out, snippet);
        jb_append(out, "}");
    }
    jb_append(out, "]");
}

static void tool_codebase_fuzzy(json_node_t *args, jbuf_t *out) {
    const char *query = json_get_str(args, "query");
    int limit = (int)json_get_int(args, "limit", 10);
    if (!query) { jb_append(out, "{\"error\":\"query is required\"}"); return; }
    if (limit > MAX_RESULTS) limit = MAX_RESULTS;
    ensure_search_ready();
    if (!g_code_index) { jb_append(out, "{\"error\":\"index not initialized\"}"); return; }

    session_add("fuzzy_search", query);

    sws_result_t results[MAX_RESULTS];
    int n = sws_fuzzy_search(g_code_index, query, results, limit);
    rerank_with_path_boost(results, n, query);

    jb_append(out, "[");
    char pathbuf[4096], snippet[512];
    for (int i = 0; i < n; i++) {
        extract_path(results[i].text, pathbuf, sizeof(pathbuf));
        int line = extract_snippet(results[i].text, query, snippet, sizeof(snippet));
        if (i > 0) jb_append(out, ",");
        jb_append(out, "{\"file\":");
        jb_append_escaped(out, pathbuf);
        jb_append(out, ",\"line\":%d,\"score\":%.4f,\"snippet\":", line, results[i].score);
        jb_append_escaped(out, snippet);
        jb_append(out, "}");
    }
    jb_append(out, "]");
}

static void tool_codebase_status(json_node_t *args, jbuf_t *out) {
    (void)args;
    ensure_search_ready();
    if (!g_code_index) { jb_append(out, "\"Index not initialized\""); return; }
    sws_info_t info;
    sws_info(g_code_index, &info);
    jb_append(out, "{\"files\":%u,\"tokens\":%u,\"trigrams\":%u,"
              "\"memory_bytes\":%zu,\"index_time_ms\":%.1f,\"project\":",
              info.doc_count, info.token_count, info.trigram_count,
              info.memory_bytes, g_index_time_ms);
    jb_append_escaped(out, g_project_root);
    jb_append(out, "}");
}

static void tool_codebase_reindex(json_node_t *args, jbuf_t *out) {
    (void)args;
    if (g_code_index) sws_free(g_code_index);
    g_files_indexed = 0;
    g_search_ready = true;
    g_code_index = sws_new(0);

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    index_directory(g_code_index, g_project_root);
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    g_index_time_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                      (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;

    /* Persist */
    char idx_path[4096];
    snprintf(idx_path, sizeof(idx_path), "%s/%s", g_data_dir, INDEX_FILE);
    sws_save(g_code_index, idx_path);

    jb_append(out, "{\"files\":%u,\"time_ms\":%.1f}", g_files_indexed, g_index_time_ms);
}

static void tool_memory_store(json_node_t *args, jbuf_t *out) {
    const char *key = json_get_str(args, "key");
    const char *value = json_get_str(args, "value");
    const char *tags = json_get_str(args, "tags");
    if (!key || !value) { jb_append(out, "{\"error\":\"key and value required\"}"); return; }

    pthread_mutex_lock(&g_memory_lock);

    /* Check if key exists, update if so */
    int idx = -1;
    for (int i = 0; i < g_memory_count; i++) {
        if (strcmp(g_memories[i].key, key) == 0) { idx = i; break; }
    }

    if (idx >= 0) {
        /* Update existing */
        free(g_memories[idx].value);
        free(g_memories[idx].tags);
        g_memories[idx].value = strdup(value);
        g_memories[idx].tags = tags ? strdup(tags) : strdup("");
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        g_memories[idx].timestamp = ts.tv_sec;

        /* Update search index */
        uint64_t id = fnv1a_64(key, strlen(key));
        sws_remove(g_memory_index, id);
        char doc[65536];
        snprintf(doc, sizeof(doc), "%s\n%s\n%s", key, value, g_memories[idx].tags);
        sws_add(g_memory_index, id, doc, NULL, 0);
    } else if (g_memory_count < MAX_MEMORIES) {
        /* New entry */
        memory_entry_t *m = &g_memories[g_memory_count];
        m->key = strdup(key);
        m->value = strdup(value);
        m->tags = tags ? strdup(tags) : strdup("");
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        m->timestamp = ts.tv_sec;

        char doc[65536];
        snprintf(doc, sizeof(doc), "%s\n%s\n%s", key, value, m->tags);
        sws_add(g_memory_index, fnv1a_64(key, strlen(key)), doc, NULL, 0);
        g_memory_count++;
    }

    memory_save();
    pthread_mutex_unlock(&g_memory_lock);

    jb_append(out, "{\"stored\":");
    jb_append_escaped(out, key);
    jb_append(out, ",\"total\":%d}", g_memory_count);
}

static void tool_memory_recall(json_node_t *args, jbuf_t *out) {
    const char *query = json_get_str(args, "query");
    int limit = (int)json_get_int(args, "limit", 5);
    if (!query) { jb_append(out, "{\"error\":\"query required\"}"); return; }
    if (limit > 20) limit = 20;

    sws_result_t results[20];
    int n = sws_bm25_search(g_memory_index, query, results, limit);

    jb_append(out, "[");
    for (int i = 0; i < n; i++) {
        /* Find the memory entry by matching the key (first line of doc text) */
        char keybuf[4096];
        extract_path(results[i].text, keybuf, sizeof(keybuf));

        pthread_mutex_lock(&g_memory_lock);
        memory_entry_t *found = NULL;
        for (int j = 0; j < g_memory_count; j++) {
            if (strcmp(g_memories[j].key, keybuf) == 0) { found = &g_memories[j]; break; }
        }
        pthread_mutex_unlock(&g_memory_lock);

        if (i > 0) jb_append(out, ",");
        if (found) {
            jb_append(out, "{\"key\":");
            jb_append_escaped(out, found->key);
            jb_append(out, ",\"value\":");
            jb_append_escaped(out, found->value);
            jb_append(out, ",\"tags\":");
            jb_append_escaped(out, found->tags);
            jb_append(out, ",\"score\":%.4f}", results[i].score);
        } else {
            jb_append(out, "{\"key\":");
            jb_append_escaped(out, keybuf);
            jb_append(out, ",\"score\":%.4f}", results[i].score);
        }
    }
    jb_append(out, "]");
}

static void tool_memory_list(json_node_t *args, jbuf_t *out) {
    const char *tag_filter = json_get_str(args, "tag");

    pthread_mutex_lock(&g_memory_lock);
    jb_append(out, "[");
    int printed = 0;
    for (int i = 0; i < g_memory_count; i++) {
        if (tag_filter && tag_filter[0] && !strstr(g_memories[i].tags, tag_filter))
            continue;
        if (printed > 0) jb_append(out, ",");
        jb_append(out, "{\"key\":");
        jb_append_escaped(out, g_memories[i].key);
        jb_append(out, ",\"value\":");
        jb_append_escaped(out, g_memories[i].value);
        jb_append(out, ",\"tags\":");
        jb_append_escaped(out, g_memories[i].tags);
        jb_append(out, "}");
        printed++;
    }
    pthread_mutex_unlock(&g_memory_lock);
    jb_append(out, "]");
}

static void tool_memory_forget(json_node_t *args, jbuf_t *out) {
    const char *key = json_get_str(args, "key");
    if (!key) { jb_append(out, "{\"error\":\"key required\"}"); return; }

    pthread_mutex_lock(&g_memory_lock);
    int found = -1;
    for (int i = 0; i < g_memory_count; i++) {
        if (strcmp(g_memories[i].key, key) == 0) { found = i; break; }
    }
    if (found >= 0) {
        sws_remove(g_memory_index, fnv1a_64(key, strlen(key)));
        free(g_memories[found].key);
        free(g_memories[found].value);
        free(g_memories[found].tags);
        /* Shift remaining */
        for (int i = found; i < g_memory_count - 1; i++)
            g_memories[i] = g_memories[i + 1];
        g_memory_count--;
        memory_save();
        jb_append(out, "{\"deleted\":true}");
    } else {
        jb_append(out, "{\"deleted\":false,\"error\":\"key not found\"}");
    }
    pthread_mutex_unlock(&g_memory_lock);
}

static void tool_memory_update(json_node_t *args, jbuf_t *out) {
    const char *key = json_get_str(args, "key");
    const char *append = json_get_str(args, "append");
    const char *tags = json_get_str(args, "tags");
    if (!key || !append) { jb_append(out, "{\"error\":\"key and append required\"}"); return; }

    pthread_mutex_lock(&g_memory_lock);

    /* Find existing */
    int idx = -1;
    for (int i = 0; i < g_memory_count; i++) {
        if (strcmp(g_memories[i].key, key) == 0) { idx = i; break; }
    }

    if (idx >= 0) {
        /* Append to existing value */
        size_t old_len = strlen(g_memories[idx].value);
        size_t add_len = strlen(append);
        char *new_val = malloc(old_len + 1 + add_len + 1); /* old + \n + append + \0 */
        memcpy(new_val, g_memories[idx].value, old_len);
        new_val[old_len] = '\n';
        memcpy(new_val + old_len + 1, append, add_len);
        new_val[old_len + 1 + add_len] = '\0';
        free(g_memories[idx].value);
        g_memories[idx].value = new_val;

        /* Merge tags if provided */
        if (tags && tags[0]) {
            size_t tl = strlen(g_memories[idx].tags);
            size_t nl = strlen(tags);
            char *new_tags = malloc(tl + 1 + nl + 1);
            memcpy(new_tags, g_memories[idx].tags, tl);
            new_tags[tl] = ',';
            memcpy(new_tags + tl + 1, tags, nl);
            new_tags[tl + 1 + nl] = '\0';
            free(g_memories[idx].tags);
            g_memories[idx].tags = new_tags;
        }

        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        g_memories[idx].timestamp = ts.tv_sec;

        /* Update search index */
        uint64_t id = fnv1a_64(key, strlen(key));
        sws_remove(g_memory_index, id);
        char doc[65536];
        snprintf(doc, sizeof(doc), "%s\n%s\n%s", key, g_memories[idx].value, g_memories[idx].tags);
        sws_add(g_memory_index, id, doc, NULL, 0);

        memory_save();
        pthread_mutex_unlock(&g_memory_lock);

        jb_append(out, "{\"updated\":");
        jb_append_escaped(out, key);
        jb_append(out, ",\"action\":\"appended\",\"total_length\":%zu}", old_len + 1 + add_len);
    } else if (g_memory_count < MAX_MEMORIES) {
        /* Create new */
        memory_entry_t *m = &g_memories[g_memory_count];
        m->key = strdup(key);
        m->value = strdup(append);
        m->tags = tags ? strdup(tags) : strdup("");
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        m->timestamp = ts.tv_sec;

        char doc[65536];
        snprintf(doc, sizeof(doc), "%s\n%s\n%s", key, append, m->tags);
        sws_add(g_memory_index, fnv1a_64(key, strlen(key)), doc, NULL, 0);
        g_memory_count++;

        memory_save();
        pthread_mutex_unlock(&g_memory_lock);

        jb_append(out, "{\"updated\":");
        jb_append_escaped(out, key);
        jb_append(out, ",\"action\":\"created\",\"total_length\":%zu}", strlen(append));
    } else {
        pthread_mutex_unlock(&g_memory_lock);
        jb_append(out, "{\"error\":\"memory limit reached\"}");
    }
}

static void tool_session_log(json_node_t *args, jbuf_t *out) {
    const char *type = json_get_str(args, "type");
    const char *content = json_get_str(args, "content");
    if (!type || !content) { jb_append(out, "{\"error\":\"type and content required\"}"); return; }
    session_add(type, content);
    jb_append(out, "{\"logged\":true,\"events\":%d}", g_session_count);
}

static void tool_session_context(json_node_t *args, jbuf_t *out) {
    const char *query = json_get_str(args, "query");
    int limit = (int)json_get_int(args, "limit", 10);
    if (!query) { jb_append(out, "{\"error\":\"query required\"}"); return; }
    if (limit > MAX_RESULTS) limit = MAX_RESULTS;

    sws_result_t results[MAX_RESULTS];
    int n = sws_bm25_search(g_session_index, query, results, limit);

    jb_append(out, "[");
    for (int i = 0; i < n; i++) {
        int idx = (int)results[i].id;
        if (idx < 0 || idx >= g_session_count) continue;
        if (i > 0) jb_append(out, ",");
        jb_append(out, "{\"type\":");
        jb_append_escaped(out, g_session[idx].type);
        jb_append(out, ",\"content\":");
        jb_append_escaped(out, g_session[idx].content);
        jb_append(out, ",\"score\":%.4f}", results[i].score);
    }
    jb_append(out, "]");
}

static void tool_process_stats(json_node_t *args, jbuf_t *out) {
    (void)args;
    ensure_search_ready();
    sws_info_t code_info = {0}, mem_info = {0};
    if (g_code_index) sws_info(g_code_index, &code_info);
    if (g_memory_index) sws_info(g_memory_index, &mem_info);

    jb_append(out, "{\"version\":\"%s\",\"codebase\":{\"files\":%u,\"tokens\":%u,"
              "\"memory_bytes\":%zu},\"memories\":%d,\"session_events\":%d,"
              "\"project\":",
              MCP_VERSION, code_info.doc_count, code_info.token_count,
              code_info.memory_bytes, g_memory_count, g_session_count);
    jb_append_escaped(out, g_project_root);
    jb_append(out, "}");
}

/* --- Autopilot tool implementations --- */

static void tool_autopilot_start(json_node_t *args, jbuf_t *out) {
    const char *goal = json_get_str(args, "goal");
    json_node_t *steps = json_get(args, "steps");

    if (!goal) { jb_append(out, "{\"error\":\"goal is required\"}"); return; }
    if (!steps || steps->type != JSON_ARRAY || steps->children.count == 0) {
        jb_append(out, "{\"error\":\"steps array is required and must not be empty\"}");
        return;
    }

    pthread_mutex_lock(&g_autopilot_lock);

    /* Clear any existing state */
    autopilot_clear();

    g_autopilot.active = true;
    g_autopilot.goal = strdup(goal);
    g_autopilot.current_step = 0;
    g_autopilot.iterations = 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    g_autopilot.started_at = ts.tv_sec;

    for (int i = 0; i < steps->children.count && i < MAX_AUTOPILOT_STEPS; i++) {
        json_node_t *s = steps->children.items[i];
        const char *text = (s->type == JSON_STRING) ? s->sval : "untitled step";
        g_autopilot.steps[g_autopilot.step_count].text = strdup(text);
        g_autopilot.steps[g_autopilot.step_count].done = false;
        g_autopilot.step_count++;
    }

    autopilot_save();
    pthread_mutex_unlock(&g_autopilot_lock);

    session_add("autopilot", goal);
    mcp_log("autopilot started: \"%s\" (%d steps)", goal, g_autopilot.step_count);

    jb_append(out, "{\"active\":true,\"goal\":");
    jb_append_escaped(out, goal);
    jb_append(out, ",\"steps\":%d,\"message\":\"Autopilot engaged. Complete each step and call autopilot_step when done.\"}", g_autopilot.step_count);
}

static void tool_autopilot_status(json_node_t *args, jbuf_t *out) {
    (void)args;
    pthread_mutex_lock(&g_autopilot_lock);

    if (!g_autopilot.active) {
        pthread_mutex_unlock(&g_autopilot_lock);
        jb_append(out, "{\"active\":false,\"message\":\"No autopilot session. Call autopilot_start to begin.\"}");
        return;
    }

    g_autopilot.iterations++;
    autopilot_save();

    int done_count = 0;
    for (int i = 0; i < g_autopilot.step_count; i++)
        if (g_autopilot.steps[i].done) done_count++;

    /* Elapsed time */
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t elapsed_s = now.tv_sec - g_autopilot.started_at;
    int elapsed_m = (int)(elapsed_s / 60);
    int elapsed_h = elapsed_m / 60;
    elapsed_m %= 60;

    jb_append(out, "{\"active\":true,\"paused\":%s,\"goal\":", g_autopilot.paused ? "true" : "false");
    jb_append_escaped(out, g_autopilot.goal);
    jb_append(out, ",\"iteration\":%d,\"progress\":\"%d/%d steps\",\"elapsed\":\"%dh%02dm\"",
              g_autopilot.iterations, done_count, g_autopilot.step_count, elapsed_h, elapsed_m);

    /* Current step instruction */
    if (g_autopilot.paused) {
        jb_append(out, ",\"current_step\":%d,\"instruction\":\"PAUSED — call autopilot_pause to resume.\"",
                  g_autopilot.current_step + 1);
    } else if (g_autopilot.current_step < g_autopilot.step_count) {
        jb_append(out, ",\"current_step\":%d,\"instruction\":", g_autopilot.current_step + 1);
        jb_append_escaped(out, g_autopilot.steps[g_autopilot.current_step].text);
    } else {
        jb_append(out, ",\"current_step\":null,\"instruction\":\"All steps complete! Call autopilot_stop with reason 'completed'.\"");
    }

    /* Show all steps */
    jb_append(out, ",\"steps\":[");
    for (int i = 0; i < g_autopilot.step_count; i++) {
        if (i > 0) jb_append(out, ",");
        jb_append(out, "{\"step\":%d,\"text\":", i + 1);
        jb_append_escaped(out, g_autopilot.steps[i].text);
        jb_append(out, ",\"done\":%s}", g_autopilot.steps[i].done ? "true" : "false");
    }
    jb_append(out, "]");

    /* Git-aware: show modified + untracked files */
    {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && (git diff --name-only; git diff --name-only --cached; git ls-files --others --exclude-standard) 2>/dev/null | sort -u | head -30",
                 g_project_root);
        FILE *proc = popen(cmd, "r");
        if (proc) {
            jb_append(out, ",\"dirty_files\":[");
            char fbuf[1024];
            int fc = 0;
            while (fgets(fbuf, sizeof(fbuf), proc)) {
                int flen = (int)strlen(fbuf);
                while (flen > 0 && (fbuf[flen-1] == '\n' || fbuf[flen-1] == '\r'))
                    fbuf[--flen] = '\0';
                if (flen == 0) continue;
                if (fc > 0) jb_append(out, ",");
                jb_append_escaped(out, fbuf);
                fc++;
            }
            pclose(proc);
            jb_append(out, "]");
        }
    }

    jb_append(out, "}");

    pthread_mutex_unlock(&g_autopilot_lock);
}

static void tool_autopilot_step(json_node_t *args, jbuf_t *out) {
    const char *summary = json_get_str(args, "summary");
    if (!summary) { jb_append(out, "{\"error\":\"summary required\"}"); return; }

    pthread_mutex_lock(&g_autopilot_lock);

    if (!g_autopilot.active) {
        pthread_mutex_unlock(&g_autopilot_lock);
        jb_append(out, "{\"error\":\"autopilot not active\"}");
        return;
    }

    if (g_autopilot.current_step >= g_autopilot.step_count) {
        pthread_mutex_unlock(&g_autopilot_lock);
        jb_append(out, "{\"error\":\"all steps already complete\",\"action\":\"call autopilot_stop\"}");
        return;
    }

    /* Mark current step done */
    g_autopilot.steps[g_autopilot.current_step].done = true;
    int completed = g_autopilot.current_step + 1;
    g_autopilot.current_step++;

    /* Log it */
    char log_msg[4096];
    snprintf(log_msg, sizeof(log_msg), "Step %d complete: %s", completed, summary);
    session_add("autopilot_step", log_msg);

    bool all_done = (g_autopilot.current_step >= g_autopilot.step_count);
    autopilot_save();

    jb_append(out, "{\"completed_step\":%d,\"summary\":", completed);
    jb_append_escaped(out, summary);

    /* Capture files changed since autopilot started (git awareness) */
    {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "cd '%s' && git diff --name-only HEAD 2>/dev/null | head -30",
                 g_project_root);
        FILE *proc = popen(cmd, "r");
        if (proc) {
            jb_append(out, ",\"files_changed\":[");
            char fbuf[1024];
            int fc = 0;
            while (fgets(fbuf, sizeof(fbuf), proc)) {
                int flen = (int)strlen(fbuf);
                while (flen > 0 && (fbuf[flen-1] == '\n' || fbuf[flen-1] == '\r'))
                    fbuf[--flen] = '\0';
                if (flen == 0) continue;
                if (fc > 0) jb_append(out, ",");
                jb_append_escaped(out, fbuf);
                fc++;
            }
            pclose(proc);
            jb_append(out, "]");
        }
    }

    if (all_done) {
        jb_append(out, ",\"all_done\":true,\"message\":\"All steps complete! Call autopilot_stop with reason 'completed'.\"}");
    } else {
        jb_append(out, ",\"all_done\":false,\"next_step\":%d,\"next_instruction\":",
                  g_autopilot.current_step + 1);
        jb_append_escaped(out, g_autopilot.steps[g_autopilot.current_step].text);
        jb_append(out, "}");
    }

    pthread_mutex_unlock(&g_autopilot_lock);
}

static void tool_autopilot_stop(json_node_t *args, jbuf_t *out) {
    const char *reason = json_get_str(args, "reason");
    if (!reason) reason = "stopped";

    pthread_mutex_lock(&g_autopilot_lock);

    int done_count = 0;
    for (int i = 0; i < g_autopilot.step_count; i++)
        if (g_autopilot.steps[i].done) done_count++;

    char log_msg[4096];
    snprintf(log_msg, sizeof(log_msg), "Autopilot stopped (%s): %d/%d steps, %d iterations",
             reason, done_count, g_autopilot.step_count, g_autopilot.iterations);
    session_add("autopilot_stop", log_msg);
    mcp_log("%s", log_msg);

    jb_append(out, "{\"stopped\":true,\"reason\":");
    jb_append_escaped(out, reason);
    jb_append(out, ",\"completed\":\"%d/%d steps\",\"iterations\":%d}",
              done_count, g_autopilot.step_count, g_autopilot.iterations);

    g_autopilot.active = false;
    autopilot_save();

    pthread_mutex_unlock(&g_autopilot_lock);
}

/* --- codebase_grep tool (regex over actual files) --- */

static void grep_directory(const char *dir, const char *root, regex_t *re,
                           jbuf_t *out, int *count, int limit) {
    if (*count >= limit) return;
    DIR *d = opendir(dir);
    if (!d) return;

    char pathbuf[4096];
    struct dirent *ent;
    while ((ent = readdir(d)) && *count < limit) {
        if (ent->d_name[0] == '.') continue;
        if (is_excluded(ent->d_name)) continue;

        snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (lstat(pathbuf, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            grep_directory(pathbuf, root, re, out, count, limit);
        } else if (S_ISREG(st.st_mode) && (size_t)st.st_size <= INDEX_MAX_FILE_SIZE) {
            FILE *f = fopen(pathbuf, "r");
            if (!f) continue;

            /* Relative path */
            const char *relpath = pathbuf;
            size_t rootlen = strlen(root);
            if (strncmp(pathbuf, root, rootlen) == 0 && pathbuf[rootlen] == '/')
                relpath = pathbuf + rootlen + 1;

            char line[4096];
            int lineno = 0;
            while (fgets(line, sizeof(line), f) && *count < limit) {
                lineno++;
                /* Strip newline */
                size_t ll = strlen(line);
                while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
                    line[--ll] = '\0';

                if (regexec(re, line, 0, NULL, 0) == 0) {
                    if (*count > 0) jb_append(out, ",");
                    jb_append(out, "{\"file\":");
                    jb_append_escaped(out, relpath);
                    jb_append(out, ",\"line\":%d,\"text\":", lineno);
                    /* Truncate long lines */
                    if (ll > 200) line[200] = '\0';
                    jb_append_escaped(out, line);
                    jb_append(out, "}");
                    (*count)++;
                }
            }
            fclose(f);
        }
    }
    closedir(d);
}

static void tool_codebase_grep(json_node_t *args, jbuf_t *out) {
    const char *pattern = json_get_str(args, "pattern");
    int limit = (int)json_get_int(args, "limit", 20);
    if (!pattern) { jb_append(out, "{\"error\":\"pattern required\"}"); return; }
    if (limit > 100) limit = 100;

    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, &re, errbuf, sizeof(errbuf));
        jb_append(out, "{\"error\":");
        jb_append_escaped(out, errbuf);
        jb_append(out, "}");
        return;
    }

    session_add("grep", pattern);

    int count = 0;
    jb_append(out, "[");
    grep_directory(g_project_root, g_project_root, &re, out, &count, limit);
    jb_append(out, "]");

    regfree(&re);
}

/* --- autopilot_pause tool --- */

static void tool_autopilot_pause(json_node_t *args, jbuf_t *out) {
    (void)args;
    pthread_mutex_lock(&g_autopilot_lock);

    if (!g_autopilot.active) {
        pthread_mutex_unlock(&g_autopilot_lock);
        jb_append(out, "{\"error\":\"autopilot not active\"}");
        return;
    }

    g_autopilot.paused = !g_autopilot.paused;
    autopilot_save();

    const char *state = g_autopilot.paused ? "paused" : "resumed";
    session_add("autopilot_pause", state);
    mcp_log("autopilot %s", state);

    pthread_mutex_unlock(&g_autopilot_lock);

    jb_append(out, "{\"paused\":%s,\"message\":\"Autopilot %s.\"}",
              g_autopilot.paused ? "true" : "false", state);
}

/* --- set_project tool --- */

static void tool_set_project(json_node_t *args, jbuf_t *out) {
    const char *path = json_get_str(args, "path");
    if (!path || path[0] != '/') {
        jb_append(out, "{\"error\":\"absolute path required (e.g. /Users/sky/myrepo)\"}");
        return;
    }
    if (!shell_arg_safe(path)) {
        jb_append(out, "{\"error\":\"path contains unsafe characters\"}");
        return;
    }

    /* Verify directory exists */
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        jb_append(out, "{\"error\":\"directory not found\"}");
        return;
    }

    const char *old_root = strdup(g_project_root);

    /* Update project root */
    strncpy(g_project_root, path, sizeof(g_project_root) - 1);
    init_data_dir(path);

    /* Reset search — will lazy-index on next search call */
    if (g_code_index) { sws_free(g_code_index); g_code_index = NULL; }
    g_search_ready = false;
    g_files_indexed = 0;
    g_index_time_ms = 0;

    /* Reload memories from new project dir */
    pthread_mutex_lock(&g_memory_lock);
    for (int i = 0; i < g_memory_count; i++) {
        free(g_memories[i].key);
        free(g_memories[i].value);
        free(g_memories[i].tags);
    }
    g_memory_count = 0;
    if (g_memory_index) { sws_free(g_memory_index); g_memory_index = NULL; }
    g_memory_index = sws_new(0);
    memory_load();
    pthread_mutex_unlock(&g_memory_lock);

    /* Reload autopilot state */
    pthread_mutex_lock(&g_autopilot_lock);
    autopilot_clear();
    autopilot_load();
    pthread_mutex_unlock(&g_autopilot_lock);

    /* Reload wake schedule */
    pthread_mutex_lock(&g_wakes_lock);
    wakes_clear();
    wakes_load();
    pthread_mutex_unlock(&g_wakes_lock);

    session_add("set_project", path);
    mcp_log("switched project: %s -> %s", old_root, path);
    free((void *)old_root);

    jb_append(out, "{\"project\":");
    jb_append_escaped(out, path);
    jb_append(out, ",\"memories\":%d,\"message\":\"Project switched. Search will index on first query.\"}", g_memory_count);
}

/* --- Shell safety --- */

/*
 * Sanitize a string for safe use inside single-quoted shell arguments.
 * Returns 1 if safe, 0 if dangerous characters detected.
 * Allowed: alphanumeric, dot, dash, underscore, slash, colon, at, tilde, space, plus, equals
 * Rejected: quotes, semicolons, pipes, backticks, dollar, ampersand, parens, newlines, etc.
 */
static int shell_arg_safe(const char *s) {
    if (!s || !s[0]) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) continue;
        if (c == '.' || c == '-' || c == '_' || c == '/' || c == ':' ||
            c == '@' || c == '~' || c == ' ' || c == '+' || c == '=' ||
            c == '^') continue;
        return 0; /* Dangerous character */
    }
    /* Reject path traversal */
    if (strstr(s, "..")) return 0;
    return 1;
}

/* --- git tools (shell out to git) --- */

static char *run_cmd(const char *cmd, size_t max_out) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = max_out > 0 ? max_out : 65536;
    char *buf = malloc(cap + 1);
    size_t len = 0;
    size_t nr;
    while ((nr = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += nr;
        if (len >= cap) break;
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

static void tool_git_diff(json_node_t *args, jbuf_t *out) {
    const char *ref = json_get_str(args, "ref");
    char cmd[4096];

    if (ref && ref[0]) {
        if (!shell_arg_safe(ref)) {
            jb_append(out, "{\"error\":\"Invalid ref — contains unsafe characters\"}");
            return;
        }
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git diff --stat '%s' 2>&1 && echo '---DIFF---' && git diff '%s' 2>&1 | head -200",
                 g_project_root, ref, ref);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git diff --stat HEAD 2>&1 && echo '---DIFF---' && git diff HEAD 2>&1 | head -200",
                 g_project_root);
    }

    char *result = run_cmd(cmd, 32768);
    if (!result) { jb_append(out, "{\"error\":\"failed to run git diff\"}"); return; }

    session_add("git_diff", ref ? ref : "HEAD");

    /* Split at ---DIFF--- */
    char *sep = strstr(result, "---DIFF---\n");
    if (sep) {
        *sep = '\0';
        const char *diff_content = sep + 11; /* skip "---DIFF---\n" */

        jb_append(out, "{\"stats\":");
        jb_append_escaped(out, result);
        jb_append(out, ",\"diff\":");
        jb_append_escaped(out, diff_content);
        jb_append(out, "}");
    } else {
        jb_append(out, "{\"output\":");
        jb_append_escaped(out, result);
        jb_append(out, "}");
    }
    free(result);
}

static void tool_git_log(json_node_t *args, jbuf_t *out) {
    int limit = (int)json_get_int(args, "limit", 10);
    const char *path = json_get_str(args, "path");
    if (limit > 50) limit = 50;

    char cmd[4096];
    if (path && path[0]) {
        if (!shell_arg_safe(path)) {
            jb_append(out, "{\"error\":\"Invalid path — contains unsafe characters\"}");
            return;
        }
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git log --oneline --format='%%h|%%an|%%ar|%%s' -%d -- '%s' 2>&1",
                 g_project_root, limit, path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git log --oneline --format='%%h|%%an|%%ar|%%s' -%d 2>&1",
                 g_project_root, limit);
    }

    char *result = run_cmd(cmd, 16384);
    if (!result) { jb_append(out, "{\"error\":\"failed to run git log\"}"); return; }

    session_add("git_log", path ? path : "all");

    /* Parse pipe-delimited lines into JSON */
    jb_append(out, "[");
    char *line = result;
    int count = 0;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strlen(line) < 3) { if (nl) { line = nl + 1; continue; } else break; }

        /* Split by | */
        char *hash = line;
        char *author = strchr(hash, '|');
        if (!author) { if (nl) { line = nl + 1; continue; } else break; }
        *author++ = '\0';
        char *date = strchr(author, '|');
        if (!date) { if (nl) { line = nl + 1; continue; } else break; }
        *date++ = '\0';
        char *msg = strchr(date, '|');
        if (!msg) { if (nl) { line = nl + 1; continue; } else break; }
        *msg++ = '\0';

        if (count > 0) jb_append(out, ",");
        jb_append(out, "{\"hash\":");
        jb_append_escaped(out, hash);
        jb_append(out, ",\"author\":");
        jb_append_escaped(out, author);
        jb_append(out, ",\"date\":");
        jb_append_escaped(out, date);
        jb_append(out, ",\"message\":");
        jb_append_escaped(out, msg);
        jb_append(out, "}");
        count++;

        if (!nl) break;
        line = nl + 1;
    }
    jb_append(out, "]");
    free(result);
}

/* --- codebase_overview: language detection + structure --- */

typedef struct { const char *ext; const char *lang; } ext_lang_t;
static const ext_lang_t LANG_MAP[] = {
    {".c", "C"}, {".h", "C/C++ Header"}, {".cpp", "C++"}, {".cc", "C++"},
    {".rs", "Rust"}, {".go", "Go"}, {".py", "Python"}, {".js", "JavaScript"},
    {".ts", "TypeScript"}, {".tsx", "TypeScript/React"}, {".jsx", "JavaScript/React"},
    {".rb", "Ruby"}, {".java", "Java"}, {".kt", "Kotlin"}, {".swift", "Swift"},
    {".ex", "Elixir"}, {".exs", "Elixir"}, {".erl", "Erlang"},
    {".sh", "Shell"}, {".bash", "Shell"}, {".zsh", "Shell"},
    {".html", "HTML"}, {".css", "CSS"}, {".scss", "SCSS"},
    {".json", "JSON"}, {".yaml", "YAML"}, {".yml", "YAML"}, {".toml", "TOML"},
    {".md", "Markdown"}, {".sql", "SQL"}, {".lua", "Lua"}, {".zig", "Zig"},
    {".S", "Assembly"}, {".asm", "Assembly"}, {".sw", "SwarmLang"},
    {".xml", "XML"}, {".proto", "Protobuf"}, {".r", "R"},
    {".cs", "C#"}, {".fs", "F#"}, {".php", "PHP"}, {".pl", "Perl"},
    {".scala", "Scala"}, {".clj", "Clojure"}, {".hs", "Haskell"},
    {".vim", "Vim Script"}, {".el", "Emacs Lisp"}, {".dart", "Dart"},
    {".m", "Objective-C"}, {".mm", "Objective-C++"},
    {NULL, NULL}
};

typedef struct { char ext[16]; const char *lang; int files; int lines; } lang_stat_t;

static void count_lines_walk(const char *dir, const char *root,
                             lang_stat_t *stats, int *nstats, int max_stats,
                             int *total_files, int *total_lines, int depth) {
    if (depth > 6) return; /* don't go too deep */
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    char pathbuf[4096];
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        if (is_excluded(ent->d_name)) continue;
        snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (lstat(pathbuf, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            count_lines_walk(pathbuf, root, stats, nstats, max_stats,
                             total_files, total_lines, depth + 1);
        } else if (S_ISREG(st.st_mode) && (size_t)st.st_size <= INDEX_MAX_FILE_SIZE) {
            /* Get extension */
            const char *dot = strrchr(ent->d_name, '.');
            if (!dot) continue;

            /* Find language */
            const char *lang = NULL;
            for (int i = 0; LANG_MAP[i].ext; i++) {
                if (strcmp(dot, LANG_MAP[i].ext) == 0) { lang = LANG_MAP[i].lang; break; }
            }
            if (!lang) continue;

            /* Count lines */
            FILE *f = fopen(pathbuf, "r");
            if (!f) continue;
            int lines = 0;
            int ch;
            while ((ch = fgetc(f)) != EOF) {
                if (ch == '\n') lines++;
            }
            fclose(f);

            (*total_files)++;
            (*total_lines) += lines;

            /* Find or create stat entry */
            int si = -1;
            for (int i = 0; i < *nstats; i++) {
                if (strcmp(stats[i].ext, dot) == 0) { si = i; break; }
            }
            if (si < 0 && *nstats < max_stats) {
                si = (*nstats)++;
                strncpy(stats[si].ext, dot, sizeof(stats[si].ext) - 1);
                stats[si].lang = lang;
                stats[si].files = 0;
                stats[si].lines = 0;
            }
            if (si >= 0) {
                stats[si].files++;
                stats[si].lines += lines;
            }
        }
    }
    closedir(d);
}

static void tool_codebase_overview(json_node_t *args, jbuf_t *out) {
    (void)args;

    lang_stat_t stats[64];
    int nstats = 0;
    int total_files = 0, total_lines = 0;

    count_lines_walk(g_project_root, g_project_root, stats, &nstats, 64,
                     &total_files, &total_lines, 0);

    /* Sort by lines descending */
    for (int i = 0; i < nstats - 1; i++) {
        for (int j = i + 1; j < nstats; j++) {
            if (stats[j].lines > stats[i].lines) {
                lang_stat_t tmp = stats[i];
                stats[i] = stats[j];
                stats[j] = tmp;
            }
        }
    }

    /* Get top-level dirs */
    char top_dirs_cmd[4096];
    snprintf(top_dirs_cmd, sizeof(top_dirs_cmd),
             "cd '%s' && find . -maxdepth 2 -type d ! -path '*/.*' ! -path '*/node_modules/*' "
             "! -path '*/build/*' ! -path '*/__pycache__/*' ! -path '*/target/*' 2>/dev/null | head -30",
             g_project_root);
    char *dirs = run_cmd(top_dirs_cmd, 4096);

    /* Check if git repo */
    char git_cmd[4096];
    snprintf(git_cmd, sizeof(git_cmd), "cd '%s' && git rev-parse --short HEAD 2>/dev/null", g_project_root);
    char *git_head = run_cmd(git_cmd, 64);
    if (git_head) {
        size_t gl = strlen(git_head);
        while (gl > 0 && (git_head[gl-1] == '\n' || git_head[gl-1] == '\r')) git_head[--gl] = '\0';
    }

    session_add("overview", g_project_root);

    jb_append(out, "{\"project\":");
    jb_append_escaped(out, g_project_root);
    jb_append(out, ",\"total_files\":%d,\"total_lines\":%d", total_files, total_lines);

    if (git_head && git_head[0]) {
        jb_append(out, ",\"git_head\":");
        jb_append_escaped(out, git_head);
    }

    jb_append(out, ",\"languages\":[");
    for (int i = 0; i < nstats && i < 15; i++) {
        if (i > 0) jb_append(out, ",");
        jb_append(out, "{\"lang\":");
        jb_append_escaped(out, stats[i].lang);
        jb_append(out, ",\"ext\":");
        jb_append_escaped(out, stats[i].ext);
        jb_append(out, ",\"files\":%d,\"lines\":%d}", stats[i].files, stats[i].lines);
    }
    jb_append(out, "]");

    if (dirs && dirs[0]) {
        jb_append(out, ",\"structure\":");
        jb_append_escaped(out, dirs);
    }

    jb_append(out, "}");

    free(dirs);
    free(git_head);
}

/* ================================================================
 * Section 9b: Conductor-Inspired Workspace Tools
 *
 * Git worktree orchestration, checkpoints, scripts, todos, PR workflow.
 * Inspired by Conductor (conductor.build) — YC-backed multi-agent orchestrator.
 * ================================================================ */

/* City names for auto-naming workspaces (Conductor-style) */
static const char *CITY_NAMES[] = {
    "tokyo", "paris", "berlin", "london", "cairo", "oslo", "lima",
    "rome", "seoul", "dubai", "prague", "vienna", "lisbon", "zurich",
    "kyoto", "nairobi", "bogota", "hanoi", "athens", "havana",
    "taipei", "lagos", "mumbai", "jakarta", "santiago", "montreal",
    "reykjavik", "marrakech", "bruges", "tallinn", "tbilisi", "baku",
    NULL
};

#define MAX_WORKSPACES 32

typedef struct {
    char name[64];
    char branch[128];
    char path[4096];
    char task[256];    /* what this workspace is for */
    bool active;
} workspace_t;

static workspace_t g_workspaces[MAX_WORKSPACES];
static int g_workspace_count = 0;
static pthread_mutex_t g_workspace_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Workspace persistence --- */

static void workspaces_save(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/workspaces.json", g_data_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\"workspaces\":[");
    for (int i = 0; i < g_workspace_count; i++) {
        if (i > 0) fprintf(f, ",");
        fprintf(f, "{\"name\":\"%s\",\"branch\":\"%s\",\"path\":\"%s\",\"task\":\"",
                g_workspaces[i].name, g_workspaces[i].branch, g_workspaces[i].path);
        for (const char *c = g_workspaces[i].task; *c; c++) {
            if (*c == '"') fprintf(f, "\\\"");
            else if (*c == '\\') fprintf(f, "\\\\");
            else fputc(*c, f);
        }
        fprintf(f, "\",\"active\":%s}", g_workspaces[i].active ? "true" : "false");
    }
    fprintf(f, "]}");
    fclose(f);
}

static void workspaces_load(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/workspaces.json", g_data_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return; }
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    const char *p = buf;
    json_node_t *root = json_parse(&p);
    if (!root) { free(buf); return; }

    json_node_t *ws = json_get(root, "workspaces");
    if (ws && ws->type == JSON_ARRAY) {
        for (int i = 0; i < ws->children.count && g_workspace_count < MAX_WORKSPACES; i++) {
            json_node_t *w = ws->children.items[i];
            const char *name = json_get_str(w, "name");
            const char *branch = json_get_str(w, "branch");
            const char *wpath = json_get_str(w, "path");
            const char *wtask = json_get_str(w, "task");
            json_node_t *act = json_get(w, "active");
            if (name && branch && wpath) {
                workspace_t *ws_entry = &g_workspaces[g_workspace_count++];
                strncpy(ws_entry->name, name, sizeof(ws_entry->name) - 1);
                strncpy(ws_entry->branch, branch, sizeof(ws_entry->branch) - 1);
                strncpy(ws_entry->path, wpath, sizeof(ws_entry->path) - 1);
                if (wtask) strncpy(ws_entry->task, wtask, sizeof(ws_entry->task) - 1);
                ws_entry->active = act && act->type == JSON_BOOL ? act->bval : true;
            }
        }
    }

    json_free(root);
    free(buf);
}

/* --- Pick a city name not already used --- */
static const char *pick_workspace_name(void) {
    for (int c = 0; CITY_NAMES[c]; c++) {
        bool used = false;
        for (int w = 0; w < g_workspace_count; w++) {
            if (g_workspaces[w].active && strcmp(g_workspaces[w].name, CITY_NAMES[c]) == 0) {
                used = true;
                break;
            }
        }
        if (!used) return CITY_NAMES[c];
    }
    return "workspace";
}

/* --- workspace_create: git worktree add with isolated branch --- */

static void tool_workspace_create(json_node_t *args, jbuf_t *out) {
    const char *name = json_get_str(args, "name");
    const char *task = json_get_str(args, "task");
    char cmd[4096];

    pthread_mutex_lock(&g_workspace_lock);

    /* Auto-name if not provided */
    char auto_name[64];
    if (!name || !name[0]) {
        strncpy(auto_name, pick_workspace_name(), sizeof(auto_name) - 1);
        auto_name[sizeof(auto_name) - 1] = '\0';
        name = auto_name;
    }

    /* Validate name — alphanumeric, hyphens, underscores only */
    if (!shell_arg_safe(name)) {
        pthread_mutex_unlock(&g_workspace_lock);
        jb_append(out, "{\"error\":\"Invalid workspace name — use alphanumeric, hyphens, underscores only\"}");
        return;
    }

    /* Check not duplicate */
    for (int i = 0; i < g_workspace_count; i++) {
        if (g_workspaces[i].active && strcmp(g_workspaces[i].name, name) == 0) {
            pthread_mutex_unlock(&g_workspace_lock);
            jb_append(out, "{\"error\":\"Workspace already exists\"}");  /* name already validated */
            return;
        }
    }

    if (g_workspace_count >= MAX_WORKSPACES) {
        pthread_mutex_unlock(&g_workspace_lock);
        jb_append(out, "{\"error\":\"Maximum workspaces reached\"}");  /* MAX_WORKSPACES limit */
        return;
    }

    /* Create branch name */
    char branch[128];
    snprintf(branch, sizeof(branch), "conductor/%s", name);

    /* Workspace path */
    char ws_path[4096];
    snprintf(ws_path, sizeof(ws_path), "%s/.conductor/%s", g_project_root, name);

    /* Create git worktree */
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git worktree add -b '%s' '%s' HEAD 2>&1",
             g_project_root, branch, ws_path);
    char *result = run_cmd(cmd, 4096);

    if (!result || strstr(result, "fatal") || strstr(result, "error")) {
        pthread_mutex_unlock(&g_workspace_lock);
        jb_append(out, "{\"error\":\"Failed to create worktree: ");
        if (result) {
            /* Inline escape without quotes */
            for (const char *c = result; *c; c++) {
                if (*c == '"') jb_append(out, "\\\"");
                else if (*c == '\n') jb_append(out, " ");
                else jb_append(out, "%c", *c);
            }
        }
        jb_append(out, "\"}");
        free(result);
        return;
    }
    free(result);

    /* Register workspace */
    workspace_t *ws = &g_workspaces[g_workspace_count++];
    strncpy(ws->name, name, sizeof(ws->name) - 1);
    strncpy(ws->branch, branch, sizeof(ws->branch) - 1);
    strncpy(ws->path, ws_path, sizeof(ws->path) - 1);
    if (task) strncpy(ws->task, task, sizeof(ws->task) - 1);
    ws->active = true;

    workspaces_save();
    pthread_mutex_unlock(&g_workspace_lock);

    session_add("workspace_create", name);
    mcp_log("workspace created: %s → %s", name, ws_path);

    jb_append(out, "{\"name\":");
    jb_append_escaped(out, name);
    jb_append(out, ",\"branch\":");
    jb_append_escaped(out, branch);
    jb_append(out, ",\"path\":");
    jb_append_escaped(out, ws_path);
    if (task && task[0]) {
        jb_append(out, ",\"task\":");
        jb_append_escaped(out, task);
    }
    jb_append(out, ",\"message\":\"Workspace ready. Agent can work in this isolated worktree.\"}");
}

/* --- workspace_list: show all active workspaces --- */

static void tool_workspace_list(json_node_t *args, jbuf_t *out) {
    (void)args;
    pthread_mutex_lock(&g_workspace_lock);

    jb_append(out, "{\"workspaces\":[");
    int count = 0;
    for (int i = 0; i < g_workspace_count; i++) {
        if (!g_workspaces[i].active) continue;
        if (count > 0) jb_append(out, ",");

        /* Get git status for this workspace */
        char cmd[4096];
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git diff --stat HEAD 2>/dev/null | tail -1",
                 g_workspaces[i].path);
        char *status = run_cmd(cmd, 512);
        if (status) {
            size_t sl = strlen(status);
            while (sl > 0 && (status[sl-1] == '\n' || status[sl-1] == '\r')) status[--sl] = '\0';
        }

        jb_append(out, "{\"name\":");
        jb_append_escaped(out, g_workspaces[i].name);
        jb_append(out, ",\"branch\":");
        jb_append_escaped(out, g_workspaces[i].branch);
        jb_append(out, ",\"path\":");
        jb_append_escaped(out, g_workspaces[i].path);
        if (g_workspaces[i].task[0]) {
            jb_append(out, ",\"task\":");
            jb_append_escaped(out, g_workspaces[i].task);
        }
        if (status && status[0]) {
            jb_append(out, ",\"changes\":");
            jb_append_escaped(out, status);
        }
        jb_append(out, "}");
        free(status);
        count++;
    }
    jb_append(out, "],\"count\":%d}", count);

    pthread_mutex_unlock(&g_workspace_lock);
}

/* --- workspace_archive: remove worktree and mark inactive --- */

static void tool_workspace_archive(json_node_t *args, jbuf_t *out) {
    const char *name = json_get_str(args, "name");
    if (!name) { jb_append(out, "{\"error\":\"name is required\"}"); return; }
    if (!shell_arg_safe(name)) {
        jb_append(out, "{\"error\":\"Invalid name — contains unsafe characters\"}");
        return;
    }

    pthread_mutex_lock(&g_workspace_lock);

    workspace_t *ws = NULL;
    for (int i = 0; i < g_workspace_count; i++) {
        if (g_workspaces[i].active && strcmp(g_workspaces[i].name, name) == 0) {
            ws = &g_workspaces[i];
            break;
        }
    }

    if (!ws) {
        pthread_mutex_unlock(&g_workspace_lock);
        jb_append(out, "{\"error\":\"Workspace not found\"}");
        return;
    }

    /* Remove worktree */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "cd '%s' && git worktree remove '%s' --force 2>&1",
             g_project_root, ws->path);
    char *result = run_cmd(cmd, 4096);

    /* Delete branch */
    snprintf(cmd, sizeof(cmd), "cd '%s' && git branch -D '%s' 2>&1",
             g_project_root, ws->branch);
    char *br_result = run_cmd(cmd, 1024);

    ws->active = false;
    workspaces_save();
    pthread_mutex_unlock(&g_workspace_lock);

    session_add("workspace_archive", name);
    mcp_log("workspace archived: %s", name);

    jb_append(out, "{\"archived\":");
    jb_append_escaped(out, name);
    jb_append(out, ",\"message\":\"Workspace and branch removed.\"}");

    free(result);
    free(br_result);
}

/* --- checkpoint_save: save workspace state as git ref --- */

static void tool_checkpoint_save(json_node_t *args, jbuf_t *out) {
    const char *name = json_get_str(args, "name");
    const char *label = json_get_str(args, "label");
    if (!name) { jb_append(out, "{\"error\":\"workspace name is required\"}"); return; }
    if (!label) label = "auto";
    if (!shell_arg_safe(name) || !shell_arg_safe(label)) {
        jb_append(out, "{\"error\":\"Invalid name or label — contains unsafe characters\"}");
        return;
    }

    pthread_mutex_lock(&g_workspace_lock);

    workspace_t *ws = NULL;
    for (int i = 0; i < g_workspace_count; i++) {
        if (g_workspaces[i].active && strcmp(g_workspaces[i].name, name) == 0) {
            ws = &g_workspaces[i];
            break;
        }
    }

    if (!ws) {
        pthread_mutex_unlock(&g_workspace_lock);
        jb_append(out, "{\"error\":\"Workspace not found\"}");
        return;
    }

    /* Commit tracked changes in workspace (exclude secrets) */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git add -u && git commit -m 'checkpoint: %s' --allow-empty 2>&1",
             ws->path, label);
    char *commit_result = run_cmd(cmd, 4096);

    /* Get commit hash */
    snprintf(cmd, sizeof(cmd), "cd '%s' && git rev-parse HEAD 2>&1", ws->path);
    char *hash = run_cmd(cmd, 64);
    if (hash) {
        size_t hl = strlen(hash);
        while (hl > 0 && (hash[hl-1] == '\n' || hash[hl-1] == '\r')) hash[--hl] = '\0';
    }

    /* Create ref for this checkpoint */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git update-ref refs/conductor/%s/%lld HEAD 2>&1",
             ws->path, name, (long long)ts.tv_sec);
    run_cmd(cmd, 256);

    pthread_mutex_unlock(&g_workspace_lock);

    session_add("checkpoint_save", name);
    mcp_log("checkpoint saved: %s/%s → %s", name, label, hash ? hash : "?");

    jb_append(out, "{\"workspace\":");
    jb_append_escaped(out, name);
    jb_append(out, ",\"label\":");
    jb_append_escaped(out, label);
    if (hash && hash[0]) {
        jb_append(out, ",\"commit\":");
        jb_append_escaped(out, hash);
    }
    jb_append(out, ",\"message\":\"Checkpoint saved. Use checkpoint_restore to revert.\"}");

    free(commit_result);
    free(hash);
}

/* --- checkpoint_restore: revert workspace to a previous checkpoint --- */

static void tool_checkpoint_restore(json_node_t *args, jbuf_t *out) {
    const char *name = json_get_str(args, "name");
    const char *ref = json_get_str(args, "ref");
    if (!name) { jb_append(out, "{\"error\":\"workspace name is required\"}"); return; }
    if (!shell_arg_safe(name)) {
        jb_append(out, "{\"error\":\"Invalid workspace name\"}");
        return;
    }
    if (ref && ref[0] && !shell_arg_safe(ref)) {
        jb_append(out, "{\"error\":\"Invalid ref — contains unsafe characters\"}");
        return;
    }

    pthread_mutex_lock(&g_workspace_lock);

    workspace_t *ws = NULL;
    for (int i = 0; i < g_workspace_count; i++) {
        if (g_workspaces[i].active && strcmp(g_workspaces[i].name, name) == 0) {
            ws = &g_workspaces[i];
            break;
        }
    }

    if (!ws) {
        pthread_mutex_unlock(&g_workspace_lock);
        jb_append(out, "{\"error\":\"Workspace not found\"}");
        return;
    }

    char cmd[4096];

    if (ref && ref[0]) {
        /* Restore to specific ref */
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git reset --hard '%s' 2>&1", ws->path, ref);
    } else {
        /* List available checkpoints */
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git log --oneline -10 2>&1", ws->path);
        char *log = run_cmd(cmd, 4096);
        pthread_mutex_unlock(&g_workspace_lock);

        jb_append(out, "{\"workspace\":");
        jb_append_escaped(out, name);
        jb_append(out, ",\"checkpoints\":");
        jb_append_escaped(out, log ? log : "(none)");
        jb_append(out, ",\"message\":\"Pass a commit hash as 'ref' to restore.\"}");
        free(log);
        return;
    }

    char *result = run_cmd(cmd, 4096);
    pthread_mutex_unlock(&g_workspace_lock);

    session_add("checkpoint_restore", name);
    mcp_log("checkpoint restored: %s → %s", name, ref);

    jb_append(out, "{\"workspace\":");
    jb_append_escaped(out, name);
    jb_append(out, ",\"restored_to\":");
    jb_append_escaped(out, ref);
    jb_append(out, ",\"result\":");
    jb_append_escaped(out, result ? result : "ok");
    jb_append(out, "}");
    free(result);
}

/* --- workspace_diff: show changes in workspace vs base branch --- */

static void tool_workspace_diff(json_node_t *args, jbuf_t *out) {
    const char *name = json_get_str(args, "name");
    if (!name) { jb_append(out, "{\"error\":\"workspace name is required\"}"); return; }
    if (!shell_arg_safe(name)) {
        jb_append(out, "{\"error\":\"Invalid name — contains unsafe characters\"}");
        return;
    }

    pthread_mutex_lock(&g_workspace_lock);
    workspace_t *ws = NULL;
    for (int i = 0; i < g_workspace_count; i++) {
        if (g_workspaces[i].active && strcmp(g_workspaces[i].name, name) == 0) {
            ws = &g_workspaces[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_workspace_lock);

    if (!ws) {
        jb_append(out, "{\"error\":\"Workspace not found\"}");
        return;
    }

    /* Get default branch */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git symbolic-ref refs/remotes/origin/HEAD 2>/dev/null | sed 's@refs/remotes/origin/@@' || echo main",
             g_project_root);
    char *default_branch = run_cmd(cmd, 128);
    if (default_branch) {
        size_t dl = strlen(default_branch);
        while (dl > 0 && (default_branch[dl-1] == '\n' || default_branch[dl-1] == '\r'))
            default_branch[--dl] = '\0';
    }

    /* Diff stat */
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git diff --stat '%s' 2>&1",
             ws->path, default_branch ? default_branch : "main");
    char *stat_result = run_cmd(cmd, 8192);

    /* Diff content (limited) */
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git diff '%s' 2>&1 | head -300",
             ws->path, default_branch ? default_branch : "main");
    char *diff_result = run_cmd(cmd, 32768);

    /* Commit count */
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git rev-list --count '%s'..HEAD 2>&1",
             ws->path, default_branch ? default_branch : "main");
    char *commit_count = run_cmd(cmd, 32);
    if (commit_count) {
        size_t cl = strlen(commit_count);
        while (cl > 0 && (commit_count[cl-1] == '\n' || commit_count[cl-1] == '\r'))
            commit_count[--cl] = '\0';
    }

    session_add("workspace_diff", name);

    jb_append(out, "{\"workspace\":");
    jb_append_escaped(out, name);
    jb_append(out, ",\"base\":");
    jb_append_escaped(out, default_branch ? default_branch : "main");
    if (commit_count && commit_count[0]) {
        jb_append(out, ",\"commits\":");
        jb_append_escaped(out, commit_count);
    }
    jb_append(out, ",\"stats\":");
    jb_append_escaped(out, stat_result ? stat_result : "(no changes)");
    jb_append(out, ",\"diff\":");
    jb_append_escaped(out, diff_result ? diff_result : "(no changes)");
    jb_append(out, "}");

    free(default_branch);
    free(stat_result);
    free(diff_result);
    free(commit_count);
}


/* ================================================================
 * Section 10: MCP Protocol Handler
 * ================================================================ */

static void send_response(int64_t id, const char *result_json) {
    printf("{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":%s}\n",
           (long long)id, result_json);
    fflush(stdout);
}

static void send_error(int64_t id, int code, const char *message) {
    jbuf_t jb; jb_init(&jb);
    jb_append(&jb, "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"error\":{\"code\":%d,\"message\":",
              (long long)id, code);
    jb_append_escaped(&jb, message);
    jb_append(&jb, "}}");
    printf("%s\n", jb.buf);
    fflush(stdout);
    jb_free(&jb);
}

static void handle_initialize(int64_t id) {
    jbuf_t jb; jb_init(&jb);
    jb_append(&jb, "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{"
              "\"tools\":{}},\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
              MCP_NAME, MCP_VERSION);
    send_response(id, jb.buf);
    jb_free(&jb);
}

static void handle_tools_list(int64_t id) {
    jbuf_t jb; jb_init(&jb);
    jb_append(&jb, "{\"tools\":[");
    for (int i = 0; TOOLS[i].name; i++) {
        if (i > 0) jb_append(&jb, ",");
        jb_append(&jb, "{\"name\":\"%s\",\"description\":", TOOLS[i].name);
        jb_append_escaped(&jb, TOOLS[i].description);
        jb_append(&jb, ",\"inputSchema\":%s}", TOOLS[i].schema_json);
    }
    jb_append(&jb, "]}");
    send_response(id, jb.buf);
    jb_free(&jb);
}

static void handle_tools_call(int64_t id, json_node_t *params) {
    const char *name = json_get_str(params, "name");
    json_node_t *args = json_get(params, "arguments");

    if (!name) {
        send_error(id, -32602, "Missing tool name");
        return;
    }

    /* Find tool */
    mcp_tool_t *tool = NULL;
    for (int i = 0; TOOLS[i].name; i++) {
        if (strcmp(TOOLS[i].name, name) == 0) { tool = &TOOLS[i]; break; }
    }

    if (!tool) {
        send_error(id, -32602, "Unknown tool");
        return;
    }

    /* Execute */
    jbuf_t result; jb_init(&result);
    tool->handler(args, &result);

    /* Wrap in MCP tool result format */
    jbuf_t jb; jb_init(&jb);
    jb_append(&jb, "{\"content\":[{\"type\":\"text\",\"text\":");
    /* The result is already JSON, but MCP expects it as a string inside text */
    jb_append_escaped(&jb, result.buf);
    jb_append(&jb, "}]}");

    send_response(id, jb.buf);
    jb_free(&jb);
    jb_free(&result);
}

static void handle_message(const char *line) {
    const char *p = line;
    json_node_t *msg = json_parse(&p);
    if (!msg) { mcp_log("failed to parse message"); return; }

    const char *method = json_get_str(msg, "method");
    int64_t id = json_get_int(msg, "id", -1);

    if (!method) {
        if (id >= 0) send_error(id, -32600, "Missing method");
        json_free(msg);
        return;
    }

    json_node_t *params = json_get(msg, "params");

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id);
    } else if (strcmp(method, "notifications/initialized") == 0) {
        /* No response needed */
    } else if (strcmp(method, "tools/list") == 0) {
        handle_tools_list(id);
    } else if (strcmp(method, "tools/call") == 0) {
        handle_tools_call(id, params);
    } else if (strcmp(method, "ping") == 0) {
        send_response(id, "{}");
    } else {
        if (id >= 0) send_error(id, -32601, "Method not found");
    }

    json_free(msg);
}

/* ================================================================
 * Section 11: Initialization & Main
 * ================================================================ */

static void ensure_dir(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void init_data_dir(const char *project_root) {
    snprintf(g_data_dir, sizeof(g_data_dir), "%s/%s", project_root, DATA_DIR_NAME);
    ensure_dir(g_data_dir);
}

int main(int argc, char **argv) {
    /* Determine project root: arg1, or CWD */
    char cwd[4096];
    const char *root = NULL;
    if (argc > 1) {
        root = argv[1];
    } else {
        if (getcwd(cwd, sizeof(cwd)))
            root = cwd;
        else
            root = ".";
    }

    mcp_log("starting %s v%s", MCP_NAME, MCP_VERSION);
    mcp_log("project root: %s", root);

    /* Init data dir */
    init_data_dir(root);

    /* Load .gitignore patterns before indexing */
    load_gitignore(root);

    /* Init subsystems */
    init_search(root);
    init_memory();
    init_session();
    autopilot_load();
    workspaces_load();
    wakes_load();

    mcp_log("ready — %u files indexed, %d memories loaded", g_files_indexed, g_memory_count);

    /* Main stdio loop */
    char *line = malloc(MAX_LINE);
    while (fgets(line, MAX_LINE, stdin)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        handle_message(line);
    }

    free(line);

    /* Cleanup */
    if (g_code_index) sws_free(g_code_index);
    if (g_memory_index) sws_free(g_memory_index);
    if (g_session_index) sws_free(g_session_index);

    mcp_log("shutdown");
    return 0;
}
