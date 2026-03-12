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

#include "../src/swarmrt_search.h"

/* ================================================================
 * Section 1: Configuration
 * ================================================================ */

#define MCP_VERSION         "0.2.0"
#define MCP_NAME            "swarmrt-mcp"
#define MAX_LINE            (4 * 1024 * 1024)  /* 4MB max JSON-RPC message */
#define MAX_TOOLS           32
#define MAX_RESULTS         20
#define MAX_MEMORIES        8192
#define MAX_SESSION_EVENTS  4096
#define MAX_AUTOPILOT_STEPS 64
#define MEMORY_FILE         "memories.sws"
#define INDEX_FILE          "index.sws"
#define DATA_DIR_NAME       ".swarmrt"
#define AUTOPILOT_FILE      "autopilot.json"

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

            if (S_ISDIR(st.st_mode)) {
                dir_entry_t *e = calloc(1, sizeof(dir_entry_t));
                e->path = strdup(pathbuf);
                e->next = stack;
                stack = e;
            } else if (S_ISREG(st.st_mode) && (size_t)st.st_size <= INDEX_MAX_FILE_SIZE) {
                FILE *f = fopen(pathbuf, "r");
                if (!f) continue;
                char *buf = malloc(INDEX_PREVIEW_SIZE + 1);
                size_t nr = fread(buf, 1, INDEX_PREVIEW_SIZE, f);
                fclose(f);
                buf[nr] = '\0';

                if (is_binary_data(buf, nr)) { free(buf); continue; }

                /* Make path relative to project root */
                const char *relpath = pathbuf;
                size_t rootlen = strlen(root);
                if (strncmp(pathbuf, root, rootlen) == 0 && pathbuf[rootlen] == '/')
                    relpath = pathbuf + rootlen + 1;

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
    {NULL, NULL, NULL, NULL}
};

/* ================================================================
 * Section 9: Tool Implementations
 * ================================================================ */

static void tool_codebase_search(json_node_t *args, jbuf_t *out) {
    const char *query = json_get_str(args, "query");
    int limit = (int)json_get_int(args, "limit", 10);
    if (!query) { jb_append(out, "\"Error: query is required\""); return; }
    if (limit > MAX_RESULTS) limit = MAX_RESULTS;
    ensure_search_ready();
    if (!g_code_index) { jb_append(out, "\"Error: index not initialized\""); return; }

    session_add("search", query);

    sws_result_t results[MAX_RESULTS];
    int n = sws_bm25_search(g_code_index, query, results, limit);

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
    if (!query) { jb_append(out, "\"Error: query is required\""); return; }
    if (limit > MAX_RESULTS) limit = MAX_RESULTS;
    ensure_search_ready();
    if (!g_code_index) { jb_append(out, "\"Error: index not initialized\""); return; }

    session_add("fuzzy_search", query);

    sws_result_t results[MAX_RESULTS];
    int n = sws_fuzzy_search(g_code_index, query, results, limit);

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
    if (!key || !value) { jb_append(out, "\"Error: key and value required\""); return; }

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
    if (!query) { jb_append(out, "\"Error: query required\""); return; }
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
    if (!key) { jb_append(out, "\"Error: key required\""); return; }

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
    if (!key || !append) { jb_append(out, "\"Error: key and append required\""); return; }

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
        jb_append(out, "\"Error: memory limit reached\"");
    }
}

static void tool_session_log(json_node_t *args, jbuf_t *out) {
    const char *type = json_get_str(args, "type");
    const char *content = json_get_str(args, "content");
    if (!type || !content) { jb_append(out, "\"Error: type and content required\""); return; }
    session_add(type, content);
    jb_append(out, "{\"logged\":true,\"events\":%d}", g_session_count);
}

static void tool_session_context(json_node_t *args, jbuf_t *out) {
    const char *query = json_get_str(args, "query");
    int limit = (int)json_get_int(args, "limit", 10);
    if (!query) { jb_append(out, "\"Error: query required\""); return; }
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

    if (!goal) { jb_append(out, "\"Error: goal is required\""); return; }
    if (!steps || steps->type != JSON_ARRAY || steps->children.count == 0) {
        jb_append(out, "\"Error: steps array is required and must not be empty\"");
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
    jb_append(out, "]}");

    pthread_mutex_unlock(&g_autopilot_lock);
}

static void tool_autopilot_step(json_node_t *args, jbuf_t *out) {
    const char *summary = json_get_str(args, "summary");
    if (!summary) { jb_append(out, "\"Error: summary required\""); return; }

    pthread_mutex_lock(&g_autopilot_lock);

    if (!g_autopilot.active) {
        pthread_mutex_unlock(&g_autopilot_lock);
        jb_append(out, "\"Error: autopilot not active\"");
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
    if (!pattern) { jb_append(out, "\"Error: pattern required\""); return; }
    if (limit > 100) limit = 100;

    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, &re, errbuf, sizeof(errbuf));
        jb_append(out, "{\"error\":\"Invalid regex: ");
        jb_append(out, "%s\"}", errbuf);
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
        jb_append(out, "\"Error: autopilot not active\"");
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
        jb_append(out, "\"Error: absolute path required (e.g. /Users/sky/myrepo)\"");
        return;
    }

    /* Verify directory exists */
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        jb_append(out, "\"Error: directory not found: ");
        jb_append(out, "%s\"", path);
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

    session_add("set_project", path);
    mcp_log("switched project: %s -> %s", old_root, path);
    free((void *)old_root);

    jb_append(out, "{\"project\":");
    jb_append_escaped(out, path);
    jb_append(out, ",\"memories\":%d,\"message\":\"Project switched. Search will index on first query.\"}", g_memory_count);
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
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git diff --stat '%s' 2>&1 && echo '---DIFF---' && git diff '%s' 2>&1 | head -200",
                 g_project_root, ref, ref);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git diff --stat HEAD 2>&1 && echo '---DIFF---' && git diff HEAD 2>&1 | head -200",
                 g_project_root);
    }

    char *result = run_cmd(cmd, 32768);
    if (!result) { jb_append(out, "\"Error: failed to run git diff\""); return; }

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
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git log --oneline --format='%%h|%%an|%%ar|%%s' -%d -- '%s' 2>&1",
                 g_project_root, limit, path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git log --oneline --format='%%h|%%an|%%ar|%%s' -%d 2>&1",
                 g_project_root, limit);
    }

    char *result = run_cmd(cmd, 16384);
    if (!result) { jb_append(out, "\"Error: failed to run git log\""); return; }

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

    /* Init subsystems */
    init_search(root);
    init_memory();
    init_session();
    autopilot_load();

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
