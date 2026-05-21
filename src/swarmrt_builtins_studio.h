/*
 * SwarmRT Phase 11: Studio Builtins
 *
 * Register, ETS, HTTP, JSON, File I/O, and utility builtins for
 * compiled .sw programs. Included by generated C code.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_BUILTINS_STUDIO_H
#define SWARMRT_BUILTINS_STUDIO_H

#include "swarmrt_native.h"
#include "swarmrt_lang.h"
#include "swarmrt_ets.h"
#include "swarmrt_otp.h"
#include "swarmrt_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#ifndef _WIN32
  #include <sys/select.h>
  #include <sys/ioctl.h>
#endif
#include "swarmrt_platform.h"
#include <sqlite3.h>
#ifdef _WIN32
  #include <io.h>
  #include <direct.h>
  #include <sys/time.h>  /* MinGW-w64 provides gettimeofday */
  #define sw_mkdir(p, m) _mkdir(p)
  #define swbs_unlink(p) _unlink(p)
  #define sw_sleep(s) Sleep((s) * 1000)
#else
  #include <unistd.h>
  #include <sys/time.h>
  #include <sys/wait.h>
  #define sw_mkdir(p, m) mkdir(p, m)
  #define swbs_unlink(p) unlink(p)
  #define sw_sleep(s) sleep(s)
#endif

/* === Registry === */

static sw_val_t *_builtin_register(sw_val_t **a, int n) {
    if (n < 2 || !a[0]->v.str || !a[1]->v.pid) return sw_val_atom("error");
    return sw_val_atom(sw_register(a[0]->v.str, a[1]->v.pid) == 0 ? "ok" : "error");
}

static sw_val_t *_builtin_whereis(sw_val_t **a, int n) {
    if (n < 1 || !a[0]->v.str) return sw_val_nil();
    sw_process_t *p = sw_whereis(a[0]->v.str);
    return p ? sw_val_pid(p) : sw_val_nil();
}

static sw_val_t *_builtin_monitor(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_PID) return sw_val_nil();
    return sw_val_int((int64_t)sw_monitor(a[0]->v.pid));
}

static sw_val_t *_builtin_link(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_PID) return sw_val_atom("error");
    sw_link(a[0]->v.pid);
    return sw_val_atom("ok");
}

/* === Value-aware ETS (hashes/compares sw_val_t by value, not pointer) === */

#define _VETS_BUCKETS 256
#define _VETS_MAX_TABLES 64

typedef struct _vets_entry {
    sw_val_t *key;
    sw_val_t *value;
    struct _vets_entry *next;
} _vets_entry_t;

typedef struct {
    _vets_entry_t *buckets[_VETS_BUCKETS];
    pthread_rwlock_t lock;
    int active;
} _vets_table_t;

static _vets_table_t _vets_tables[_VETS_MAX_TABLES];
static int _vets_next_id = 0;
static pthread_mutex_t _vets_meta = PTHREAD_MUTEX_INITIALIZER;

static uint32_t _vets_hash_val(sw_val_t *v) {
    uint64_t h = 14695981039346656037ULL;
    switch (v->type) {
        case SW_VAL_INT: {
            uint64_t k = (uint64_t)v->v.i;
            for (int i = 0; i < 8; i++) { h ^= (k & 0xff); h *= 1099511628211ULL; k >>= 8; }
            break;
        }
        case SW_VAL_STRING: case SW_VAL_ATOM: {
            for (const char *s = v->v.str; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
            break;
        }
        case SW_VAL_TUPLE: {
            for (int i = 0; i < v->v.tuple.count; i++) h ^= _vets_hash_val(v->v.tuple.items[i]) * (i + 1);
            break;
        }
        default: {
            uint64_t k = (uint64_t)(uintptr_t)v;
            h ^= k; h *= 1099511628211ULL;
            break;
        }
    }
    return (uint32_t)(h % _VETS_BUCKETS);
}

static int _vets_key_eq(sw_val_t *a, sw_val_t *b) {
    if (a->type != b->type) return 0;
    switch (a->type) {
        case SW_VAL_INT: return a->v.i == b->v.i;
        case SW_VAL_STRING: case SW_VAL_ATOM: return strcmp(a->v.str, b->v.str) == 0;
        case SW_VAL_TUPLE:
            if (a->v.tuple.count != b->v.tuple.count) return 0;
            for (int i = 0; i < a->v.tuple.count; i++)
                if (!_vets_key_eq(a->v.tuple.items[i], b->v.tuple.items[i])) return 0;
            return 1;
        case SW_VAL_PID: return a->v.pid == b->v.pid;
        default: return a == b;
    }
}

static sw_val_t *_builtin_ets_new(sw_val_t **a, int n) {
    (void)a; (void)n;
    pthread_mutex_lock(&_vets_meta);
    int id = _vets_next_id++;
    if (id >= _VETS_MAX_TABLES) { pthread_mutex_unlock(&_vets_meta); return sw_val_nil(); }
    memset(&_vets_tables[id], 0, sizeof(_vets_table_t));
    pthread_rwlock_init(&_vets_tables[id].lock, NULL);
    _vets_tables[id].active = 1;
    pthread_mutex_unlock(&_vets_meta);
    return sw_val_int((int64_t)id);
}

static sw_val_t *_builtin_ets_put(sw_val_t **a, int n) {
    if (n < 3 || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_atom("error");
    _vets_table_t *t = &_vets_tables[id];
    uint32_t bucket = _vets_hash_val(a[1]);
    /* ETS debug removed */
    pthread_rwlock_wrlock(&t->lock);
    _vets_entry_t *e = t->buckets[bucket];
    while (e) {
        if (_vets_key_eq(e->key, a[1])) { e->value = a[2]; pthread_rwlock_unlock(&t->lock); return sw_val_atom("ok"); }
        e = e->next;
    }
    _vets_entry_t *ne = (_vets_entry_t *)malloc(sizeof(_vets_entry_t));
    ne->key = a[1]; ne->value = a[2]; ne->next = t->buckets[bucket];
    t->buckets[bucket] = ne;
    pthread_rwlock_unlock(&t->lock);
    /* ETS debug removed */
    return sw_val_atom("ok");
}

static sw_val_t *_builtin_ets_get(sw_val_t **a, int n) {
    if (n < 2 || a[0]->type != SW_VAL_INT) return sw_val_nil();
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_nil();
    _vets_table_t *t = &_vets_tables[id];
    uint32_t bucket = _vets_hash_val(a[1]);
    /* ETS debug removed */
    pthread_rwlock_rdlock(&t->lock);
    _vets_entry_t *e = t->buckets[bucket];
    int count = 0;
    while (e) {
        if (_vets_key_eq(e->key, a[1])) {
            sw_val_t *v = e->value;
            pthread_rwlock_unlock(&t->lock);
            return v;
        }
        e = e->next;
        count++;
    }
    /* ETS debug removed */
    pthread_rwlock_unlock(&t->lock);
    return sw_val_nil();
}

static sw_val_t *_builtin_ets_delete(sw_val_t **a, int n) {
    if (n < 2 || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_atom("error");
    _vets_table_t *t = &_vets_tables[id];
    uint32_t bucket = _vets_hash_val(a[1]);
    pthread_rwlock_wrlock(&t->lock);
    _vets_entry_t **pp = &t->buckets[bucket];
    while (*pp) {
        if (_vets_key_eq((*pp)->key, a[1])) {
            _vets_entry_t *dead = *pp; *pp = dead->next; free(dead);
            pthread_rwlock_unlock(&t->lock); return sw_val_atom("ok");
        }
        pp = &(*pp)->next;
    }
    pthread_rwlock_unlock(&t->lock);
    return sw_val_atom("ok");
}

/* === Utilities === */

static sw_val_t *_builtin_sleep(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_INT) return sw_val_atom("ok");
    /* Use sw_receive_any with timeout to yield scheduler to other processes */
    uint64_t tag;
    void *msg = sw_receive_any((uint64_t)a[0]->v.i, &tag);
    if (msg) free(msg); /* discard any spurious message */
    return sw_val_atom("ok");
}

static sw_val_t *_builtin_getenv(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    const char *v = getenv(a[0]->v.str);
    return v ? sw_val_string(v) : sw_val_nil();
}

static sw_val_t *_builtin_typeof(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_string("null");
    switch (a[0]->type) {
        case SW_VAL_NIL: return sw_val_string("nil");
        case SW_VAL_INT: return sw_val_string("int");
        case SW_VAL_FLOAT: return sw_val_string("float");
        case SW_VAL_STRING: return sw_val_string("string");
        case SW_VAL_ATOM: return sw_val_string("atom");
        case SW_VAL_TUPLE: return sw_val_string("tuple");
        case SW_VAL_LIST: return sw_val_string("list");
        case SW_VAL_PID: return sw_val_string("pid");
        case SW_VAL_REMOTE_PID: return sw_val_string("rpid");
        case SW_VAL_MAP: return sw_val_string("map");
        default: return sw_val_string("unknown");
    }
}

static sw_val_t *_builtin_to_string(sw_val_t **a, int n) {
    if (n < 1) return sw_val_string("");
    /* Fast paths for the common scalar cases — avoid the memstream
     * allocation when we just want "42" or "ok". */
    switch (a[0]->type) {
        case SW_VAL_STRING: return a[0];
        case SW_VAL_ATOM:   return sw_val_string(a[0]->v.str);
        case SW_VAL_NIL:    return sw_val_string("nil");
        case SW_VAL_INT: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)a[0]->v.i);
            return sw_val_string(buf);
        }
        case SW_VAL_FLOAT: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%g", a[0]->v.f);
            return sw_val_string(buf);
        }
        default: break;
    }
    /* Composite values (tuples, lists, maps, pids) — render via the
     * shared formatter so to_string() produces the same readable shape
     * as print(): "{ok, 42}", "[1, 2, 3]", "%{a: 1}", "<pid:7>". */
    char *buf = NULL;
    size_t blen = 0;
    FILE *m = open_memstream(&buf, &blen);
    if (!m) return sw_val_string("?");
    sw_val_format(m, a[0]);
    fclose(m);
    sw_val_t *r = sw_val_string(buf ? buf : "");
    free(buf);
    return r;
}

static sw_val_t *_builtin_timestamp(sw_val_t **a, int n) {
    (void)a; (void)n;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return sw_val_int((int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static sw_val_t *_builtin_random_int(sw_val_t **a, int n) {
    if (n < 2 || a[0]->type != SW_VAL_INT || a[1]->type != SW_VAL_INT)
        return sw_val_int(0);
    int64_t lo = a[0]->v.i, hi = a[1]->v.i;
    return sw_val_int(lo + (int64_t)(sw_random_uniform((uint32_t)(hi - lo + 1))));
}

/* === String ops === */

static sw_val_t *_builtin_string_contains(sw_val_t **a, int n) {
    if (n < 2 || a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_STRING)
        return sw_val_atom("false");
    return sw_val_atom(strstr(a[0]->v.str, a[1]->v.str) ? "true" : "false");
}

static sw_val_t *_builtin_string_replace(sw_val_t **a, int n) {
    if (n < 3 || a[0]->type != SW_VAL_STRING ||
        a[1]->type != SW_VAL_STRING || a[2]->type != SW_VAL_STRING)
        return n >= 1 ? a[0] : sw_val_string("");
    const char *src = a[0]->v.str, *old = a[1]->v.str, *rep = a[2]->v.str;
    size_t olen = strlen(old), rlen = strlen(rep), slen = strlen(src);
    if (olen == 0) return a[0];
    /* Heap-allocate to avoid stack overflow */
    size_t cap = slen * 2 + rlen + 1;
    if (cap < 4096) cap = 4096;
    char *buf = (char *)malloc(cap);
    size_t blen = 0;
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
    buf[blen] = 0;
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* string_sub(str, start, len) — substring extraction */
static sw_val_t *_builtin_string_sub(sw_val_t **a, int n) {
    if (n < 3 || a[0]->type != SW_VAL_STRING ||
        a[1]->type != SW_VAL_INT || a[2]->type != SW_VAL_INT)
        return sw_val_string("");
    const char *s = a[0]->v.str;
    int slen = (int)strlen(s);
    int start = (int)a[1]->v.i;
    int len = (int)a[2]->v.i;
    if (start < 0) start = 0;
    if (start >= slen) return sw_val_string("");
    if (start + len > slen) len = slen - start;
    if (len <= 0) return sw_val_string("");
    char *buf = (char *)malloc(len + 1);
    memcpy(buf, s + start, len);
    buf[len] = 0;
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* string_length(str) — returns string length as int */
static sw_val_t *_builtin_string_length(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_STRING) return sw_val_int(0);
    return sw_val_int((int64_t)strlen(a[0]->v.str));
}

static sw_val_t *_builtin_list_append(sw_val_t **a, int n) {
    if (n < 2 || a[0]->type != SW_VAL_LIST) {
        sw_val_t *one = a[1];
        return sw_val_list(&one, 1);
    }
    int cnt = a[0]->v.tuple.count;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * (cnt + 1));
    for (int i = 0; i < cnt; i++) items[i] = a[0]->v.tuple.items[i];
    items[cnt] = a[1];
    sw_val_t *r = sw_val_list(items, cnt + 1);
    free(items);
    return r;
}

/* === File I/O === */

static int _mkdirp(const char *path) {
    size_t len = strlen(path);
    char *tmp = (char *)malloc(len + 1);
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; sw_mkdir(tmp, 0755); *p = '/'; }
    }
    int rc = sw_mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
    free(tmp);
    return rc;
}

static sw_val_t *_builtin_file_mkdir(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_STRING) return sw_val_atom("error");
    return sw_val_atom(_mkdirp(a[0]->v.str) == 0 ? "ok" : "error");
}

static sw_val_t *_builtin_file_write(sw_val_t **a, int n) {
    if (n < 2 || a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_STRING)
        return sw_val_atom("error");
    FILE *fp = fopen(a[0]->v.str, "w");
    if (!fp) return sw_val_atom("error");
    fputs(a[1]->v.str, fp);
    fclose(fp);
    return sw_val_atom("ok");
}

static sw_val_t *_builtin_file_read(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    FILE *fp = fopen(a[0]->v.str, "r");
    if (!fp) return sw_val_nil();
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 1048576) { fclose(fp); return sw_val_nil(); }
    char *buf = (char *)malloc(sz + 1);
    fread(buf, 1, sz, fp);
    buf[sz] = 0;
    fclose(fp);
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* === JSON === */

static sw_val_t *_builtin_json_escape(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_STRING) return sw_val_string("null");
    const char *s = a[0]->v.str;
    size_t slen = strlen(s);
    size_t cap = slen * 2 + 3;
    if (cap < 256) cap = 256;
    char *buf = (char *)malloc(cap);
    int o = 0;
    buf[o++] = '"';
    for (; *s && o < (int)cap - 2; s++) {
        switch (*s) {
            case '"':  buf[o++] = '\\'; buf[o++] = '"'; break;
            case '\\': buf[o++] = '\\'; buf[o++] = '\\'; break;
            case '\n': buf[o++] = '\\'; buf[o++] = 'n'; break;
            case '\r': buf[o++] = '\\'; buf[o++] = 'r'; break;
            case '\t': buf[o++] = '\\'; buf[o++] = 't'; break;
            default:   buf[o++] = *s; break;
        }
    }
    buf[o++] = '"';
    buf[o] = 0;
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* Extract a value from a JSON object by key.
 * Returns string for strings, sw_val for arrays/objects/numbers/bools. */
static sw_val_t *_builtin_json_get(sw_val_t **a, int n) {
    if (n < 2 || a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *json = a[0]->v.str, *key = a[1]->v.str;
    char pat[512];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return sw_val_nil();
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) return sw_val_nil();

    size_t jlen = strlen(json);
    size_t cap = jlen + 1;
    if (cap < 4096) cap = 4096;
    char *buf = (char *)malloc(cap);
    int o = 0;

    if (*p == '"') {
        p++;
        while (*p && *p != '"' && o < (int)cap - 1) {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                    case 'n': buf[o++] = '\n'; break;
                    case 't': buf[o++] = '\t'; break;
                    case '"': buf[o++] = '"'; break;
                    case '\\': buf[o++] = '\\'; break;
                    default: buf[o++] = *p; break;
                }
            } else {
                buf[o++] = *p;
            }
            p++;
        }
        buf[o] = 0;
        sw_val_t *r = sw_val_string(buf);
        free(buf);
        return r;
    } else if (*p == '[' || *p == '{') {
        char open = *p, close = (*p == '[') ? ']' : '}';
        int depth = 1;
        buf[o++] = *p;
        p++;
        while (*p && depth > 0 && o < (int)cap - 1) {
            if (*p == '"') {
                buf[o++] = *p; p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) { buf[o++] = *p; p++; }
                    buf[o++] = *p; p++;
                }
                if (*p) buf[o++] = *p;
                p++;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            if (depth >= 0) buf[o++] = *p;
            p++;
        }
        buf[o] = 0;
        sw_val_t *r = sw_val_string(buf);
        free(buf);
        return r;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && o < 256)
            buf[o++] = *p++;
        buf[o] = 0;
        sw_val_t *r;
        if (strcmp(buf, "null") == 0) r = sw_val_nil();
        else if (strcmp(buf, "true") == 0) r = sw_val_atom("true");
        else if (strcmp(buf, "false") == 0) r = sw_val_atom("false");
        else {
            char *end;
            long long v = strtoll(buf, &end, 10);
            if (*end == 0) r = sw_val_int(v);
            else r = sw_val_string(buf);
        }
        free(buf);
        return r;
    }
}

/* === HTTP POST via curl with retry + soft interrupt =========
 *
 * Forward-declared here; the body lives after _sw_popen_pid and the
 * line-editor's _sw_rl state (which it depends on for interrupt
 * detection). See the implementation below in this same file for the
 * full design notes.
 * ============================================================ */
static sw_val_t *_builtin_http_post(sw_val_t **a, int n);

/* (Implementation moved below — see after _sw_popen_pid_close.) */
#if 0
static sw_val_t *_builtin_http_post_OBSOLETE(sw_val_t **a, int n) {
    if (n < 3 || a[0]->type != SW_VAL_STRING || a[2]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *url = a[0]->v.str, *body = a[2]->v.str;

    /* Build the curl command once — reused across retry attempts. */
    size_t cmdcap = strlen(body) + strlen(url) + 4096;
    char *cmd = (char *)malloc(cmdcap);
    int off = 0;
    off += snprintf(cmd + off, cmdcap - off,
        "curl -sS -X POST --connect-timeout 30 --max-time 300");

    sw_val_t *headers = a[1];
    if (headers && headers->type == SW_VAL_LIST) {
        for (int i = 0; i < headers->v.tuple.count; i++) {
            sw_val_t *h = headers->v.tuple.items[i];
            if (h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->v.str && h->v.tuple.items[1]->v.str)
                off += snprintf(cmd + off, cmdcap - off, " -H '%s: %s'",
                    h->v.tuple.items[0]->v.str, h->v.tuple.items[1]->v.str);
        }
    }

    /* Body via temp file — keeps curl arg list small and avoids
     * shell-escaping the JSON. */
    char tmpf[256];
    snprintf(tmpf, sizeof(tmpf),
        "%s/sw_http_%d_%u.json", sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    FILE *tf = fopen(tmpf, "w");
    if (tf) { fputs(body, tf); fclose(tf); }

    /* No -o flag — let curl write to its pipe to us. We grow the
     * response buffer as bytes arrive. */
    off += snprintf(cmd + off, cmdcap - off,
        " -d @%s '%s' 2>/dev/null", tmpf, url);

    /* Stdin interrupt watcher — only enabled when stdin is a TTY AND
     * the line editor has set up raw mode (so ESC arrives as a single
     * byte without waiting for newline). swarm-code's reader process
     * triggers _sw_rl_setup on first read_line; from that point on
     * the terminal stays in raw mode for the rest of the session. */
    int stdin_fd = -1;
    if (isatty(STDIN_FILENO) && _sw_rl.saved_ok) stdin_fd = STDIN_FILENO;

    int interrupted = 0;
    char *resp_buf = NULL;
    size_t resp_len = 0;
    size_t resp_cap_local = 65536;

    int delays[] = {0, 5, 15};
    for (int attempt = 0; attempt < 3 && !interrupted; attempt++) {
        if (delays[attempt] > 0) sw_sleep(delays[attempt]);

        if (resp_buf) free(resp_buf);
        resp_buf = (char *)malloc(resp_cap_local);
        resp_len = 0;
        resp_buf[0] = 0;

        _sw_popen_pid_t ch = _sw_popen_pid(cmd);
        if (!ch.fp) continue;
        int pipe_fd = fileno(ch.fp);
        int fl = fcntl(pipe_fd, F_GETFL, 0);
        if (fl >= 0) fcntl(pipe_fd, F_SETFL, fl | O_NONBLOCK);

        char readbuf[4096];
        int done = 0;
        while (!done) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(pipe_fd, &rfds);
            if (stdin_fd >= 0) FD_SET(stdin_fd, &rfds);
            int max_fd = pipe_fd;
            if (stdin_fd > max_fd) max_fd = stdin_fd;

            /* 1-second tick so we periodically wake up and could check
             * additional state in the future (e.g. a sw-side interrupt
             * flag). For now select blocks until pipe or stdin fires. */
            struct timeval tv = {1, 0};
            int ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                done = 1;
                break;
            }

            if (stdin_fd >= 0 && FD_ISSET(stdin_fd, &rfds)) {
                unsigned char ib;
                ssize_t nb = read(stdin_fd, &ib, 1);
                if (nb == 1 && (ib == 0x1b || ib == 0x03)) {
                    interrupted = 1;
                    fputs("\n  \x1b[38;5;208m⏸ interrupted by user\x1b[0m\n", stdout);
                    fflush(stdout);
                    _sw_pkill_close(ch);
                    ch.fp = NULL; ch.pid = -1;
                    done = 1;
                    break;
                }
                /* Other keystrokes during the wait: ignore. */
            }

            if (FD_ISSET(pipe_fd, &rfds)) {
                ssize_t rn = read(pipe_fd, readbuf, sizeof(readbuf));
                if (rn < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                    done = 1; break;
                }
                if (rn == 0) { done = 1; break; }   /* EOF */
                if (resp_len + (size_t)rn + 1 > resp_cap_local) {
                    while (resp_len + (size_t)rn + 1 > resp_cap_local) resp_cap_local *= 2;
                    resp_buf = (char *)realloc(resp_buf, resp_cap_local);
                }
                memcpy(resp_buf + resp_len, readbuf, rn);
                resp_len += (size_t)rn;
                resp_buf[resp_len] = 0;
            }
        }

        if (!interrupted && ch.fp) {
            _sw_popen_pid_close(ch);
        }

        if (interrupted) break;

        /* Server error or empty body → retry. Otherwise we're done. */
        if (resp_len > 0 &&
            strstr(resp_buf, "Internal Server Error") == NULL &&
            strstr(resp_buf, "\"error\"") == NULL) {
            break;
        }
    }

    swbs_unlink(tmpf);
    free(cmd);

    if (interrupted) {
        free(resp_buf);
        /* Sentinel string the sw caller checks for. Picked to be
         * impossible to confuse with a real API response. */
        return sw_val_string("__INTERRUPTED__");
    }

    sw_val_t *r = sw_val_string(resp_buf);
    free(resp_buf);
    return r;
}
#endif /* obsolete http_post body wrapped above */

/* ============================================================
 * Line editor state — forward declared here so http_post_stream
 * can check whether the terminal is in raw mode before watching
 * stdin for interrupt keystrokes. The actual implementation lives
 * further down alongside read_line.
 * ============================================================ */

#include <termios.h>

#define SW_RL_HIST_MAX 64

typedef struct {
    struct termios saved;
    int saved_ok;
    char *entries[SW_RL_HIST_MAX];
    int count;
} _sw_rl_state_t;

static _sw_rl_state_t _sw_rl = {0};

/* ============================================================
 * popen with pid tracking — so we can kill on interrupt
 * ============================================================
 *
 * Standard popen() hides the child pid. We need it to deliver
 * SIGTERM when the user hits ESC during an LLM stream. This is a
 * minimal fork+exec+pipe that gives us back both the read FILE*
 * and the child pid. Uses /bin/sh -c like popen() does, so the
 * command string format is identical. */

typedef struct {
    FILE *fp;
    pid_t pid;
} _sw_popen_pid_t;

static _sw_popen_pid_t _sw_popen_pid(const char *cmd) {
    _sw_popen_pid_t r;
    r.fp = NULL;
    r.pid = -1;
#ifndef _WIN32
    int pipefd[2];
    if (pipe(pipefd) < 0) return r;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return r;
    }
    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* Put child in its own process group so killpg() later can
         * take down the whole subshell + curl without signaling us. */
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    /* parent */
    close(pipefd[1]);
    r.fp = fdopen(pipefd[0], "r");
    r.pid = pid;
#else
    (void)cmd;
#endif
    return r;
}

/* Kill the child's process group with SIGTERM so curl and any shell
 * middleman both die. Then reap via waitpid so we don't leave zombies. */
static int _sw_pkill_close(_sw_popen_pid_t p) {
#ifndef _WIN32
    if (p.pid > 0) {
        killpg(p.pid, SIGTERM);
        /* Give it 200ms to exit gracefully, then SIGKILL. */
        for (int i = 0; i < 20; i++) {
            int status;
            pid_t r = waitpid(p.pid, &status, WNOHANG);
            if (r == p.pid) break;
            usleep(10000);
        }
        killpg(p.pid, SIGKILL);
        int status;
        waitpid(p.pid, &status, 0);
    }
#endif
    if (p.fp) fclose(p.fp);
    return 0;
}

static int _sw_popen_pid_close(_sw_popen_pid_t p) {
#ifndef _WIN32
    if (p.fp) fclose(p.fp);
    int status = 0;
    if (p.pid > 0) waitpid(p.pid, &status, 0);
    return status;
#else
    if (p.fp) fclose(p.fp);
    return 0;
#endif
}

/* ============================================================
 * Bidirectional subprocess — for MCP stdio + any long-lived child
 * ============================================================
 *
 * `shell()` is one-shot. `_sw_popen_pid` is read-only. For protocols
 * like MCP (newline-delimited JSON-RPC over stdio) we need a live
 * subprocess we can write to AND read from across many turns.
 *
 * Builtins:
 *   subprocess_spawn(cmd)              → handle (int) | -1 on error
 *   subprocess_send_line(h, line)      → 'ok' | 'error'  (appends \n if missing)
 *   subprocess_recv_line(h, [timeout]) → string | nil    (default 5000 ms)
 *   subprocess_close(h)                → 'ok' | 'error'
 *
 * Implementation: a small registry of (pid, stdin_fd, stdout_fd, buf).
 * 32 concurrent subprocesses cap — enough for any realistic agent.
 * Line-buffered on the read side; partial chunks survive across recv
 * calls in the slot's buffer. */
/* Forward decl — defined later with the spinner utilities. */
static uint64_t _sw_now_ms(void);

#define _SW_SUBPROC_MAX 32
typedef struct {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    char *buf;
    int buf_len;
    int buf_cap;
    int active;
} _sw_subproc_t;
static _sw_subproc_t _sw_subprocs[_SW_SUBPROC_MAX];

static sw_val_t *_builtin_subprocess_spawn(sw_val_t **a, int n) {
#ifdef _WIN32
    (void)a; (void)n;
    return sw_val_int(-1);
#else
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_int(-1);
    int slot = -1;
    for (int i = 0; i < _SW_SUBPROC_MAX; i++) if (!_sw_subprocs[i].active) { slot = i; break; }
    if (slot < 0) return sw_val_int(-1);

    int p_in[2], p_out[2];
    if (pipe(p_in) != 0) return sw_val_int(-1);
    if (pipe(p_out) != 0) { close(p_in[0]); close(p_in[1]); return sw_val_int(-1); }

    pid_t pid = fork();
    if (pid < 0) {
        close(p_in[0]); close(p_in[1]); close(p_out[0]); close(p_out[1]);
        return sw_val_int(-1);
    }
    if (pid == 0) {
        /* child */
        dup2(p_in[0], STDIN_FILENO);
        dup2(p_out[1], STDOUT_FILENO);
        close(p_in[0]); close(p_in[1]); close(p_out[0]); close(p_out[1]);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", a[0]->v.str, (char *)NULL);
        _exit(127);
    }
    /* parent */
    close(p_in[0]);
    close(p_out[1]);

    _sw_subprocs[slot].pid = pid;
    _sw_subprocs[slot].stdin_fd = p_in[1];
    _sw_subprocs[slot].stdout_fd = p_out[0];
    _sw_subprocs[slot].buf = (char *)malloc(4096);
    _sw_subprocs[slot].buf_len = 0;
    _sw_subprocs[slot].buf_cap = 4096;
    _sw_subprocs[slot].active = 1;
    return sw_val_int(slot);
#endif
}

static sw_val_t *_builtin_subprocess_send_line(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT || !a[1] || a[1]->type != SW_VAL_STRING)
        return sw_val_atom("error");
    int slot = (int)a[0]->v.i;
    if (slot < 0 || slot >= _SW_SUBPROC_MAX || !_sw_subprocs[slot].active)
        return sw_val_atom("error");
    const char *s = a[1]->v.str;
    size_t len = strlen(s);
    ssize_t w = write(_sw_subprocs[slot].stdin_fd, s, len);
    if (w < 0) return sw_val_atom("error");
    if (len == 0 || s[len - 1] != '\n') {
        if (write(_sw_subprocs[slot].stdin_fd, "\n", 1) < 0) return sw_val_atom("error");
    }
    return sw_val_atom("ok");
}

static sw_val_t *_builtin_subprocess_recv_line(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_nil();
    int slot = (int)a[0]->v.i;
    if (slot < 0 || slot >= _SW_SUBPROC_MAX || !_sw_subprocs[slot].active) return sw_val_nil();
    int timeout_ms = (n >= 2 && a[1] && a[1]->type == SW_VAL_INT) ? (int)a[1]->v.i : 5000;

    _sw_subproc_t *sp = &_sw_subprocs[slot];
    uint64_t start = _sw_now_ms();
    while (1) {
        /* Look for a complete line in the buffer. */
        for (int i = 0; i < sp->buf_len; i++) {
            if (sp->buf[i] == '\n') {
                char *line = (char *)malloc(i + 1);
                memcpy(line, sp->buf, i);
                line[i] = '\0';
                int rest = sp->buf_len - (i + 1);
                if (rest > 0) memmove(sp->buf, sp->buf + i + 1, rest);
                sp->buf_len = rest;
                sw_val_t *r = sw_val_string(line);
                free(line);
                return r;
            }
        }
        /* No complete line yet; check timeout, then read more. */
        if (timeout_ms > 0 && _sw_now_ms() - start >= (uint64_t)timeout_ms) return sw_val_nil();

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sp->stdout_fd, &rfds);
        struct timeval tv = { 0, 100000 }; /* 100ms tick */
        int ret = select(sp->stdout_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) { if (errno == EINTR) continue; return sw_val_nil(); }
        if (ret == 0) continue; /* timeout, recheck */

        /* Grow buffer if near full. */
        if (sp->buf_cap - sp->buf_len < 1024) {
            sp->buf_cap *= 2;
            sp->buf = (char *)realloc(sp->buf, sp->buf_cap);
        }
        ssize_t got = read(sp->stdout_fd, sp->buf + sp->buf_len, sp->buf_cap - sp->buf_len);
        if (got > 0) sp->buf_len += (int)got;
        else if (got == 0) return sw_val_nil(); /* EOF */
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return sw_val_nil();
    }
}

static sw_val_t *_builtin_subprocess_close(sw_val_t **a, int n) {
#ifdef _WIN32
    (void)a; (void)n;
    return sw_val_atom("error");
#else
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    int slot = (int)a[0]->v.i;
    if (slot < 0 || slot >= _SW_SUBPROC_MAX || !_sw_subprocs[slot].active) return sw_val_atom("error");
    _sw_subproc_t *sp = &_sw_subprocs[slot];
    close(sp->stdin_fd);
    close(sp->stdout_fd);
    /* Give the child 100ms to exit on its own (close stdin → graceful
     * shutdown for MCP servers), then SIGTERM, then SIGKILL. */
    for (int i = 0; i < 10; i++) {
        int st;
        if (waitpid(sp->pid, &st, WNOHANG) == sp->pid) goto cleaned;
        usleep(10000);
    }
    killpg(sp->pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        int st;
        if (waitpid(sp->pid, &st, WNOHANG) == sp->pid) goto cleaned;
        usleep(10000);
    }
    killpg(sp->pid, SIGKILL);
    { int st; waitpid(sp->pid, &st, 0); }
cleaned:
    free(sp->buf);
    sp->buf = NULL;
    sp->active = 0;
    return sw_val_atom("ok");
#endif
}

/* ============================================================
 * http_post — popen+select implementation with soft interrupt
 * ============================================================
 *
 * Refactored from a blocking system() call. Same retry semantics,
 * same response shape as before — but the user can hit ESC (or
 * Ctrl-C in raw-mode terminals) to abort an in-flight request.
 * On interrupt, the curl child is SIGTERM'd via killpg, the partial
 * response is discarded, and we return the sentinel string
 * "__INTERRUPTED__" so the sw caller can show a "stopped" notice
 * and skip retry / return to the prompt.
 *
 * Why this lives here (not next to the forward decl above): the
 * popen+select machinery and the line-editor raw-mode flag are
 * defined just above this section, so the implementation can only
 * follow them in source order.
 * ============================================================ */
static sw_val_t *_builtin_http_post(sw_val_t **a, int n) {
    if (n < 3 || a[0]->type != SW_VAL_STRING || a[2]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *url = a[0]->v.str, *body = a[2]->v.str;

    /* Build the curl command once — reused across retry attempts. */
    size_t cmdcap = strlen(body) + strlen(url) + 4096;
    char *cmd = (char *)malloc(cmdcap);
    int off = 0;
    off += snprintf(cmd + off, cmdcap - off,
        "curl -sS -X POST --connect-timeout 30 --max-time 300");

    sw_val_t *headers = a[1];
    if (headers && headers->type == SW_VAL_LIST) {
        for (int i = 0; i < headers->v.tuple.count; i++) {
            sw_val_t *h = headers->v.tuple.items[i];
            if (h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->v.str && h->v.tuple.items[1]->v.str)
                off += snprintf(cmd + off, cmdcap - off, " -H '%s: %s'",
                    h->v.tuple.items[0]->v.str, h->v.tuple.items[1]->v.str);
        }
    }

    /* Body via temp file — keeps curl arg list small and avoids
     * shell-escaping the JSON. */
    char tmpf[256];
    snprintf(tmpf, sizeof(tmpf),
        "%s/sw_http_%d_%u.json", sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    FILE *tf = fopen(tmpf, "w");
    if (tf) { fputs(body, tf); fclose(tf); }

    /* No -o flag — let curl write to its pipe to us so we can stream
     * into a growable buffer (and bail mid-flight on interrupt). */
    off += snprintf(cmd + off, cmdcap - off,
        " -d @%s '%s' 2>/dev/null", tmpf, url);

    /* Stdin interrupt watcher — only enabled when stdin is a TTY AND
     * the line editor has set up raw mode (so ESC arrives as a single
     * byte without waiting for newline). swarm-code's reader process
     * triggers _sw_rl_setup on first read_line; from that point on
     * the terminal stays in raw mode for the rest of the session. */
    int stdin_fd = -1;
    if (isatty(STDIN_FILENO) && _sw_rl.saved_ok) stdin_fd = STDIN_FILENO;

    int interrupted = 0;
    char *resp_buf = NULL;
    size_t resp_len = 0;
    size_t resp_cap_local = 65536;

    int delays[] = {0, 5, 15};
    for (int attempt = 0; attempt < 3 && !interrupted; attempt++) {
        if (delays[attempt] > 0) sw_sleep(delays[attempt]);

        if (resp_buf) free(resp_buf);
        resp_buf = (char *)malloc(resp_cap_local);
        resp_len = 0;
        resp_buf[0] = 0;

        _sw_popen_pid_t ch = _sw_popen_pid(cmd);
        if (!ch.fp) continue;
        int pipe_fd = fileno(ch.fp);
        int fl = fcntl(pipe_fd, F_GETFL, 0);
        if (fl >= 0) fcntl(pipe_fd, F_SETFL, fl | O_NONBLOCK);

        char readbuf[4096];
        int done = 0;
        while (!done) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(pipe_fd, &rfds);
            if (stdin_fd >= 0) FD_SET(stdin_fd, &rfds);
            int max_fd = pipe_fd;
            if (stdin_fd > max_fd) max_fd = stdin_fd;

            /* 1-second tick — just keeps select from blocking forever
             * if both fds go quiet. Could be longer; 1s is fine. */
            struct timeval tv = {1, 0};
            int ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                done = 1; break;
            }

            if (stdin_fd >= 0 && FD_ISSET(stdin_fd, &rfds)) {
                unsigned char ib;
                ssize_t nb = read(stdin_fd, &ib, 1);
                if (nb == 1 && (ib == 0x1b || ib == 0x03)) {
                    interrupted = 1;
                    fputs("\n  \x1b[38;5;208m⏸ interrupted by user\x1b[0m\n", stdout);
                    fflush(stdout);
                    _sw_pkill_close(ch);
                    ch.fp = NULL; ch.pid = -1;
                    done = 1; break;
                }
                /* Other keystrokes during the wait: ignore. */
            }

            if (FD_ISSET(pipe_fd, &rfds)) {
                ssize_t rn = read(pipe_fd, readbuf, sizeof(readbuf));
                if (rn < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                    done = 1; break;
                }
                if (rn == 0) { done = 1; break; }   /* EOF */
                if (resp_len + (size_t)rn + 1 > resp_cap_local) {
                    while (resp_len + (size_t)rn + 1 > resp_cap_local) resp_cap_local *= 2;
                    resp_buf = (char *)realloc(resp_buf, resp_cap_local);
                }
                memcpy(resp_buf + resp_len, readbuf, rn);
                resp_len += (size_t)rn;
                resp_buf[resp_len] = 0;
            }
        }

        if (!interrupted && ch.fp) _sw_popen_pid_close(ch);
        if (interrupted) break;

        /* Server error or empty body → retry. Otherwise we're done. */
        if (resp_len > 0 &&
            strstr(resp_buf, "Internal Server Error") == NULL &&
            strstr(resp_buf, "\"error\"") == NULL) {
            break;
        }
    }

    swbs_unlink(tmpf);
    free(cmd);

    if (interrupted) {
        free(resp_buf);
        return sw_val_string("__INTERRUPTED__");
    }

    sw_val_t *r = sw_val_string(resp_buf);
    free(resp_buf);
    return r;
}

/* ============================================================
 * Stream emitter — column-aware output for streamed LLM content
 * ============================================================
 *
 * Enforces a 2-column left margin (so assistant text aligns with
 * the bordered input box) and wraps lines at (term_width - 2) so
 * long lines don't overrun the right edge.
 *
 * Correctness details:
 *   - UTF-8 continuation bytes (10xxxxxx) don't advance the column;
 *     only the first byte of a sequence does. Roughly correct for
 *     most text (wide chars like emoji count as 1 col, but that's
 *     a rare case and the wrap just happens one col late).
 *   - ANSI CSI sequences (\x1b [ ... final-byte) are passed through
 *     without advancing the column counter.
 *   - Newlines reset column to 0 and defer the 2-space indent until
 *     the next non-newline byte, so blank lines stay blank.
 *   - Wrapping is mid-column for v1 — no word-boundary detection.
 */

typedef struct {
    int term_w;
    int right_margin;
    int col;
    int in_ansi;
    int line_started;
} _sw_stream_state_t;

static int _sw_term_cols(void) {
#ifndef _WIN32
    struct winsize ws;
    /* Try stdout first, then stderr, then /dev/tty — one of them will be
     * a real TTY even when the others are piped. `tput cols` via shell()
     * fails because its stdout is a pipe; ioctl on /dev/tty doesn't have
     * that problem. */
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd >= 0) {
        int col = 0;
        if (ioctl(tty_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            col = (int)ws.ws_col;
        close(tty_fd);
        if (col > 0) return col;
    }
#endif
    return 80;
}

/* sw builtin: term_cols() → int  (number of columns in the current tty) */
static sw_val_t *_builtin_term_cols(sw_val_t **a, int n) {
    (void)a; (void)n;
    return sw_val_int((int64_t)_sw_term_cols());
}

static void _sw_stream_init(_sw_stream_state_t *s) {
    int w = _sw_term_cols();
    s->term_w = w;
    s->right_margin = (w > 10) ? (w - 2) : w;
    s->col = 0;
    s->in_ansi = 0;
    s->line_started = 0;
}

static void _sw_stream_reset_line(_sw_stream_state_t *s) {
    /* Called after \r\e[K clears the spinner line: cursor is at col 0
     * and we need to re-emit the 2-space indent before the next byte. */
    s->col = 0;
    s->line_started = 0;
    s->in_ansi = 0;
}

static void _sw_stream_emit(_sw_stream_state_t *s, char c) {
    /* Lazy leading indent: emit 2 spaces at the start of each line,
     * but only when about to write a non-newline byte. */
    if (!s->line_started && !s->in_ansi && c != '\n' && c != '\r' && c != '\x1b') {
        fputs("  ", stdout);
        s->col = 2;
        s->line_started = 1;
    }
    if (s->in_ansi) {
        fputc(c, stdout);
        if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) s->in_ansi = 0;
        return;
    }
    if (c == '\x1b') {
        s->in_ansi = 1;
        fputc(c, stdout);
        return;
    }
    if (c == '\n') {
        fputc(c, stdout);
        s->col = 0;
        s->line_started = 0;
        return;
    }
    if (c == '\r') {
        fputc(c, stdout);
        s->col = 0;
        return;
    }
    int is_cont = ((unsigned char)c & 0xC0) == 0x80;
    if (!is_cont && s->col >= s->right_margin) {
        fputs("\n  ", stdout);
        s->col = 2;
        s->line_started = 1;
    }
    fputc(c, stdout);
    if (!is_cont) s->col++;
}

/* ============================================================
 * Spinner — Claude-Code-style rotating verb + elapsed time
 * ============================================================
 *
 * Shown during the dead-air phase between sending an LLM request and
 * receiving the first streamed token. Cleared the moment real content
 * is about to be printed.
 *
 * The verb list is inspired by Claude Code's spinnerVerbs.ts — a
 * curated subset trimmed for binary size. One verb is picked per
 * http_post_stream call and stays stable for the whole call, matching
 * CC's behavior (verb is constant, elapsed time + tokens tick).
 */

static const char *_sw_spinner_verbs[] = {
    "Accomplishing", "Architecting", "Baking", "Beaming", "Brewing",
    "Calculating", "Cerebrating", "Churning", "Cogitating", "Concocting",
    "Contemplating", "Cooking", "Crafting", "Creating", "Crunching",
    "Crystallizing", "Deliberating", "Deciphering", "Divining", "Doodling",
    "Enchanting", "Envisioning", "Fermenting", "Finagling", "Forging",
    "Forming", "Gallivanting", "Generating", "Germinating", "Hashing",
    "Hatching", "Ideating", "Imagining", "Incubating", "Inferring",
    "Infusing", "Kneading", "Manifesting", "Marinating", "Meandering",
    "Mulling", "Musing", "Noodling", "Orchestrating", "Percolating",
    "Pondering", "Processing", "Puzzling", "Ruminating", "Scheming",
    "Simmering", "Sketching", "Spelunking", "Spinning", "Stewing",
    "Swirling", "Synthesizing", "Thinking", "Tinkering", "Transfiguring",
    "Unfurling", "Vibing", "Wandering", "Whisking", "Working",
    "Wrangling", "Zesting"
};
static const int _sw_spinner_verb_count =
    (int)(sizeof(_sw_spinner_verbs) / sizeof(_sw_spinner_verbs[0]));

static uint64_t _sw_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

static const char *_sw_pick_spinner_verb(void) {
    static int seeded = 0;
    if (!seeded) {
        /* sw_random_u32 is already entropy-seeded; use it. */
        seeded = 1;
    }
    unsigned r = sw_random_u32();
    return _sw_spinner_verbs[r % (unsigned)_sw_spinner_verb_count];
}

/* Format elapsed ms as a compact human duration: "0.3s", "12s", "1m 23s". */
static void _sw_format_elapsed(uint64_t ms, char *out, size_t outcap) {
    if (ms < 1000) {
        snprintf(out, outcap, "%llums", (unsigned long long)ms);
    } else if (ms < 10000) {
        snprintf(out, outcap, "%.1fs", (double)ms / 1000.0);
    } else if (ms < 60000) {
        snprintf(out, outcap, "%llus", (unsigned long long)(ms / 1000));
    } else if (ms < 3600000) {
        unsigned long long mins = ms / 60000;
        unsigned long long secs = (ms % 60000) / 1000;
        snprintf(out, outcap, "%llum %llus", mins, secs);
    } else {
        unsigned long long hrs = ms / 3600000;
        unsigned long long mins = (ms % 3600000) / 60000;
        snprintf(out, outcap, "%lluh %llum", hrs, mins);
    }
}

/* Draw/refresh the spinner line in-place using \r and clear-to-EOL. */
static void _sw_spinner_draw(const char *verb, uint64_t elapsed_ms, int tokens) {
    /* Frame glyphs: 6 forward + 6 reverse for a gentle bloom. */
    static const char *frames[] = {
        "·", "✢", "✳", "✶", "✻", "✽",
        "✻", "✶", "✳", "✢", "·", "·"
    };
    static const int frame_count = 12;
    int frame = (int)((elapsed_ms / 120) % (uint64_t)frame_count);

    char elapsed_s[32];
    _sw_format_elapsed(elapsed_ms, elapsed_s, sizeof(elapsed_s));

    /* Colors: deep-red glyph (brand), dim verb, dim parenthetical. */
    const char *c_brand = "\x1b[38;5;124m";
    const char *c_dim   = "\x1b[38;5;240m";
    const char *c_reset = "\x1b[0m";

    /* Only show token counter once we've accumulated something worth showing. */
    if (tokens >= 25) {
        printf("\r\x1b[K %s%s%s %s%s… (%s · ↑ %d tokens · esc to interrupt)%s",
               c_brand, frames[frame], c_reset,
               c_dim, verb, elapsed_s, tokens, c_reset);
    } else {
        printf("\r\x1b[K %s%s%s %s%s… (%s · esc to interrupt)%s",
               c_brand, frames[frame], c_reset,
               c_dim, verb, elapsed_s, c_reset);
    }
    fflush(stdout);
}

/*
 * Stream output sink for http_post_stream.
 *
 * Two modes:
 *   1. tty mode (subagent == 0)  — bytes go to stdout via the column-aware
 *      `_sw_stream_emit` (the existing wrap+indent behaviour).
 *   2. subagent mode (subagent == 1) — bytes are batched into small chunks
 *      and sent as `{'stream_chunk', name, text}` messages to `target` so
 *      a parent agent can multiplex many subagent streams without
 *      interleaving on a shared TTY. No spinner, no ESC interrupt path,
 *      no inline reasoning UI — the parent decides how to render.
 */
typedef struct {
    int subagent;
    sw_process_t *target;
    const char *name;       /* agent label, included in every message */
    char chunk[256];
    int chunk_len;
} _stream_out_t;

static void _stream_out_init(_stream_out_t *o, sw_process_t *target, const char *name) {
    o->subagent = (target != NULL);
    o->target = target;
    o->name = name;
    o->chunk_len = 0;
}

static void _stream_out_send_tagged(sw_process_t *target, const char *tag,
                                    const char *name, const char *text) {
    sw_val_t *items[3];
    items[0] = sw_val_atom(tag);
    items[1] = sw_val_string(name);
    items[2] = sw_val_string(text);
    sw_val_t *msg = sw_val_tuple(items, 3);
    sw_send_tagged(target, SW_TAG_NONE, msg);
}

static void _stream_out_flush(_stream_out_t *o) {
    if (!o->subagent || o->chunk_len == 0) return;
    o->chunk[o->chunk_len] = '\0';
    _stream_out_send_tagged(o->target, "stream_chunk", o->name, o->chunk);
    o->chunk_len = 0;
}

static void _stream_out_putc(_stream_out_t *o, char c, _sw_stream_state_t *st) {
    if (!o->subagent) {
        _sw_stream_emit(st, c);
        return;
    }
    if (o->chunk_len >= (int)sizeof(o->chunk) - 1) _stream_out_flush(o);
    o->chunk[o->chunk_len++] = c;
    /* Flush at a newline so the parent renderer can produce nice line breaks. */
    if (c == '\n') _stream_out_flush(o);
}

/*
 * http_post_stream(url, headers_list, body, [target_pid, name]) → string (full content)
 *
 * POSTs a JSON body with "stream": true to an OpenAI-compatible endpoint,
 * parses the Server-Sent-Events stream token-by-token, prints each
 * delta.content to stdout as it arrives (for visual streaming), and
 * returns the complete accumulated assistant content as a string wrapped
 * in a minimal OpenAI response shape so the existing extract_content
 * path keeps working:
 *    {"choices":[{"message":{"role":"assistant","content":"..."}}]}
 *
 * If the optional 4th + 5th args (target_pid, name_string) are supplied,
 * runs in "subagent mode": no stdout, no spinner, no ESC interrupt — every
 * content chunk is sent to target_pid as `{'stream_chunk', name, text}`,
 * reasoning chunks as `{'stream_reason', name, text}`, and a final
 * `{'stream_done', name}` is sent when the call ends. The parent agent is
 * responsible for rendering them with whatever prefix / colour it likes.
 *
 * While waiting for the first byte from upstream, a Claude-Code-style
 * spinner shows on stdout (verb + elapsed + token estimate). It is
 * cleared the instant real content is about to be printed or a tool
 * call marker is detected.
 *
 * Detects <tool_call> in the stream and stops printing to stdout once
 * it's seen, so tool-call JSON doesn't leak into the user's view.
 */
static sw_val_t *_builtin_http_post_stream(sw_val_t **a, int n) {
    if (n < 3 || a[0]->type != SW_VAL_STRING || a[2]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *url = a[0]->v.str;
    sw_val_t *headers = a[1];
    const char *body = a[2]->v.str;

    /* Subagent mode: route content+reasoning as messages, skip TTY UI. */
    sw_process_t *subagent_target = NULL;
    const char *subagent_name = "agent";
    if (n >= 5 && a[3] && a[3]->type == SW_VAL_PID && a[4] && a[4]->type == SW_VAL_STRING) {
        subagent_target = a[3]->v.pid;
        subagent_name = a[4]->v.str;
    }
    _stream_out_t so;
    _stream_out_init(&so, subagent_target, subagent_name);

    /* Write body to temp file to avoid shell-escaping the JSON. */
    char body_file[256];
    snprintf(body_file, sizeof(body_file), "%s/sw_stream_%d_%u.json",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    FILE *bf = fopen(body_file, "w");
    if (!bf) return sw_val_nil();
    fputs(body, bf);
    fclose(bf);

    /* Redirect curl's stderr to a temp file so we can surface real errors
     * (ECONNREFUSED, NXDOMAIN, TLS failures, etc) instead of silently
     * returning empty content and leaving the user to guess. Previously
     * we piped stderr to /dev/null which hid every transport failure. */
    char err_file[256];
    snprintf(err_file, sizeof(err_file), "%s/sw_stream_err_%d_%u.txt",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());

    /* Build the curl command with headers + -N for unbuffered streaming. */
    size_t cmdcap = 8192;
    char *cmd = (char *)malloc(cmdcap);
    int off = 0;
    /* --keepalive-time 30: send TCP keepalives so flaky long-distance
     *   routes (api.z.ai, sushi, anything overseas) don't silently drop
     *   an idle stream during long reasoning chains.
     * --retry 2 --retry-delay 1 --retry-connrefused --retry-all-errors:
     *   curl auto-retries connection failures BEFORE any data arrives;
     *   does NOT restart an already-streaming response (so safe for
     *   streaming). Catches transient SSL/connect timeouts (curl 28/35).
     * --max-time 1800: hard ceiling at 30 min — long reasoning is fine
     *   but eventually we want to surface a failure rather than hang. */
    off += snprintf(cmd + off, cmdcap - off,
        "curl -sS -N -X POST --connect-timeout 30 --max-time 1800 "
        "--keepalive-time 30 "
        "--retry 2 --retry-delay 1 --retry-connrefused --retry-all-errors");
    if (headers && headers->type == SW_VAL_LIST) {
        for (int i = 0; i < headers->v.tuple.count; i++) {
            sw_val_t *h = headers->v.tuple.items[i];
            if (h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->v.str && h->v.tuple.items[1]->v.str)
                off += snprintf(cmd + off, cmdcap - off, " -H '%s: %s'",
                    h->v.tuple.items[0]->v.str, h->v.tuple.items[1]->v.str);
        }
    }
    off += snprintf(cmd + off, cmdcap - off,
        " --data-binary @%s '%s' 2>%s", body_file, url, err_file);

    _sw_popen_pid_t ch = _sw_popen_pid(cmd);
    FILE *pp = ch.fp;
    free(cmd);
    if (!pp) { swbs_unlink(body_file); swbs_unlink(err_file); return sw_val_nil(); }

    /* Put the pipe in non-blocking mode so we can tick the spinner while
     * waiting for the first byte from the upstream LLM. */
    int pipe_fd = fileno(pp);
    int _fl = fcntl(pipe_fd, F_GETFL, 0);
    if (_fl >= 0) fcntl(pipe_fd, F_SETFL, _fl | O_NONBLOCK);

    /* If stdin is a TTY and already in raw mode (the line editor set it
     * up at startup), we can watch it for ESC / Ctrl+C to interrupt the
     * streaming generation. Not fatal if it isn't a TTY — we just skip
     * the interrupt path. */
    int stdin_fd = -1;
    if (!so.subagent && isatty(STDIN_FILENO) && _sw_rl.saved_ok) {
        stdin_fd = STDIN_FILENO;
    }
    int interrupted = 0;

    /* Pick one random verb for this call. Spinner only on a TTY and
     * only when not a subagent — subagents never touch stdout. */
    int is_tty = !so.subagent && isatty(fileno(stdout));
    const char *verb = _sw_pick_spinner_verb();
    int spinner_drawn = 0;
    uint64_t t_start = _sw_now_ms();
    uint64_t t_next_tick = t_start;

    /* Column-aware stream emitter: 2-col left indent + wrap at term_w-2. */
    _sw_stream_state_t stream;
    _sw_stream_init(&stream);

    /* Accumulate all token deltas here. We keep a lookahead window so
     * tool-call markers can be fully detected before any of their bytes
     * leak to the user's terminal.  We detect TWO formats:
     *   1. <tool_call>{...}</tool_call>  — prompted XML wrapper
     *   2. \ncall:name{...}              — Gemma 4 native format
     * The lookahead must cover the longer marker ("<tool_call>" = 11). */
    size_t buf_cap = 65536, buf_len = 0;
    char *buffer = (char *)malloc(buf_cap);
    buffer[0] = '\0';
    int seen_tool_call = 0;
    size_t print_pos = 0; /* next byte in buffer to be considered for stdout */
    const size_t LOOKAHEAD = 11; /* max(strlen("<tool_call>"), strlen("\ncall:X")) */

    /* Reasoning channel — GLM-5.1, DeepSeek-R1, and o1-style models
     * stream `delta.reasoning_content` separately from `delta.content`.
     * We display reasoning dimmed+italic inline so the user sees the
     * model think, but never add it to `buffer` — only `content` is
     * scanned for tool-call markers. Also exposed in the response JSON
     * so the agent layer can detect "thought but said nothing" and
     * decide to retry instead of giving up. */
    size_t reason_cap = 16384, reason_len = 0;
    char *reasoning = (char *)malloc(reason_cap);
    reasoning[0] = '\0';
    int reason_started = 0;  /* have we opened the dim italic block? */
    int content_started = 0; /* have we transitioned reasoning → content? */

    /* Draw the spinner immediately so the user sees *something* at t=0. */
    if (is_tty) {
        _sw_spinner_draw(verb, 0, 0);
        spinner_drawn = 1;
    }

    /* Line buffer for SSE parsing. We read non-blocking into a scratch
     * buffer and split lines ourselves — fgets() would block and prevent
     * spinner ticking during dead air. */
    char line[16384];
    size_t line_len = 0;
    char readbuf[4096];
    int done = 0;
    const int spinner_tick_ms = 80;

    /* Scrape `usage.prompt_tokens` / `completion_tokens` from whichever
     * SSE chunk carries it (transformers-serve emits usage in the final
     * chunk after `finish_reason`). Using server-provided token counts
     * instead of char estimates lets us budget compaction against the
     * model's actual context window. */
    long long prompt_tokens = -1;
    long long completion_tokens = -1;
    long long total_tokens = -1;
    /* Track the server's finish_reason so we can distinguish a normal
     * "stop" from a truncation ("length") or a content filter. */
    char finish_reason[32] = {0};

    while (!done) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipe_fd, &rfds);
        if (stdin_fd >= 0) FD_SET(stdin_fd, &rfds);
        int max_fd = pipe_fd;
        if (stdin_fd > max_fd) max_fd = stdin_fd;
        uint64_t now = _sw_now_ms();
        uint64_t remaining = (t_next_tick > now) ? (t_next_tick - now) : 0;
        struct timeval tv;
        tv.tv_sec = (time_t)(remaining / 1000);
        tv.tv_usec = (suseconds_t)((remaining % 1000) * 1000);
        int ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Check stdin for an interrupt keystroke. ESC (0x1b) or
         * Ctrl+C (0x03) aborts the stream, kills the child, and
         * returns the partial buffer with an [Interrupted] marker
         * the model will see on the next turn. */
        if (stdin_fd >= 0 && FD_ISSET(stdin_fd, &rfds)) {
            unsigned char ib;
            ssize_t nb = read(stdin_fd, &ib, 1);
            if (nb == 1 && (ib == 0x1b || ib == 0x03)) {
                interrupted = 1;
                if (spinner_drawn) {
                    fputs("\r\x1b[K", stdout);
                    spinner_drawn = 0;
                    _sw_stream_reset_line(&stream);
                }
                fputs("\n  \x1b[38;5;208m⏸ interrupted by user\x1b[0m\n", stdout);
                fflush(stdout);
                _sw_pkill_close(ch);
                pp = NULL;
                ch.fp = NULL;
                ch.pid = -1;
                done = 1;
                break;
            }
            /* Any other keystroke while streaming: ignore and loop. */
        }
        if (ret == 0) {
            /* Timeout — tick the spinner whenever it's supposed to be
             * showing. Three cases need it:
             *   1. Dead air before the first content byte (TTFT wait)
             *   2. After we've detected <tool_call> and are still
             *      receiving the rest of the JSON (avoids the silent
             *      multi-second gap between rationale text and the
             *      tool header)
             *   3. After a blank/whitespace-only start where the model
             *      skipped rationale and went straight to the tool_call
             * The `spinner_drawn` flag is set exactly when the spinner
             * should be visible on screen right now, so we just refresh
             * it. */
            if (is_tty && spinner_drawn) {
                uint64_t elapsed = _sw_now_ms() - t_start;
                int est_tokens = (int)(buf_len / 4);
                _sw_spinner_draw(verb, elapsed, est_tokens);
            }
            t_next_tick = _sw_now_ms() + (uint64_t)spinner_tick_ms;
            continue;
        }

        ssize_t rn = read(pipe_fd, readbuf, sizeof(readbuf));
        if (rn < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        if (rn == 0) { done = 1; break; }

        for (ssize_t ri = 0; ri < rn && !done; ri++) {
            char ch = readbuf[ri];
            if (line_len < sizeof(line) - 1) line[line_len++] = ch;
            if (ch != '\n') continue;
            line[line_len] = '\0';
            size_t this_line_len = line_len;
            line_len = 0;

            if (this_line_len < 6 || strncmp(line, "data: ", 6) != 0) continue;
            const char *json = line + 6;
            if (strncmp(json, "[DONE]", 6) == 0) { done = 1; break; }

            /* Opportunistically scrape token counts from any chunk
             * that includes a `usage` object. Final chunk usually
             * does; intermediate chunks usually don't. */
            {
                const char *u = strstr(json, "\"prompt_tokens\":");
                if (u) { prompt_tokens = strtoll(u + 16, NULL, 10); }
                u = strstr(json, "\"completion_tokens\":");
                if (u) { completion_tokens = strtoll(u + 20, NULL, 10); }
                u = strstr(json, "\"total_tokens\":");
                if (u) { total_tokens = strtoll(u + 15, NULL, 10); }
                /* finish_reason — capture on the last chunk that has one */
                const char *fr = strstr(json, "\"finish_reason\":\"");
                if (fr) {
                    fr += 17;
                    size_t k = 0;
                    while (*fr && *fr != '"' && k < sizeof(finish_reason) - 1) {
                        finish_reason[k++] = *fr++;
                    }
                    finish_reason[k] = '\0';
                }
            }

            /* Reasoning channel: parse first since it usually streams
             * before content. A chunk can have reasoning, content, both,
             * or neither. The `"reasoning_content":"` prefix can't false-
             * match `"content":"` because of the underscore boundary. */
            {
                const char *rp = strstr(json, "\"reasoning_content\":\"");
                if (rp) {
                    rp += 21;
                    char rtok[8192];
                    size_t rtok_len = 0;
                    while (*rp && *rp != '"' && rtok_len < sizeof(rtok) - 1) {
                        if (*rp == '\\' && *(rp + 1)) {
                            char esc = *(rp + 1);
                            switch (esc) {
                                case 'n': rtok[rtok_len++] = '\n'; rp += 2; break;
                                case 't': rtok[rtok_len++] = '\t'; rp += 2; break;
                                case 'r': rtok[rtok_len++] = '\r'; rp += 2; break;
                                case '"': rtok[rtok_len++] = '"'; rp += 2; break;
                                case '\\': rtok[rtok_len++] = '\\'; rp += 2; break;
                                case '/': rtok[rtok_len++] = '/'; rp += 2; break;
                                default: rtok[rtok_len++] = esc; rp += 2; break;
                            }
                        } else {
                            rtok[rtok_len++] = *rp++;
                        }
                    }
                    if (rtok_len > 0) {
                        if (reason_len + rtok_len + 1 > reason_cap) {
                            while (reason_len + rtok_len + 1 > reason_cap) reason_cap *= 2;
                            reasoning = (char *)realloc(reasoning, reason_cap);
                        }
                        memcpy(reasoning + reason_len, rtok, rtok_len);
                        reason_len += rtok_len;
                        reasoning[reason_len] = '\0';

                        if (so.subagent) {
                            /* Send each reasoning chunk as a message; parent
                             * decides whether/how to render it. */
                            char tmp[8200];
                            size_t cp = rtok_len < sizeof(tmp) - 1 ? rtok_len : sizeof(tmp) - 1;
                            memcpy(tmp, rtok, cp);
                            tmp[cp] = '\0';
                            _stream_out_send_tagged(so.target, "stream_reason", so.name, tmp);
                            reason_started = 1;
                        } else {
                            if (spinner_drawn) {
                                fputs("\r\x1b[K", stdout);
                                spinner_drawn = 0;
                                _sw_stream_reset_line(&stream);
                            }
                            if (!reason_started) {
                                fputs("  \x1b[38;5;240m\x1b[3m", stdout);
                                reason_started = 1;
                            }
                            fwrite(rtok, 1, rtok_len, stdout);
                            fflush(stdout);
                        }
                    }
                }
            }

            const char *p = strstr(json, "\"content\":\"");
            if (!p) continue;
            p += 11;

            char tok[8192];
            size_t tok_len = 0;
            while (*p && *p != '"' && tok_len < sizeof(tok) - 1) {
                if (*p == '\\' && *(p + 1)) {
                    char esc = *(p + 1);
                    switch (esc) {
                        case 'n': tok[tok_len++] = '\n'; p += 2; break;
                        case 't': tok[tok_len++] = '\t'; p += 2; break;
                        case 'r': tok[tok_len++] = '\r'; p += 2; break;
                        case '"': tok[tok_len++] = '"'; p += 2; break;
                        case '\\': tok[tok_len++] = '\\'; p += 2; break;
                        case '/': tok[tok_len++] = '/'; p += 2; break;
                        default: tok[tok_len++] = esc; p += 2; break;
                    }
                } else {
                    tok[tok_len++] = *p++;
                }
            }
            tok[tok_len] = '\0';

            /* Reasoning → content transition: close the dim italic block
             * with a separator so user sees the model's thinking end and
             * its actual response begin. Only fires once per turn.
             * In subagent mode there's no inline UI to close. */
            if (tok_len > 0 && reason_started && !content_started) {
                if (!so.subagent) {
                    fputs("\x1b[0m\n\n", stdout);
                    fflush(stdout);
                }
                content_started = 1;
            }

            if (buf_len + tok_len + 1 > buf_cap) {
                while (buf_len + tok_len + 1 > buf_cap) buf_cap *= 2;
                buffer = (char *)realloc(buffer, buf_cap);
            }
            memcpy(buffer + buf_len, tok, tok_len);
            buf_len += tok_len;
            buffer[buf_len] = '\0';

            if (!seen_tool_call) {
                /* Detect tool call: either <tool_call> or [punct/ws]call: */
                const char *marker = strstr(buffer, "<tool_call>");
                if (!marker) {
                    /* Native call:NAME{...} — scan from print_pos for
                     * `call:` not preceded by an identifier char. Accepts
                     * "\ncall:" (Gemma) and ".call:" / ",call:" (GLM-5.1)
                     * but rejects "http_call:" (false positive). */
                    const char *nc = NULL;
                    size_t scan_start = print_pos;
                    for (size_t i = scan_start; i + 5 <= buf_len; i++) {
                        if (buffer[i] != 'c') continue;
                        if (memcmp(buffer + i, "call:", 5) != 0) continue;
                        char prev = (i == 0) ? '\n' : buffer[i - 1];
                        int is_word = (prev >= 'a' && prev <= 'z') ||
                                      (prev >= 'A' && prev <= 'Z') ||
                                      (prev >= '0' && prev <= '9') ||
                                      prev == '_';
                        if (!is_word) { nc = buffer + i; break; }
                    }
                    if (nc) marker = nc;
                }
                if (marker) {
                    seen_tool_call = 1;
                    if (spinner_drawn) {
                        fputs("\r\x1b[K", stdout);
                        spinner_drawn = 0;
                        _sw_stream_reset_line(&stream);
                    }
                    size_t marker_idx = (size_t)(marker - buffer);
                    /* Emit text before the tool marker, but strip Gemma 4's
                     * bare "thought" rationale prefix — it's a model artifact
                     * that adds noise.  Only strip if the pre-marker text is
                     * JUST whitespace + "thought" + whitespace. */
                    {
                        size_t tmp = print_pos;
                        while (tmp < marker_idx && (buffer[tmp]==' '||buffer[tmp]=='\n'||buffer[tmp]=='\t'||buffer[tmp]=='\r')) tmp++;
                        int is_bare_thought = (marker_idx - tmp >= 7 && strncmp(buffer + tmp, "thought", 7) == 0);
                        if (is_bare_thought) {
                            size_t after_t = tmp + 7;
                            while (after_t < marker_idx && (buffer[after_t]==' '||buffer[after_t]=='\n'||buffer[after_t]=='\t'||buffer[after_t]=='\r')) after_t++;
                            if (after_t >= marker_idx) print_pos = marker_idx; /* skip it */
                        }
                    }
                    while (print_pos < marker_idx) {
                        _stream_out_putc(&so, buffer[print_pos++], &stream);
                    }
                    _stream_out_putc(&so, '\n', &stream);
                    if (!so.subagent) fflush(stdout);
                    _stream_out_flush(&so);
                    /* Re-raise the spinner so the user sees continuous
                     * progress while the rest of the tool_call JSON
                     * streams in, instead of a silent multi-second gap
                     * before the tool header lands. */
                    if (is_tty) {
                        uint64_t elapsed = _sw_now_ms() - t_start;
                        _sw_spinner_draw(verb, elapsed, (int)(buf_len / 4));
                        spinner_drawn = 1;
                    }
                } else {
                    /* About to emit real bytes — clear spinner first. */
                    if (print_pos + LOOKAHEAD < buf_len && spinner_drawn) {
                        fputs("\r\x1b[K", stdout);
                        spinner_drawn = 0;
                        _sw_stream_reset_line(&stream);
                    }
                    while (print_pos + LOOKAHEAD < buf_len) {
                        _stream_out_putc(&so, buffer[print_pos++], &stream);
                    }
                    if (so.subagent) _stream_out_flush(&so);
                    else fflush(stdout);
                }
            }
        }
    }

    /* EOF — flush whatever is still in the lookahead window. */
    if (spinner_drawn) {
        fputs("\r\x1b[K", stdout);
        spinner_drawn = 0;
        _sw_stream_reset_line(&stream);
    }
    /* If the model emitted reasoning but never started speaking content,
     * close the dim/italic block cleanly so the terminal styling resets.
     * In subagent mode there's no inline UI to close. */
    if (reason_started && !content_started && !so.subagent) {
        fputs("\x1b[0m\n", stdout);
        fflush(stdout);
    }
    if (!seen_tool_call) {
        while (print_pos < buf_len) {
            _stream_out_putc(&so, buffer[print_pos++], &stream);
        }
    }
    if (so.subagent) {
        _stream_out_flush(&so);
    } else {
        fputs("\n", stdout);
        fflush(stdout);
    }
    /* If the user interrupted, we already killed + waited for the
     * child inside _sw_pkill_close — don't re-close. Otherwise close
     * the FILE* and wait for the child normally. */
    int curl_status = 0;
    int curl_exit = 0;
    if (!interrupted && pp) {
        curl_status = _sw_popen_pid_close(ch);
#ifdef _WIN32
        curl_exit = curl_status;
#else
        curl_exit = WIFEXITED(curl_status) ? WEXITSTATUS(curl_status) : -1;
#endif
    }
    swbs_unlink(body_file);

    /* If curl failed, read its stderr and surface a real error to the
     * user. This is the difference between "server returned empty"
     * (confusing) and "curl: (7) Failed to connect to sushi port 8000:
     * Connection refused" (actionable). */
    if (curl_exit != 0 && buf_len == 0) {
        FILE *ef = fopen(err_file, "r");
        char errbuf[1024];
        size_t erlen = 0;
        if (ef) {
            erlen = fread(errbuf, 1, sizeof(errbuf) - 1, ef);
            fclose(ef);
        }
        errbuf[erlen] = '\0';
        /* Strip trailing newline for cleaner display. */
        while (erlen > 0 && (errbuf[erlen - 1] == '\n' || errbuf[erlen - 1] == '\r')) {
            errbuf[--erlen] = '\0';
        }
        if (so.subagent) {
            char emsg[1100];
            snprintf(emsg, sizeof(emsg), "[curl exit %d] %s", curl_exit,
                     erlen > 0 ? errbuf : "(no stderr captured)");
            _stream_out_send_tagged(so.target, "stream_chunk", so.name, emsg);
        } else {
            fprintf(stdout, "\n  \x1b[38;5;208m⚠ curl exited %d\x1b[0m\n", curl_exit);
            if (erlen > 0) {
                fprintf(stdout, "  \x1b[38;5;240m%s\x1b[0m\n", errbuf);
            } else {
                fprintf(stdout, "  \x1b[38;5;240m(no stderr captured — check the URL/endpoint)\x1b[0m\n");
            }
            fflush(stdout);
        }
    }
    swbs_unlink(err_file);

    /* Append a marker the model will see in history if the turn was
     * cut short. Three cases:
     *   1. User hit ESC/Ctrl+C  → "[Request interrupted by user]"
     *   2. curl timed out (28)  → "[Response cut off by transport timeout]"
     *   3. finish_reason="length" → "[Response truncated at max_tokens limit — increase max_tokens and retry]"
     * Claude Code uses similar markers so the model knows the
     * previous response was incomplete. */
    const char *trunc_marker = NULL;
    if (interrupted) {
        trunc_marker = "\n\n[Request interrupted by user]";
    } else if (curl_exit == 28) {
        trunc_marker = "\n\n[Response cut off by transport timeout after 30 minutes — "
                       "the generation was still in progress. Retry with a more targeted "
                       "request or split the work across turns.]";
    } else if (strcmp(finish_reason, "length") == 0) {
        trunc_marker = "\n\n[Response truncated at max_tokens output limit. "
                       "Retry with SWARM_CODE_MAX_OUTPUT_TOKENS set higher, or break "
                       "the work into smaller pieces.]";
    }
    if (trunc_marker) {
        size_t ml = strlen(trunc_marker);
        if (buf_len + ml + 1 > buf_cap) {
            while (buf_len + ml + 1 > buf_cap) buf_cap *= 2;
            buffer = (char *)realloc(buffer, buf_cap);
        }
        memcpy(buffer + buf_len, trunc_marker, ml);
        buf_len += ml;
        buffer[buf_len] = '\0';
        /* Also show it on screen so the user can see it happened.
         * In subagent mode, surface as a chunk so the parent can render. */
        if (so.subagent) {
            _stream_out_send_tagged(so.target, "stream_chunk", so.name, trunc_marker);
        } else if (is_tty && !interrupted) {
            fprintf(stdout, "\n  \x1b[38;5;208m⚠%s\x1b[0m\n", trunc_marker + 2);
            fflush(stdout);
        }
    }

    /* Wrap the accumulated content in a minimal OpenAI-shaped response so
     * the existing extract_content path just works. Need to JSON-encode
     * the content string (escape quotes, backslashes, newlines). */
    size_t enc_cap = buf_len * 2 + 256;
    char *enc = (char *)malloc(enc_cap);
    size_t ep = 0;
    for (size_t i = 0; i < buf_len && ep < enc_cap - 8; i++) {
        char c = buffer[i];
        switch (c) {
            case '"':  enc[ep++] = '\\'; enc[ep++] = '"'; break;
            case '\\': enc[ep++] = '\\'; enc[ep++] = '\\'; break;
            case '\n': enc[ep++] = '\\'; enc[ep++] = 'n'; break;
            case '\r': enc[ep++] = '\\'; enc[ep++] = 'r'; break;
            case '\t': enc[ep++] = '\\'; enc[ep++] = 't'; break;
            default:
                if ((unsigned char)c < 0x20) {
                    ep += snprintf(enc + ep, enc_cap - ep, "\\u%04x", (unsigned char)c);
                } else {
                    enc[ep++] = c;
                }
        }
    }
    enc[ep] = '\0';

    /* JSON-encode reasoning so the agent layer can read it back. Same
     * escape rules as content. We keep them in separate fields so the
     * agent's tool-call extractor never scans reasoning text. */
    size_t renc_cap = reason_len * 2 + 256;
    char *renc = (char *)malloc(renc_cap);
    size_t rep = 0;
    for (size_t i = 0; i < reason_len && rep < renc_cap - 8; i++) {
        char c = reasoning[i];
        switch (c) {
            case '"':  renc[rep++] = '\\'; renc[rep++] = '"'; break;
            case '\\': renc[rep++] = '\\'; renc[rep++] = '\\'; break;
            case '\n': renc[rep++] = '\\'; renc[rep++] = 'n'; break;
            case '\r': renc[rep++] = '\\'; renc[rep++] = 'r'; break;
            case '\t': renc[rep++] = '\\'; renc[rep++] = 't'; break;
            default:
                if ((unsigned char)c < 0x20) {
                    rep += snprintf(renc + rep, renc_cap - rep, "\\u%04x", (unsigned char)c);
                } else {
                    renc[rep++] = c;
                }
        }
    }
    renc[rep] = '\0';

    size_t out_cap = ep + rep + 512;
    char *out = (char *)malloc(out_cap);
    /* Include the usage block so sw callers can read real token counts
     * from the server. Missing values (no usage chunk seen) are omitted
     * rather than sent as 0 to distinguish "unknown" from "zero". */
    if (prompt_tokens >= 0) {
        snprintf(out, out_cap,
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"%s\",\"reasoning_content\":\"%s\"}}],"
            "\"usage\":{\"prompt_tokens\":%lld,\"completion_tokens\":%lld,\"total_tokens\":%lld}}",
            enc, renc,
            prompt_tokens,
            completion_tokens >= 0 ? completion_tokens : 0,
            total_tokens >= 0 ? total_tokens : prompt_tokens + (completion_tokens >= 0 ? completion_tokens : 0));
    } else {
        snprintf(out, out_cap,
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"%s\",\"reasoning_content\":\"%s\"}}]}",
            enc, renc);
    }

    sw_val_t *result = sw_val_string(out);
    free(enc);
    free(renc);
    free(reasoning);
    free(out);
    free(buffer);

    /* Tell the parent we're done — useful as a sentinel without having
     * to grep the chunk stream. */
    if (so.subagent) {
        sw_val_t *items[2];
        items[0] = sw_val_atom("stream_done");
        items[1] = sw_val_string(so.name);
        sw_val_t *msg = sw_val_tuple(items, 2);
        sw_send_tagged(so.target, SW_TAG_NONE, msg);
    }
    return result;
}

/* === Supervisor === */

typedef struct {
    sw_val_t *fn;
} _sup_child_closure_t;

static void _sup_child_entry(void *raw) {
    _sup_child_closure_t *c = (_sup_child_closure_t *)raw;
    sw_val_apply(c->fn, NULL, 0);
}

/*
 * supervise(strategy_atom, children_list)
 *   strategy: :one_for_one | :one_for_all | :rest_for_one
 *   children: [{:name, fun() { ... }, :permanent | :temporary | :transient}, ...]
 * Returns: pid of the supervisor
 */
static sw_val_t *_builtin_supervise(sw_val_t **a, int n) {
    if (n < 2) return sw_val_nil();
    sw_val_t *strat_val = a[0];
    sw_val_t *children = a[1];
    if (!children || children->type != SW_VAL_LIST) return sw_val_nil();

    sw_restart_strategy_t strat = SW_ONE_FOR_ONE;
    if (strat_val->type == SW_VAL_ATOM) {
        if (strcmp(strat_val->v.str, "one_for_all") == 0) strat = SW_ONE_FOR_ALL;
        else if (strcmp(strat_val->v.str, "rest_for_one") == 0) strat = SW_REST_FOR_ONE;
    }

    int nchildren = children->v.tuple.count;
    if (nchildren <= 0 || nchildren > 64) return sw_val_nil();
    sw_child_spec_t *specs = (sw_child_spec_t *)calloc(nchildren, sizeof(sw_child_spec_t));
    int valid = 0;

    for (int i = 0; i < nchildren; i++) {
        sw_val_t *child = children->v.tuple.items[i];
        if (child->type != SW_VAL_TUPLE || child->v.tuple.count < 3) continue;

        sw_val_t *name_v = child->v.tuple.items[0];
        sw_val_t *fn_v = child->v.tuple.items[1];
        sw_val_t *restart_v = child->v.tuple.items[2];

        if (name_v->type == SW_VAL_ATOM || name_v->type == SW_VAL_STRING)
            strncpy(specs[valid].name, name_v->v.str, 63);
        else
            snprintf(specs[valid].name, 63, "child_%d", i);

        _sup_child_closure_t *c = (_sup_child_closure_t *)malloc(sizeof(_sup_child_closure_t));
        c->fn = fn_v;
        specs[valid].start_func = _sup_child_entry;
        specs[valid].start_arg = c;
        specs[valid].restart = SW_PERMANENT;

        if (restart_v->type == SW_VAL_ATOM) {
            if (strcmp(restart_v->v.str, "temporary") == 0) specs[valid].restart = SW_TEMPORARY;
            else if (strcmp(restart_v->v.str, "transient") == 0) specs[valid].restart = SW_TRANSIENT;
        }
        valid++;
    }

    sw_sup_spec_t sup_spec;
    memset(&sup_spec, 0, sizeof(sup_spec));
    sup_spec.strategy = strat;
    sup_spec.max_restarts = 3;
    sup_spec.max_seconds = 5;
    sup_spec.children = specs;
    sup_spec.num_children = valid;

    sw_process_t *sup = sw_supervisor_start("sw_sup", &sup_spec);
    /* specs not freed — supervisor holds a shallow copy of the pointer */
    return sup ? sw_val_pid(sup) : sw_val_nil();
}

/* === Distributed Nodes === */

#include "swarmrt_node.h"

/* node_start(name, port) → 'ok' | 'error' */
static sw_val_t *_builtin_node_start(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING)
        return sw_val_atom("error");
    int port = (a[1]->type == SW_VAL_INT) ? (int)a[1]->v.i : 0;
    int ok = sw_node_start(a[0]->v.str, (uint16_t)port);
    return sw_val_atom(ok == 0 ? "ok" : "error");
}

/* node_stop() → 'ok' */
static sw_val_t *_builtin_node_stop(sw_val_t **a, int n) {
    (void)a; (void)n;
    sw_node_stop();
    return sw_val_atom("ok");
}

/* node_name() → string */
static sw_val_t *_builtin_node_name(sw_val_t **a, int n) {
    (void)a; (void)n;
    const char *name = sw_node_name();
    return name ? sw_val_string(name) : sw_val_nil();
}

/* node_connect(name, host, port) → 'ok' | 'error' */
static sw_val_t *_builtin_node_connect(sw_val_t **a, int n) {
    if (n < 3) return sw_val_atom("error");
    if (a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_STRING ||
        a[2]->type != SW_VAL_INT) return sw_val_atom("error");
    int ok = sw_node_connect(a[0]->v.str, a[1]->v.str, (uint16_t)a[2]->v.i);
    return sw_val_atom(ok == 0 ? "ok" : "error");
}

/* node_disconnect(name) → 'ok' | 'error' */
static sw_val_t *_builtin_node_disconnect(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_STRING) return sw_val_atom("error");
    int ok = sw_node_disconnect(a[0]->v.str);
    return sw_val_atom(ok == 0 ? "ok" : "error");
}

/* node_is_connected(name) → 'true' | 'false' */
static sw_val_t *_builtin_node_is_connected(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_STRING) return sw_val_atom("false");
    return sw_val_atom(sw_node_is_connected(a[0]->v.str) ? "true" : "false");
}

/* node_peers() → list of strings */
static sw_val_t *_builtin_node_peers(sw_val_t **a, int n) {
    (void)a; (void)n;
    char names[SW_NODE_MAX_PEERS][SW_NODE_NAME_MAX];
    int count = sw_node_peers(names, SW_NODE_MAX_PEERS);
    sw_val_t **items = malloc(sizeof(sw_val_t *) * (count > 0 ? count : 1));
    for (int i = 0; i < count; i++)
        items[i] = sw_val_string(names[i]);
    sw_val_t *result = sw_val_list(items, count);
    free(items);
    return result;
}

/* Forward declare JSON encoder (defined in Phase 13 section below) */
static void _json_encode_val(sw_val_t *v, char **buf, size_t *cap, size_t *pos);

/* node_send(node_name, reg_name, msg) → 'ok' | 'error'
 * Serializes msg sw_val_t to JSON and sends via sw_node_send. The
 * encode buffer grows automatically — large messages no longer
 * silently truncate at 256KB. */
static sw_val_t *_builtin_node_send(sw_val_t **a, int n) {
    if (n < 3) return sw_val_atom("error");
    if (a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_STRING)
        return sw_val_atom("error");
    /* Type-preserving binary marshal — see sw_marshal in swarmrt_node.c.
     * Replaces the JSON path that lost tuple/atom semantics on the
     * round-trip and broke any send pattern more structured than a
     * single string. */
    uint8_t *buf = NULL;
    uint32_t blen = 0;
    if (sw_marshal(a[2], &buf, &blen) < 0) {
        if (buf) free(buf);
        return sw_val_atom("error");
    }
    int ok = sw_node_send(a[0]->v.str, a[1]->v.str, SW_TAG_NONE,
                          buf, blen);
    free(buf);
    return sw_val_atom(ok == 0 ? "ok" : "error");
}

/* === Map Builtins === */

/* === Process Introspection === */

/* process_info(pid) → map with process details */
static sw_val_t *_builtin_process_info(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_PID || !a[0]->v.pid)
        return sw_val_nil();
    sw_process_t *proc = a[0]->v.pid;

    sw_val_t *keys[8], *vals[8];
    int c = 0;
    keys[c] = sw_val_atom("pid");     vals[c] = sw_val_int((int64_t)proc->pid); c++;
    keys[c] = sw_val_atom("status");
    switch (proc->state) {
        case SW_PROC_RUNNING:  vals[c] = sw_val_atom("running"); break;
        case SW_PROC_RUNNABLE: vals[c] = sw_val_atom("runnable"); break;
        case SW_PROC_WAITING:  vals[c] = sw_val_atom("waiting"); break;
        case SW_PROC_EXITING:  vals[c] = sw_val_atom("exiting"); break;
        default:               vals[c] = sw_val_atom("unknown"); break;
    }
    c++;
    keys[c] = sw_val_atom("reductions");  vals[c] = sw_val_int((int64_t)proc->reductions_done); c++;
    keys[c] = sw_val_atom("messages");    vals[c] = sw_val_int((int64_t)proc->mailbox.count); c++;
    keys[c] = sw_val_atom("heap_used");   vals[c] = sw_val_int((int64_t)(proc->heap.top - proc->heap.start)); c++;
    keys[c] = sw_val_atom("heap_size");   vals[c] = sw_val_int((int64_t)proc->heap.size); c++;
    if (proc->reg_entry)
        { keys[c] = sw_val_atom("name"); vals[c] = sw_val_string(proc->reg_entry->name); c++; }

    return sw_val_map_new(keys, vals, c);
}

/* process_list() → list of pids of all alive processes */
static sw_val_t *_builtin_process_list(sw_val_t **a, int n) {
    (void)a; (void)n;
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

/* registered() → list of {name, pid} tuples for all registered processes */
static sw_val_t *_builtin_registered(sw_val_t **a, int n) {
    (void)a; (void)n;
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

/* map_new() → empty map */
static sw_val_t *_builtin_map_new(sw_val_t **a, int n) {
    (void)a; (void)n;
    return sw_val_map_new(NULL, NULL, 0);
}

/* map_get(map, key) → value or nil */
static sw_val_t *_builtin_map_get(sw_val_t **a, int n) {
    if (n < 2) return sw_val_nil();
    return sw_val_map_get(a[0], a[1]);
}

/* map_put(map, key, value) → new map with key set */
static sw_val_t *_builtin_map_put(sw_val_t **a, int n) {
    if (n < 3) return sw_val_nil();
    return sw_val_map_put(a[0], a[1], a[2]);
}

/* map_keys(map) → list of keys */
static sw_val_t *_builtin_map_keys(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_MAP) return sw_val_list(NULL, 0);
    int cnt = a[0]->v.map.count;
    if (cnt == 0) return sw_val_list(NULL, 0);
    sw_val_t **items = malloc(sizeof(sw_val_t*) * cnt);
    for (int i = 0; i < cnt; i++) items[i] = a[0]->v.map.keys[i];
    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

/* map_values(map) → list of values */
static sw_val_t *_builtin_map_values(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_MAP) return sw_val_list(NULL, 0);
    int cnt = a[0]->v.map.count;
    if (cnt == 0) return sw_val_list(NULL, 0);
    sw_val_t **items = malloc(sizeof(sw_val_t*) * cnt);
    for (int i = 0; i < cnt; i++) items[i] = a[0]->v.map.vals[i];
    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

/* map_merge(map1, map2) → new map with all keys from both */
static sw_val_t *_builtin_map_merge(sw_val_t **a, int n) {
    if (n < 2) return sw_val_nil();
    if (!a[0] || a[0]->type != SW_VAL_MAP) return (n >= 2 && a[1]) ? a[1] : sw_val_map_new(NULL, NULL, 0);
    if (!a[1] || a[1]->type != SW_VAL_MAP) return a[0];
    /* Start with a copy of map1 */
    sw_val_t *result = sw_val_map_new(a[0]->v.map.keys, a[0]->v.map.vals, a[0]->v.map.count);
    /* Add/overwrite with map2 entries */
    for (int i = 0; i < a[1]->v.map.count; i++)
        result = sw_val_map_put(result, a[1]->v.map.keys[i], a[1]->v.map.vals[i]);
    return result;
}

/* map_has_key(map, key) → 'true' | 'false'.
 *
 * Reuses sw_val_map_get for the lookup so the atom-vs-string fallback
 * matches: a map built with literal atom keys (`%{a: 1}`) returns true
 * for both `map_has_key(m, 'a')` AND `map_has_key(m, "a")`, mirroring
 * map_get's behaviour. The older direct sw_val_equal loop diverged
 * from map_get and surprised users. */
static sw_val_t *_builtin_map_has_key(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_MAP) return sw_val_atom("false");
    sw_val_t *v = sw_val_map_get(a[0], a[1]);
    return (v && v->type != SW_VAL_NIL) ? sw_val_atom("true") : sw_val_atom("false");
}

/* map_size(map) → int. Length on a map already returns 0; this gives
 * users the obviously-named query they reach for. */
static sw_val_t *_builtin_map_size(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_MAP) return sw_val_int(0);
    return sw_val_int(a[0]->v.map.count);
}

/* map_remove(map, key) → new map without key. Returns the original
 * map if the key isn't present (no allocation). */
static sw_val_t *_builtin_map_remove(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_MAP) return n >= 1 ? a[0] : sw_val_nil();
    sw_val_t *m = a[0];
    int idx = -1;
    for (int i = 0; i < m->v.map.count; i++) {
        if (sw_val_equal(m->v.map.keys[i], a[1])) { idx = i; break; }
    }
    if (idx < 0) return m; /* key not present — nothing to do */
    int new_cnt = m->v.map.count - 1;
    if (new_cnt == 0) return sw_val_map_new(NULL, NULL, 0);
    sw_val_t **k = malloc(sizeof(sw_val_t*) * new_cnt);
    sw_val_t **v = malloc(sizeof(sw_val_t*) * new_cnt);
    int j = 0;
    for (int i = 0; i < m->v.map.count; i++) {
        if (i == idx) continue;
        k[j] = m->v.map.keys[i];
        v[j] = m->v.map.vals[i];
        j++;
    }
    sw_val_t *r = sw_val_map_new(k, v, new_cnt);
    free(k); free(v);
    return r;
}

/* format(template, args...) → string.
 *
 * Like Python's `"hello {}".format(name)` but variadic at the call site.
 * Each `{}` placeholder consumes the next positional arg. `{{` and `}}`
 * escape literal braces. Composite values (tuples, lists, maps, pids)
 * render via sw_val_format — same shape as `print` produces — so the
 * common "stitch a log line" use becomes:
 *
 *     print(format("[{}] req={} ms={}", level, req_id, elapsed_ms))
 *
 * instead of `"[" ++ to_string(level) ++ "] req=" ++ ... ++ ...`. */
static sw_val_t *_builtin_format(sw_val_t **a, int n) {
    if (n < 1 || a[0]->type != SW_VAL_STRING) return sw_val_string("");
    const char *tpl = a[0]->v.str;
    char *buf = NULL;
    size_t blen = 0;
    FILE *m = open_memstream(&buf, &blen);
    if (!m) return sw_val_string("");
    int arg_idx = 1;
    for (const char *p = tpl; *p; ) {
        if (p[0] == '{' && p[1] == '}') {
            if (arg_idx < n) {
                sw_val_format(m, a[arg_idx++]);
            } else {
                fputs("{}", m);  /* not enough args — leave the marker visible */
            }
            p += 2;
        } else if (p[0] == '{' && p[1] == '{') {
            fputc('{', m); p += 2;
        } else if (p[0] == '}' && p[1] == '}') {
            fputc('}', m); p += 2;
        } else {
            fputc(*p, m); p++;
        }
    }
    fclose(m);
    sw_val_t *r = sw_val_string(buf ? buf : "");
    free(buf);
    return r;
}

/* === Error mechanism for try/catch === */

/* error(reason) — sets thread-local error, caught by try/catch.
 * Silent if no try/catch is wrapping the call site. Pair this with
 * `try { … } catch e { … }` for recoverable failures.
 *
 * For UNRECOVERABLE failures (programmer bug, invariant violated,
 * impossible state) use panic(msg) below — it prints and exits.
 */
static sw_val_t *_builtin_error(sw_val_t **a, int n) {
    extern __thread sw_val_t *_sw_error;
    _sw_error = (n >= 1) ? a[0] : sw_val_string("error");
    return sw_val_nil();
}

/* Print the current call stack to stderr, top of stack first. Reads
 * the per-thread ring buffer the codegen maintains via
 * _sw_trace_push / _sw_trace_pop. Static — included by the generated
 * C, which is the only place the trace globals are defined. */
typedef struct { const char *module_name; const char *fn_name; int line; } _sw_frame_t;
extern __thread _sw_frame_t _sw_trace[];
extern __thread int _sw_trace_top;
extern __thread int _sw_trace_overflowed;

static void _sw_print_trace(void) {
    extern __thread int _sw_current_line;
    extern __thread const char *_sw_current_file;
    int top = _sw_trace_top;
    if (top <= 0) return;
    if (top > 64) top = 64;
    fprintf(stderr, "  call chain (innermost first):\n");
    /* The innermost frame's "current line" lives in _sw_current_line —
     * the frame's stored `line` is the function's *entry* line. */
    for (int i = top - 1; i >= 0; i--) {
        const char *m = _sw_trace[i].module_name ? _sw_trace[i].module_name : "?";
        const char *fn = _sw_trace[i].fn_name ? _sw_trace[i].fn_name : "?";
        int line = (i == top - 1 && _sw_current_line > 0)
                       ? _sw_current_line : _sw_trace[i].line;
        fprintf(stderr, "    [%d] %s.%s at src/%s.sw:%d\n", top - 1 - i, m, fn, m, line);
    }
    if (_sw_trace_overflowed)
        fprintf(stderr, "    ... (truncated; %d frames omitted)\n", _sw_trace_top - 64);
}

/* C-level panic helper for builtins. Takes a printf-style format so
 * we can include context ("hd of empty list", "tuple has 3 elements,
 * asked for index 5") without allocating an sw_val_t. Reads the
 * runtime line/file trackers the codegen keeps current. NORETURN. */
__attribute__((noreturn))
static void _sw_runtime_panic(const char *fmt, ...) {
    extern __thread int _sw_current_line;
    extern __thread const char *_sw_current_file;
    /* Format the message twice: once for stderr (with the ANSI banner
     * and call chain) and once into a clean buffer that we hand to
     * sw_process_panic so it lands in EXIT/DOWN messages. */
    char reason_buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason_buf, sizeof(reason_buf), fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n\x1b[1;31mpanic\x1b[0m: %s\n", reason_buf);
    if (_sw_current_file && _sw_current_line > 0)
        fprintf(stderr, "  at %s:%d\n", _sw_current_file, _sw_current_line);
    _sw_print_trace();
    fflush(stderr);
    /* Panic the CURRENT sw process, not the whole binary. The scheduler
     * tears down our process slot and propagates EXIT signals (carrying
     * `reason_buf`) to linked processes — so supervisors, link, monitor,
     * trap_exit all work as documented. exit(1) is the fallback only for
     * panics outside any sw process (e.g. from the C main thread). */
    sw_process_t *me = sw_self();
    if (me) sw_process_panic(me, -1, reason_buf);
    exit(1);
}

/* panic(msg) — print "panic: msg at FILE:LINE" to stderr and exit(1).
 * Cannot be caught. Use for programmer bugs (nil where a value was
 * needed, list empty when it shouldn't be, impossible cases in a
 * case expression). For recoverable conditions reach for error()
 * + try/catch instead. */
static sw_val_t *_builtin_panic(sw_val_t **a, int n) {
    extern __thread int _sw_current_line;
    extern __thread const char *_sw_current_file;
    char msg_buf[512];
    if (n >= 1 && a[0]) {
        char *b = NULL; size_t bl = 0;
        FILE *m = open_memstream(&b, &bl);
        if (m) { sw_val_format(m, a[0]); fclose(m); }
        snprintf(msg_buf, sizeof(msg_buf), "%s", b ? b : "(no message)");
        free(b);
    } else {
        snprintf(msg_buf, sizeof(msg_buf), "(no message)");
    }
    fprintf(stderr, "\n\x1b[1;31mpanic\x1b[0m: %s\n", msg_buf);
    if (_sw_current_file && _sw_current_line > 0)
        fprintf(stderr, "  at %s:%d\n", _sw_current_file, _sw_current_line);
    _sw_print_trace();
    fflush(stderr);
    /* Per-process panic — pass msg_buf as the reason so trap_exit
     * handlers receive the user's message in {'EXIT', from, msg}. */
    sw_process_t *me = sw_self();
    if (me) sw_process_panic(me, -1, msg_buf);
    exit(1);
}

/* expect(value, msg) — returns value if non-nil; otherwise panics
 * with msg. The idiomatic "unwrap" pattern:
 *
 *   name = expect(map_get(user, 'name'), "user has no name field")
 *
 * Saves the explicit `if (x == nil) { panic("...") }` wrapper.
 *
 * The literal `nil` lexes to atom 'nil', so we also treat that as
 * the absent value here — otherwise `expect(nil, ...)` returns the
 * atom 'nil' instead of panicking, defeating the whole point of
 * expect for callers who literally wrote `nil`. sw_val_is_truthy
 * uses the same equivalence. */
static sw_val_t *_builtin_expect(sw_val_t **a, int n) {
    if (n < 1) {
        sw_val_t *msg = sw_val_string("expect: missing value argument");
        sw_val_t *args[1] = { msg };
        return _builtin_panic(args, 1);
    }
    int is_nil = (!a[0] || a[0]->type == SW_VAL_NIL ||
                  (a[0]->type == SW_VAL_ATOM && a[0]->v.str &&
                   strcmp(a[0]->v.str, "nil") == 0));
    if (!is_nil) return a[0];
    sw_val_t *msg = (n >= 2) ? a[1] : sw_val_string("expected non-nil value, got nil");
    sw_val_t *args[1] = { msg };
    return _builtin_panic(args, 1);
}

/* ============================================================
 * Process lifecycle — link / monitor / exit / trap_exit
 * ============================================================
 *
 * Erlang-grade fault tolerance from userland sw. All wrappers
 * around the existing runtime APIs:
 *
 *   link(pid)              → 'ok' | 'error'
 *   unlink(pid)            → 'ok' | 'error'
 *   monitor(pid)           → reference (int) — 0 on failure
 *   demonitor(ref)         → 'ok' | 'error'
 *   exit_proc(pid, reason) → 'ok'  — kills pid with a reason atom
 *                                   (named exit_proc to avoid collision
 *                                    with `exit` / `sys_exit` shorthand)
 *   trap_exit('true'|'false') → 'ok' — toggle exit-signal trapping
 *                                      on the current process
 *
 * With trap_exit on, exit signals arrive as `{'EXIT', from_pid, reason}`
 * messages instead of killing the receiver — the canonical pattern
 * for a supervisor written in sw.
 */
/* link() and monitor() already exist near top of file. We just add
 * the missing pieces here: unlink, demonitor, exit_proc, trap_exit. */
static sw_val_t *_builtin_unlink(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_PID) return sw_val_atom("error");
    return sw_val_atom(sw_unlink(a[0]->v.pid) == 0 ? "ok" : "error");
}

static sw_val_t *_builtin_demonitor(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    return sw_val_atom(sw_demonitor((uint64_t)a[0]->v.i) == 0 ? "ok" : "error");
}

static sw_val_t *_builtin_exit_proc(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_PID) return sw_val_atom("error");
    int reason = 2;  /* default: 'killed' */
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_ATOM) {
        if (strcmp(a[1]->v.str, "normal") == 0) reason = 0;
        else if (strcmp(a[1]->v.str, "killed") == 0) reason = 2;
        else reason = 3;
    }
    sw_process_kill(a[0]->v.pid, reason);
    return sw_val_atom("ok");
}

static sw_val_t *_builtin_trap_exit(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_atom("error");
    int v = 0;
    if (a[0]->type == SW_VAL_ATOM && strcmp(a[0]->v.str, "true") == 0) v = 1;
    else if (a[0]->type == SW_VAL_INT && a[0]->v.i != 0) v = 1;
    sw_process_flag(SW_FLAG_TRAP_EXIT, v);
    return sw_val_atom("ok");
}

/* ============================================================
 * SQLite — embedded database for agent state + memory
 * ============================================================
 *
 * Builtins:
 *   db_open(path)           → handle (int) | -1
 *   db_exec(h, sql)         → 'ok' | error_string  (DDL or 0-result stmts)
 *   db_query(h, sql, [args]) → list of %{col: val, ...} rows
 *   db_close(h)             → 'ok' | 'error'
 *
 * Parameter binding uses `?` placeholders. Args list must match
 * placeholder count. Ints, floats, strings, atoms, nil are bound to
 * matching sqlite types; everything else is coerced to string.
 *
 * Linked against system libsqlite3. Connection cap = 32.
 */
#define _SW_SQLITE_MAX 32
static sqlite3 *_sw_sqlite_db[_SW_SQLITE_MAX];

static sw_val_t *_builtin_db_open(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_int(-1);
    int slot = -1;
    for (int i = 0; i < _SW_SQLITE_MAX; i++) if (!_sw_sqlite_db[i]) { slot = i; break; }
    if (slot < 0) return sw_val_int(-1);
    sqlite3 *db = NULL;
    if (sqlite3_open(a[0]->v.str, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return sw_val_int(-1);
    }
    _sw_sqlite_db[slot] = db;
    return sw_val_int(slot);
}

static sw_val_t *_builtin_db_close(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    int slot = (int)a[0]->v.i;
    if (slot < 0 || slot >= _SW_SQLITE_MAX || !_sw_sqlite_db[slot]) return sw_val_atom("error");
    sqlite3_close(_sw_sqlite_db[slot]);
    _sw_sqlite_db[slot] = NULL;
    return sw_val_atom("ok");
}

/* Bind a single arg to a prepared sqlite stmt at 1-based index. */
static int _sw_db_bind(sqlite3_stmt *stmt, int idx, sw_val_t *v) {
    if (!v || v->type == SW_VAL_NIL) return sqlite3_bind_null(stmt, idx);
    switch (v->type) {
        case SW_VAL_INT:    return sqlite3_bind_int64(stmt, idx, v->v.i);
        case SW_VAL_FLOAT:  return sqlite3_bind_double(stmt, idx, v->v.f);
        case SW_VAL_STRING: return sqlite3_bind_text(stmt, idx, v->v.str, -1, SQLITE_TRANSIENT);
        case SW_VAL_ATOM:   return sqlite3_bind_text(stmt, idx, v->v.str, -1, SQLITE_TRANSIENT);
        default: {
            /* Fall back: render via sw_val_format to a string. */
            char *buf = NULL; size_t blen = 0;
            FILE *m = open_memstream(&buf, &blen);
            if (m) { sw_val_format(m, v); fclose(m); }
            int rc = sqlite3_bind_text(stmt, idx, buf ? buf : "", -1, SQLITE_TRANSIENT);
            free(buf);
            return rc;
        }
    }
}

/* Take one row's columns and build a sw map %{col_name: value}. */
static sw_val_t *_sw_db_row_to_map(sqlite3_stmt *stmt) {
    int ncols = sqlite3_column_count(stmt);
    sw_val_t **keys = (sw_val_t **)malloc(sizeof(sw_val_t *) * ncols);
    sw_val_t **vals = (sw_val_t **)malloc(sizeof(sw_val_t *) * ncols);
    for (int i = 0; i < ncols; i++) {
        keys[i] = sw_val_string(sqlite3_column_name(stmt, i));
        switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_INTEGER:
                vals[i] = sw_val_int(sqlite3_column_int64(stmt, i)); break;
            case SQLITE_FLOAT:
                vals[i] = sw_val_float(sqlite3_column_double(stmt, i)); break;
            case SQLITE_TEXT:
                vals[i] = sw_val_string((const char *)sqlite3_column_text(stmt, i)); break;
            case SQLITE_BLOB: {
                /* Render blobs as base64-ish hex strings — agents rarely
                 * need raw bytes. If someone does, we add a db_query_blob
                 * variant later. */
                int sz = sqlite3_column_bytes(stmt, i);
                const unsigned char *bytes = (const unsigned char *)sqlite3_column_blob(stmt, i);
                char *hex = (char *)malloc(sz * 2 + 1);
                for (int j = 0; j < sz; j++) sprintf(hex + j * 2, "%02x", bytes[j]);
                hex[sz * 2] = '\0';
                vals[i] = sw_val_string(hex);
                free(hex);
                break;
            }
            case SQLITE_NULL:
            default:
                vals[i] = sw_val_nil(); break;
        }
    }
    sw_val_t *m = sw_val_map_new(keys, vals, ncols);
    free(keys); free(vals);
    return m;
}

static sw_val_t *_builtin_db_exec(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT || !a[1] || a[1]->type != SW_VAL_STRING)
        return sw_val_atom("error");
    int slot = (int)a[0]->v.i;
    if (slot < 0 || slot >= _SW_SQLITE_MAX || !_sw_sqlite_db[slot]) return sw_val_atom("error");
    char *err = NULL;
    int rc = sqlite3_exec(_sw_sqlite_db[slot], a[1]->v.str, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sw_val_t *r = sw_val_string(err ? err : "sqlite error");
        if (err) sqlite3_free(err);
        return r;
    }
    return sw_val_atom("ok");
}

static sw_val_t *_builtin_db_query(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT || !a[1] || a[1]->type != SW_VAL_STRING)
        return sw_val_list(NULL, 0);
    int slot = (int)a[0]->v.i;
    if (slot < 0 || slot >= _SW_SQLITE_MAX || !_sw_sqlite_db[slot]) return sw_val_list(NULL, 0);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(_sw_sqlite_db[slot], a[1]->v.str, -1, &stmt, NULL) != SQLITE_OK)
        return sw_val_list(NULL, 0);

    if (n >= 3 && a[2] && a[2]->type == SW_VAL_LIST) {
        for (int i = 0; i < a[2]->v.tuple.count; i++)
            _sw_db_bind(stmt, i + 1, a[2]->v.tuple.items[i]);
    }

    sw_val_t **rows = NULL;
    int nrows = 0, rows_cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (nrows == rows_cap) {
            rows_cap = rows_cap ? rows_cap * 2 : 16;
            rows = (sw_val_t **)realloc(rows, sizeof(sw_val_t *) * rows_cap);
        }
        rows[nrows++] = _sw_db_row_to_map(stmt);
    }
    sqlite3_finalize(stmt);
    sw_val_t *r = sw_val_list(rows, nrows);
    free(rows);
    return r;
}

/* ================================================================
 * Phase 13: Agent Stdlib Batteries
 *
 * http_get, shell, json_encode, json_decode, file_exists, file_list,
 * file_delete, after, every, llm_complete, string_split, string_trim,
 * string_upper, string_lower, string_starts_with, string_ends_with
 * ================================================================ */

/* === HTTP GET with retry === */

/* http_get — read full response into a growing buffer (no silent
 * truncation at 512KB anymore). Streams from the temp file in 64KB
 * chunks, doubling the response buffer as needed. */
static sw_val_t *_builtin_http_get(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *url = a[0]->v.str;

    size_t cmdcap = strlen(url) + 4096;
    char *cmd = (char *)malloc(cmdcap);
    char outf[256];
    snprintf(outf, sizeof(outf), "%s/sw_http_out_%d_%u.json",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());

    int off = 0;
    off += snprintf(cmd + off, cmdcap - off,
        "curl -sS --connect-timeout 30 --max-time 120");

    if (n >= 2 && a[1] && a[1]->type == SW_VAL_LIST) {
        for (int i = 0; i < a[1]->v.tuple.count; i++) {
            sw_val_t *h = a[1]->v.tuple.items[i];
            if (h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->v.str && h->v.tuple.items[1]->v.str)
                off += snprintf(cmd + off, cmdcap - off, " -H '%s: %s'",
                    h->v.tuple.items[0]->v.str, h->v.tuple.items[1]->v.str);
        }
    }
    off += snprintf(cmd + off, cmdcap - off, " '%s' -o %s 2>/dev/null", url, outf);

    char *resp = NULL;
    size_t resp_len = 0;
    int delays[] = {0, 3, 10};
    for (int attempt = 0; attempt < 3; attempt++) {
        if (delays[attempt] > 0) sw_sleep(delays[attempt]);
        system(cmd);
        FILE *fp = fopen(outf, "r");
        if (!fp) continue;
        size_t cap = 65536;
        if (resp) free(resp);
        resp = (char *)malloc(cap);
        resp_len = 0;
        char chunk[8192];
        size_t rd;
        while ((rd = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
            if (resp_len + rd + 1 > cap) {
                while (resp_len + rd + 1 > cap) cap *= 2;
                resp = (char *)realloc(resp, cap);
            }
            memcpy(resp + resp_len, chunk, rd);
            resp_len += rd;
        }
        resp[resp_len] = 0;
        fclose(fp);
        if (resp_len > 0) { swbs_unlink(outf); break; }
    }
    swbs_unlink(outf);
    free(cmd);
    if (!resp) return sw_val_string("");
    sw_val_t *r = sw_val_string(resp);
    free(resp);
    return r;
}

/* === Shell: run command, capture stdout === */
/* Uses system() + file redirect instead of popen() to avoid fork() in a
 * multi-threaded process.  popen() calls fork() which duplicates only the
 * calling thread — any mutex held by a scheduler thread at that instant
 * is permanently locked in the child, corrupting the malloc allocator and
 * causing segfaults in the parent on macOS.  system() uses posix_spawn on
 * modern macOS, sidestepping the issue entirely. */

/* shell_sandboxed(cmd, opts) — like shell() but the child is wrapped
 * in an OS-level sandbox. Defaults are restrictive: network blocked,
 * filesystem read-only outside /tmp + the cwd.
 *
 * Platform support:
 *   macOS — sandbox-exec with a generated `.sb` profile
 *   Linux — firejail (if installed)
 *
 * If sandboxing isn't available, returns nil rather than silently
 * running the un-sandboxed command — agents that asked for a
 * sandbox shouldn't quietly get full access.
 *
 * opts is currently a placeholder for future knobs (allow_net,
 * extra_read_paths, cpu_seconds…). Pass nil for the default policy.
 */
static sw_val_t *_builtin_shell_sandboxed(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    const char *cmd = a[0]->v.str;
    (void)n; /* opts arg reserved for future knobs */

#ifdef __APPLE__
    /* Write a minimal sandbox-exec profile. Restrictive by default:
     * - no network
     * - read-only outside /tmp + /private/tmp + standard system dirs
     * - no writes outside /tmp + /private/tmp
     */
    char profile_path[256];
    snprintf(profile_path, sizeof(profile_path), "%s/sw_sandbox_%d_%u.sb",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    FILE *pf = fopen(profile_path, "w");
    if (!pf) return sw_val_nil();
    /* Permissive-but-network-blocked profile. macOS dyld + libc need
     * a surprising amount of access to even load `sh`, so a strict
     * "deny default" profile aborts with SIGABRT before the user's
     * command runs. Instead we "allow default" then selectively
     * deny network + writes outside /tmp. Future opts can tighten. */
    fprintf(pf,
        "(version 1)\n"
        "(allow default)\n"
        "(deny network*)\n"
        "(deny file-write* (subpath \"/Users\") (subpath \"/etc\")\n"
        "                  (subpath \"/usr\") (subpath \"/bin\")\n"
        "                  (subpath \"/sbin\") (subpath \"/Library\"))\n"
        "(allow file-write* (subpath \"/tmp\") (subpath \"/private/tmp\"))\n"
    );
    fclose(pf);

    /* Capture output via popen — easier than the tmp-file dance of shell(). */
    size_t cmdlen = strlen(cmd) + strlen(profile_path) + 256;
    char *full = (char *)malloc(cmdlen);
    snprintf(full, cmdlen, "sandbox-exec -f %s /bin/sh -c %c%s%c 2>&1",
             profile_path, '\'', cmd, '\'');
    FILE *fp = popen(full, "r");
    free(full);
    if (!fp) { swbs_unlink(profile_path); return sw_val_nil(); }
    /* sw processes have small per-process stacks; allocate the read
     * buffer on the heap instead of using a 64 KB stack array. */
    size_t out_cap = 65536;
    char *outbuf = (char *)malloc(out_cap);
    if (!outbuf) { pclose(fp); swbs_unlink(profile_path); return sw_val_nil(); }
    size_t got = fread(outbuf, 1, out_cap - 1, fp);
    outbuf[got] = '\0';
    int status = pclose(fp);
    swbs_unlink(profile_path);

    sw_val_t *items[2];
    items[0] = sw_val_int(WEXITSTATUS(status));
    items[1] = sw_val_string(outbuf);
    free(outbuf);
    return sw_val_tuple(items, 2);
#elif !defined(_WIN32)
    /* Linux — try firejail. If absent, return nil (don't silently
     * fall back to un-sandboxed). */
    if (system("command -v firejail >/dev/null 2>&1") != 0) {
        return sw_val_nil();
    }
    size_t cmdlen = strlen(cmd) + 256;
    char *full = (char *)malloc(cmdlen);
    snprintf(full, cmdlen,
        "firejail --quiet --net=none --private-tmp -- /bin/sh -c %c%s%c 2>&1",
        '\'', cmd, '\'');
    FILE *fp = popen(full, "r");
    free(full);
    if (!fp) return sw_val_nil();
    size_t out_cap = 65536;
    char *outbuf = (char *)malloc(out_cap);
    if (!outbuf) { pclose(fp); return sw_val_nil(); }
    size_t got = fread(outbuf, 1, out_cap - 1, fp);
    outbuf[got] = '\0';
    int status = pclose(fp);
    sw_val_t *items[2];
    items[0] = sw_val_int(WEXITSTATUS(status));
    items[1] = sw_val_string(outbuf);
    free(outbuf);
    return sw_val_tuple(items, 2);
#else
    (void)cmd;
    return sw_val_nil();
#endif
}

static sw_val_t *_builtin_shell(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *cmd = a[0]->v.str;

    /* Temp files for captured output and exit status */
    char outf[256], exitf[256];
    snprintf(outf, sizeof(outf), "%s/sw_shell_%d_%u.out",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    snprintf(exitf, sizeof(exitf), "%s/sw_shell_%d_%u.exit",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());

    /* Launch command in background, writing exit code to a file when done.
     * This lets us poll the output file while the command runs. */
    size_t cmdlen = strlen(cmd);
    size_t wrapcap = cmdlen + 1024;
    char *wrapped = (char *)malloc(wrapcap);
    if (!wrapped) return sw_val_nil();
    snprintf(wrapped, wrapcap,
        "{ %s ; echo $? > %s ; } > %s 2>&1 &", cmd, exitf, outf);
    system(wrapped);
    free(wrapped);

    /* Poll the output file, showing a live tail while the command runs.
     * Check for the exit file every second — its presence means done.
     * Show last 2 lines of output as a progress indicator. */
    int is_tty = isatty(fileno(stdout));
    int done = 0;
    int polls = 0;
    const int max_poll_seconds = 600;  /* hard cap: 10 minutes */
    off_t last_size = 0;

    while (!done && polls < max_poll_seconds) {
        usleep(1000000);  /* 1 second */
        polls++;

        /* Check if command finished */
        FILE *ef = fopen(exitf, "r");
        if (ef) { fclose(ef); done = 1; break; }

        /* Show live tail if terminal */
        if (is_tty) {
            struct stat st;
            if (stat(outf, &st) == 0 && st.st_size > last_size) {
                last_size = st.st_size;
                /* Read last 512 bytes to extract last 2 lines */
                FILE *tf = fopen(outf, "r");
                if (tf) {
                    off_t tail_start = (st.st_size > 512) ? st.st_size - 512 : 0;
                    fseek(tf, tail_start, SEEK_SET);
                    char tail[513];
                    size_t tread = fread(tail, 1, 512, tf);
                    tail[tread] = '\0';
                    fclose(tf);

                    /* Find last 2 newlines */
                    char *last_nl = NULL, *prev_nl = NULL;
                    for (char *p = tail + tread - 1; p >= tail; p--) {
                        if (*p == '\n') {
                            if (!last_nl) { last_nl = p; }
                            else if (!prev_nl) { prev_nl = p; break; }
                        }
                    }
                    const char *display = prev_nl ? prev_nl + 1 :
                                          last_nl ? last_nl + 1 : tail;
                    /* Truncate display line to ~120 chars */
                    char line_buf[128];
                    size_t dlen = strlen(display);
                    if (dlen > 120) dlen = 120;
                    memcpy(line_buf, display, dlen);
                    line_buf[dlen] = '\0';
                    /* Strip trailing newline */
                    while (dlen > 0 && (line_buf[dlen-1]=='\n'||line_buf[dlen-1]=='\r'))
                        line_buf[--dlen] = '\0';

                    fprintf(stdout, "\r\033[K    \033[38;5;240m⎿ %s\033[0m",
                            line_buf);
                    fflush(stdout);
                }
            }
        }
    }
    /* Clear the progress line */
    if (is_tty) { fputs("\r\033[K", stdout); fflush(stdout); }

    /* Read exit code */
    int status = -1;
    {
        FILE *ef = fopen(exitf, "r");
        if (ef) {
            char ebuf[16] = {0};
            size_t er = fread(ebuf, 1, 15, ef);
            ebuf[er] = 0;
            fclose(ef);
            status = atoi(ebuf);
        }
    }
    swbs_unlink(exitf);

    /* Read full output, sanitizing non-printable bytes.
     * Binary output (gzipped pages, encrypted files, images) poisons
     * the model's context and causes empty responses.  We keep only
     * printable ASCII (0x20-0x7E) plus \t \n \r.  If the result is
     * entirely binary (sanitized length is 0), return a placeholder.
     * Buffer grows from 64KB on demand — long pages or large shell
     * outputs no longer truncate at the cap. */
    size_t cap = 65536, raw_len = 0;
    char *raw = (char *)malloc(cap);
    if (!raw) { swbs_unlink(outf); return sw_val_nil(); }
    raw[0] = 0;

    FILE *fp = fopen(outf, "r");
    if (fp) {
        char chunk[8192];
        size_t rd;
        while ((rd = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
            if (raw_len + rd + 1 > cap) {
                while (raw_len + rd + 1 > cap) cap *= 2;
                raw = (char *)realloc(raw, cap);
            }
            memcpy(raw + raw_len, chunk, rd);
            raw_len += rd;
        }
        raw[raw_len] = 0;
        fclose(fp);
    }
    swbs_unlink(outf);

    /* Sanitize: strip non-printable bytes in-place */
    size_t len = 0;
    char *buf = (char *)malloc(raw_len + 64);
    if (!buf) { free(raw); return sw_val_nil(); }
    for (size_t i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 0x20 && c <= 0x7E))
            buf[len++] = (char)c;
    }
    buf[len] = 0;
    free(raw);

    /* If all content was binary, say so */
    if (len == 0 && raw_len > 0) {
        snprintf(buf, 63, "[binary output — %zu bytes, not text]", raw_len);
        len = strlen(buf);
    }

    /* Return {status, output} tuple */
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * 2);
    if (!items) { free(buf); return sw_val_nil(); }
    items[0] = sw_val_int(status);
    items[1] = sw_val_string(buf);
    sw_val_t *r = sw_val_tuple(items, 2);
    free(buf);
    free(items);
    return r;
}

/* === JSON encode: sw_val_t → JSON string === */

static void _json_encode_val(sw_val_t *v, char **buf, size_t *cap, size_t *pos);

/* Grow *buf to fit at least `need` more bytes (plus room for NUL +
 * escape headroom). Doubles capacity until it fits. */
static void _json_grow(char **buf, size_t *cap, size_t pos, size_t need) {
    size_t want = pos + need + 8;
    if (want <= *cap) return;
    size_t new_cap = *cap;
    while (new_cap < want) new_cap *= 2;
    *buf = (char *)realloc(*buf, new_cap);
    *cap = new_cap;
}

static void _json_append(char **buf, size_t *cap, size_t *pos, const char *s) {
    size_t len = strlen(s);
    _json_grow(buf, cap, *pos, len);
    memcpy(*buf + *pos, s, len);
    *pos += len;
}

/* Single-byte append used by the inner string escape loop. */
static void _json_putc(char **buf, size_t *cap, size_t *pos, char c) {
    _json_grow(buf, cap, *pos, 1);
    (*buf)[(*pos)++] = c;
}

static void _json_encode_val(sw_val_t *v, char **buf, size_t *cap, size_t *pos) {
    if (!v || v->type == SW_VAL_NIL) {
        _json_append(buf, cap, pos, "null");
    } else if (v->type == SW_VAL_INT) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", (long long)v->v.i);
        _json_append(buf, cap, pos, tmp);
    } else if (v->type == SW_VAL_FLOAT) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.17g", v->v.f);
        _json_append(buf, cap, pos, tmp);
    } else if (v->type == SW_VAL_STRING) {
        _json_append(buf, cap, pos, "\"");
        /* Escape string contents — buffer grows as needed via _json_putc. */
        for (const char *p = v->v.str; *p; p++) {
            switch (*p) {
                case '"':  _json_append(buf, cap, pos, "\\\""); break;
                case '\\': _json_append(buf, cap, pos, "\\\\"); break;
                case '\n': _json_append(buf, cap, pos, "\\n"); break;
                case '\r': _json_append(buf, cap, pos, "\\r"); break;
                case '\t': _json_append(buf, cap, pos, "\\t"); break;
                default:
                    if ((unsigned char)*p < 0x20) {
                        char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                        _json_append(buf, cap, pos, esc);
                    } else {
                        _json_putc(buf, cap, pos, *p);
                    }
            }
        }
        _json_append(buf, cap, pos, "\"");
    } else if (v->type == SW_VAL_ATOM) {
        if (strcmp(v->v.str, "true") == 0) _json_append(buf, cap, pos, "true");
        else if (strcmp(v->v.str, "false") == 0) _json_append(buf, cap, pos, "false");
        else if (strcmp(v->v.str, "nil") == 0) _json_append(buf, cap, pos, "null");
        else {
            _json_append(buf, cap, pos, "\"");
            _json_append(buf, cap, pos, v->v.str);
            _json_append(buf, cap, pos, "\"");
        }
    } else if (v->type == SW_VAL_LIST || v->type == SW_VAL_TUPLE) {
        _json_append(buf, cap, pos, "[");
        for (int i = 0; i < v->v.tuple.count; i++) {
            if (i > 0) _json_append(buf, cap, pos, ",");
            _json_encode_val(v->v.tuple.items[i], buf, cap, pos);
        }
        _json_append(buf, cap, pos, "]");
    } else if (v->type == SW_VAL_MAP) {
        _json_append(buf, cap, pos, "{");
        for (int i = 0; i < v->v.map.count; i++) {
            if (i > 0) _json_append(buf, cap, pos, ",");
            /* Key: always stringify */
            _json_append(buf, cap, pos, "\"");
            if (v->v.map.keys[i]->type == SW_VAL_STRING ||
                v->v.map.keys[i]->type == SW_VAL_ATOM)
                _json_append(buf, cap, pos, v->v.map.keys[i]->v.str);
            else {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%lld", (long long)v->v.map.keys[i]->v.i);
                _json_append(buf, cap, pos, tmp);
            }
            _json_append(buf, cap, pos, "\":");
            _json_encode_val(v->v.map.vals[i], buf, cap, pos);
        }
        _json_append(buf, cap, pos, "}");
    } else {
        _json_append(buf, cap, pos, "null");
    }
}

/* json_encode(val) → JSON string. Buffer grows automatically — large
 * agent histories with web_fetch results no longer silently truncate
 * at 256KB (which produced "Invalid request: unexpected EOF" from
 * the LLM API because the body ended mid-string). */
static sw_val_t *_builtin_json_encode(sw_val_t **a, int n) {
    if (n < 1) return sw_val_string("null");
    size_t cap = 65536;            /* initial guess; grows as needed */
    char *buf = (char *)malloc(cap);
    size_t pos = 0;
    _json_encode_val(a[0], &buf, &cap, &pos);
    buf[pos] = 0;
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* === JSON decode: JSON string → sw_val_t === */

static sw_val_t *_json_parse(const char **pp);

static void _json_skip_ws(const char **pp) {
    while (**pp == ' ' || **pp == '\t' || **pp == '\n' || **pp == '\r') (*pp)++;
}

/* Parse exactly four hex digits at *pp into a 16-bit codepoint.
 * Advances *pp past the digits. Returns -1 on bad input.
 * Does NOT consume a leading 'u' — caller positions after it. */
static int _json_parse_hex4(const char **pp) {
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

/* UTF-8 encode a 21-bit codepoint into buf. Returns bytes written (1-4). */
static int _utf8_encode(unsigned int cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static sw_val_t *_json_parse_string(const char **pp) {
    (*pp)++; /* skip opening " */
    size_t cap = 4096;
    char *buf = (char *)malloc(cap);
    size_t len = 0;
    while (**pp && **pp != '"') {
        if (**pp == '\\') {
            (*pp)++;
            switch (**pp) {
                case '"': case '\\': case '/': buf[len++] = **pp; (*pp)++; break;
                case 'n': buf[len++] = '\n'; (*pp)++; break;
                case 'r': buf[len++] = '\r'; (*pp)++; break;
                case 't': buf[len++] = '\t'; (*pp)++; break;
                case 'b': buf[len++] = '\b'; (*pp)++; break;
                case 'f': buf[len++] = '\f'; (*pp)++; break;
                case 'u': {
                    /* \uXXXX — parse 4 hex digits to a BMP codepoint.
                     * Handle surrogate pairs (\uD800-\uDBFF followed by
                     * \uDC00-\uDFFF) so emoji/CJK supplementary chars
                     * decode correctly. Otherwise UTF-8 encode the BMP
                     * codepoint as 1-3 bytes. The shell-special chars
                     * the model JSON-escapes most often (& = &,
                     * > = >, < = <) all fall in 1-byte ASCII
                     * range, which fixes the bash-tool composition bug. */
                    (*pp)++; /* skip 'u' */
                    int cp = _json_parse_hex4(pp);
                    if (cp < 0) {
                        /* Malformed escape — emit literal 'u' so we
                         * don't lose data. The hex parser left *pp
                         * wherever the bad char was. */
                        buf[len++] = 'u';
                        break;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        (*pp)[0] == '\\' && (*pp)[1] == 'u') {
                        /* High surrogate followed by \uXXXX — try to
                         * combine into a 21-bit code point. */
                        const char *save = *pp;
                        (*pp) += 2; /* skip "\u" */
                        int low = _json_parse_hex4(pp);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            unsigned int full = 0x10000 +
                                (((unsigned int)(cp - 0xD800)) << 10) +
                                (unsigned int)(low - 0xDC00);
                            if (len + 4 >= cap - 1) {
                                cap *= 2; buf = (char *)realloc(buf, cap);
                            }
                            len += _utf8_encode(full, buf + len);
                            break;
                        }
                        /* Not a valid pair — rewind and treat the
                         * high surrogate as a standalone codepoint. */
                        *pp = save;
                    }
                    if (len + 4 >= cap - 1) {
                        cap *= 2; buf = (char *)realloc(buf, cap);
                    }
                    len += _utf8_encode((unsigned int)cp, buf + len);
                    break;
                }
                default: buf[len++] = **pp; (*pp)++; break;
            }
        } else {
            buf[len++] = **pp;
            (*pp)++;
        }
        if (len >= cap - 1) { cap *= 2; buf = (char *)realloc(buf, cap); }
    }
    if (**pp == '"') (*pp)++;
    buf[len] = 0;
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

static sw_val_t *_json_parse_array(const char **pp) {
    (*pp)++; /* skip [ */
    _json_skip_ws(pp);
    int cap = 64, cnt = 0;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    while (**pp && **pp != ']') {
        items[cnt++] = _json_parse(pp);
        if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
        _json_skip_ws(pp);
        if (**pp == ',') (*pp)++;
        _json_skip_ws(pp);
    }
    if (**pp == ']') (*pp)++;
    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

static sw_val_t *_json_parse_object(const char **pp) {
    (*pp)++; /* skip { */
    _json_skip_ws(pp);
    int cap = 32, cnt = 0;
    sw_val_t **keys = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    sw_val_t **vals = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    while (**pp && **pp != '}') {
        _json_skip_ws(pp);
        if (**pp != '"') break;
        /* Parse key as atom (for dot access) */
        sw_val_t *key_str = _json_parse_string(pp);
        keys[cnt] = sw_val_atom(key_str->v.str);
        _json_skip_ws(pp);
        if (**pp == ':') (*pp)++;
        _json_skip_ws(pp);
        vals[cnt] = _json_parse(pp);
        cnt++;
        if (cnt >= cap) { cap *= 2; keys = (sw_val_t **)realloc(keys, sizeof(sw_val_t *) * cap); vals = (sw_val_t **)realloc(vals, sizeof(sw_val_t *) * cap); }
        _json_skip_ws(pp);
        if (**pp == ',') (*pp)++;
        _json_skip_ws(pp);
    }
    if (**pp == '}') (*pp)++;
    sw_val_t *r = sw_val_map_new(keys, vals, cnt);
    free(keys);
    free(vals);
    return r;
}

static sw_val_t *_json_parse(const char **pp) {
    _json_skip_ws(pp);
    if (**pp == '"') return _json_parse_string(pp);
    if (**pp == '[') return _json_parse_array(pp);
    if (**pp == '{') return _json_parse_object(pp);
    if (**pp == 't' && strncmp(*pp, "true", 4) == 0) { *pp += 4; return sw_val_atom("true"); }
    if (**pp == 'f' && strncmp(*pp, "false", 5) == 0) { *pp += 5; return sw_val_atom("false"); }
    if (**pp == 'n' && strncmp(*pp, "null", 4) == 0) { *pp += 4; return sw_val_nil(); }
    /* Number */
    const char *start = *pp;
    int is_float = 0;
    if (**pp == '-') (*pp)++;
    while (**pp >= '0' && **pp <= '9') (*pp)++;
    if (**pp == '.') { is_float = 1; (*pp)++; while (**pp >= '0' && **pp <= '9') (*pp)++; }
    if (**pp == 'e' || **pp == 'E') { is_float = 1; (*pp)++; if (**pp == '+' || **pp == '-') (*pp)++; while (**pp >= '0' && **pp <= '9') (*pp)++; }
    if (*pp == start) { (*pp)++; return sw_val_nil(); } /* junk */
    char tmp[64];
    size_t numlen = *pp - start;
    if (numlen > 63) numlen = 63;
    memcpy(tmp, start, numlen); tmp[numlen] = 0;
    if (is_float) return sw_val_float(strtod(tmp, NULL));
    return sw_val_int(strtoll(tmp, NULL, 10));
}

/* json_decode(str) → sw_val_t (map, list, string, int, etc.) */
static sw_val_t *_builtin_json_decode(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    const char *p = a[0]->v.str;
    return _json_parse(&p);
}

/* === File I/O extensions === */

#include <dirent.h>

/* file_exists(path) → 'true' | 'false' */
static sw_val_t *_builtin_file_exists(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_atom("false");
    struct stat st;
    return sw_val_atom(stat(a[0]->v.str, &st) == 0 ? "true" : "false");
}

/* file_list(dir) → list of filenames */
static sw_val_t *_builtin_file_list(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_list(NULL, 0);
    DIR *d = opendir(a[0]->v.str);
    if (!d) return sw_val_list(NULL, 0);
    int cap = 128, cnt = 0;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == 0 ||
            (ent->d_name[1] == '.' && ent->d_name[2] == 0))) continue;
        if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
        items[cnt++] = sw_val_string(ent->d_name);
    }
    closedir(d);
    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

/* file_delete(path) → 'ok' | 'error' */
static sw_val_t *_builtin_file_delete(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_atom("error");
    return sw_val_atom(unlink(a[0]->v.str) == 0 ? "ok" : "error");
}

/* file_append(path, content) → 'ok' | 'error' */
static sw_val_t *_builtin_file_append(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("error");
    FILE *f = fopen(a[0]->v.str, "a");
    if (!f) return sw_val_atom("error");
    fputs(a[1]->v.str, f);
    fclose(f);
    return sw_val_atom("ok");
}

/* === Timers: after & every === */

typedef struct { sw_val_t *fn; uint64_t ms; } _timer_closure_t;

static void _after_entry(void *raw) {
    _timer_closure_t *c = (_timer_closure_t *)raw;
    usleep((useconds_t)(c->ms * 1000));
    sw_val_apply(c->fn, NULL, 0);
    free(c);
}

/* delay(ms, fn) → pid — run fn once after ms milliseconds */
static sw_val_t *_builtin_delay(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT || !a[1]) return sw_val_nil();
    _timer_closure_t *c = (_timer_closure_t *)malloc(sizeof(_timer_closure_t));
    c->fn = a[1];
    c->ms = (uint64_t)a[0]->v.i;
    sw_process_t *p = sw_spawn(_after_entry, c);
    return p ? sw_val_pid(p) : sw_val_nil();
}

static void _every_entry(void *raw) {
    _timer_closure_t *c = (_timer_closure_t *)raw;
    for (;;) {
        usleep((useconds_t)(c->ms * 1000));
        sw_val_apply(c->fn, NULL, 0);
    }
}

/* interval(ms, fn) → pid — run fn repeatedly every ms milliseconds */
static sw_val_t *_builtin_interval(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT || !a[1]) return sw_val_nil();
    _timer_closure_t *c = (_timer_closure_t *)malloc(sizeof(_timer_closure_t));
    c->fn = a[1];
    c->ms = (uint64_t)a[0]->v.i;
    sw_process_t *p = sw_spawn(_every_entry, c);
    return p ? sw_val_pid(p) : sw_val_nil();
}

/* === LLM Client === */

/*
 * llm_complete(prompt)
 * llm_complete(prompt, opts_map)
 *   opts: %{model: "...", api_key: "...", url: "...", max_tokens: 4096, temperature: 0.7}
 *   defaults: model="gpt-4o-mini", reads OTONOMY_API_KEY or OPENAI_API_KEY env
 *   url default: "https://otonomy-inference-production.up.railway.app/v1/chat/completions"
 * Returns: string (the completion text)
 */
static sw_val_t *_builtin_llm_complete(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    const char *prompt = a[0]->v.str;

    /* Defaults — resolve URL from env: LLM_URL > OLLAMA_HOST > hardcoded */
    const char *model = "otonomy-orc";
    const char *api_key = NULL;
    const char *env_url = getenv("LLM_URL");
    const char *ollama_host = getenv("OLLAMA_HOST");
    /* Only use OLLAMA_HOST if it looks like a URL (has http), not a bind addr like 0.0.0.0 */
    const char *url = env_url ? env_url
                    : (ollama_host && strstr(ollama_host, "http")) ? ollama_host
                    : "https://otonomy-inference-production.up.railway.app/v1/chat/completions";
    int max_tokens = 4096;
    double temperature = 0.7;
    int retries = 0;
    int min_chars = 50;

    /* Parse opts map if provided */
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_MAP) {
        sw_val_t *m = a[1];
        for (int i = 0; i < m->v.map.count; i++) {
            const char *k = m->v.map.keys[i]->v.str;
            sw_val_t *v = m->v.map.vals[i];
            if (strcmp(k, "model") == 0 && v->type == SW_VAL_STRING) model = v->v.str;
            else if (strcmp(k, "api_key") == 0 && v->type == SW_VAL_STRING) api_key = v->v.str;
            else if (strcmp(k, "url") == 0 && v->type == SW_VAL_STRING) url = v->v.str;
            else if (strcmp(k, "max_tokens") == 0 && v->type == SW_VAL_INT) max_tokens = (int)v->v.i;
            else if (strcmp(k, "retries") == 0 && v->type == SW_VAL_INT) retries = (int)v->v.i;
            else if (strcmp(k, "min_chars") == 0 && v->type == SW_VAL_INT) min_chars = (int)v->v.i;
            else if (strcmp(k, "temperature") == 0) {
                if (v->type == SW_VAL_FLOAT) temperature = v->v.f;
                else if (v->type == SW_VAL_INT) temperature = (double)v->v.i;
            }
        }
    }

    /* Resolve API key from env if not provided */
    if (!api_key) api_key = getenv("OTONOMY_API_KEY");
    if (!api_key) api_key = getenv("OPENAI_API_KEY");
    if (!api_key) api_key = "ollama";  /* Ollama doesn't need a real key */

    /* Escape prompt for JSON */
    size_t plen = strlen(prompt);
    size_t esc_cap = plen * 2 + 64;
    char *esc_prompt = (char *)malloc(esc_cap);
    size_t ep = 0;
    for (size_t i = 0; i < plen && ep < esc_cap - 6; i++) {
        switch (prompt[i]) {
            case '"':  esc_prompt[ep++] = '\\'; esc_prompt[ep++] = '"'; break;
            case '\\': esc_prompt[ep++] = '\\'; esc_prompt[ep++] = '\\'; break;
            case '\n': esc_prompt[ep++] = '\\'; esc_prompt[ep++] = 'n'; break;
            case '\r': esc_prompt[ep++] = '\\'; esc_prompt[ep++] = 'r'; break;
            case '\t': esc_prompt[ep++] = '\\'; esc_prompt[ep++] = 't'; break;
            default:   esc_prompt[ep++] = prompt[i]; break;
        }
    }
    esc_prompt[ep] = 0;

    /* Build JSON body */
    size_t body_cap = ep + 512;
    char *body = (char *)malloc(body_cap);
    snprintf(body, body_cap,
        "{\"model\":\"%s\",\"max_tokens\":%d,\"temperature\":%.2f,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
        model, max_tokens, temperature, esc_prompt);
    free(esc_prompt);

    /* Build headers */
    sw_val_t **hdrs = (sw_val_t **)malloc(sizeof(sw_val_t *) * 2);
    sw_val_t **h0 = (sw_val_t **)malloc(sizeof(sw_val_t *) * 2);
    h0[0] = sw_val_string("Content-Type"); h0[1] = sw_val_string("application/json");
    hdrs[0] = sw_val_tuple(h0, 2); free(h0);
    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", api_key);
    sw_val_t **h1 = (sw_val_t **)malloc(sizeof(sw_val_t *) * 2);
    h1[0] = sw_val_string("Authorization"); h1[1] = sw_val_string(auth_hdr);
    hdrs[1] = sw_val_tuple(h1, 2); free(h1);
    sw_val_t *hdr_list = sw_val_list(hdrs, 2); free(hdrs);

    /* Retry loop: call http_post, parse response, retry if too short */
    int attempts = 1 + (retries > 0 ? retries : 0);
    if (attempts > 5) attempts = 5; /* cap at 5 */
    sw_val_t *last_result = sw_val_string("error: no response");

    for (int attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0) sw_sleep(2); /* 2s backoff between retries */

        sw_val_t *post_args[3] = { sw_val_string(url), hdr_list, sw_val_string(body) };
        sw_val_t *resp = _builtin_http_post(post_args, 3);
        if (!resp || resp->type != SW_VAL_STRING) {
            last_result = sw_val_string("error: no response");
            continue;
        }

        /* Extract content from response: choices[0].message.content
         * Falls back to "reasoning" field if content is empty (kimi/reasoning models) */
        sw_val_t *content = NULL;
        const char *try_keys[] = {"\"content\"", "\"reasoning\""};
        for (int ki = 0; ki < 2; ki++) {
            const char *found = strstr(resp->v.str, try_keys[ki]);
            if (!found) continue;
            found += strlen(try_keys[ki]);
            while (*found && (*found == ':' || *found == ' ' || *found == '\t')) found++;
            if (*found != '"') continue;
            const char *p = found;
            content = _json_parse_string(&p);
            if (content && content->type == SW_VAL_STRING && strlen(content->v.str) > 0)
                break;
            content = NULL;
        }

        if (content) {
            last_result = content;
            /* Check min_chars for retry eligibility */
            if ((int)strlen(content->v.str) >= min_chars || attempt >= attempts - 1)
                break; /* Good enough or last attempt */
            /* Too short — will retry */
        } else {
            last_result = resp;
        }
    }

    free(body);
    return last_result;
}

/* === Extra String Utilities === */

/* parse_gemma_calls(content) → list of {name_atom, args_map} tuples
 *
 * Extracts Gemma 4's native tool-call format: call:name{json_object}
 * Returns a list so the caller can iterate. Each entry is a 2-tuple:
 *   {atom("tool_name"), decoded_json_map}
 *
 * Example: "I'll read the file.\ncall:read{\"path\":\"/etc/hosts\"}"
 *  → [{read, %{path: "/etc/hosts"}}]
 */
static sw_val_t *_builtin_parse_gemma_calls(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return sw_val_list(NULL, 0);
    const char *s = a[0]->v.str;

    int cap = 8, cnt = 0;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);

    const char *p = s;
    while ((p = strstr(p, "call:")) != NULL) {
        /* Ensure call: is at a token boundary. Accept whitespace OR
         * sentence-ending punctuation as the preceding char so models
         * that emit "...broadly.call:bash{...}" (GLM-5.1) parse the
         * same as ones that emit "...\ncall:bash{...}" (Gemma).
         * The exclusion is alphanumeric + underscore — i.e. don't
         * match identifiers like `http_call:` or `myCall:`. */
        if (p != s) {
            char prev = *(p - 1);
            int is_word = (prev >= 'a' && prev <= 'z') ||
                          (prev >= 'A' && prev <= 'Z') ||
                          (prev >= '0' && prev <= '9') ||
                          prev == '_';
            if (is_word) { p += 5; continue; }
        }
        p += 5; /* skip "call:" */

        /* Extract tool name: everything up to '{' */
        const char *brace = strchr(p, '{');
        if (!brace) break;
        size_t name_len = brace - p;
        if (name_len == 0 || name_len > 64) { p = brace; continue; }
        char name_buf[65];
        memcpy(name_buf, p, name_len);
        name_buf[name_len] = '\0';
        /* Trim whitespace from name */
        while (name_len > 0 && (name_buf[name_len-1] == ' ' || name_buf[name_len-1] == '\t'))
            name_buf[--name_len] = '\0';

        /* Find matching closing brace (handle nested braces) */
        int depth = 0;
        const char *q = brace;
        int in_str = 0;
        while (*q) {
            if (*q == '"' && (q == brace || *(q-1) != '\\')) in_str = !in_str;
            if (!in_str) {
                if (*q == '{') depth++;
                else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            }
            q++;
        }
        if (depth != 0) break; /* unbalanced */

        /* Extract and parse the JSON */
        size_t json_len = q - brace;
        char *json_buf = (char *)malloc(json_len + 1);
        memcpy(json_buf, brace, json_len);
        json_buf[json_len] = '\0';

        const char *jp = json_buf;
        sw_val_t *decoded = _json_parse(&jp);
        free(json_buf);

        if (decoded && decoded->type == SW_VAL_MAP) {
            /* Build {name_atom, args_map} tuple */
            sw_val_t *tc[2];
            tc[0] = sw_val_atom(name_buf);
            tc[1] = decoded;
            if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
            items[cnt++] = sw_val_tuple(tc, 2);
        }
        p = q;
    }

    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

/* string_split(str, delim) → list of strings */
static sw_val_t *_builtin_string_split(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_list(NULL, 0);
    const char *str = a[0]->v.str;
    const char *delim = a[1]->v.str;
    size_t dlen = strlen(delim);
    if (dlen == 0) {
        sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *));
        items[0] = a[0];
        sw_val_t *r = sw_val_list(items, 1);
        free(items);
        return r;
    }
    /* Empty input → one empty segment, matching Python/JS .split().
     * Returning an empty list here made hd(string_split("")) panic
     * with "hd: list is empty" all over the codebase. */
    if (*str == 0) {
        sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *));
        items[0] = sw_val_string("");
        sw_val_t *r = sw_val_list(items, 1);
        free(items);
        return r;
    }
    int cap = 32, cnt = 0;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    const char *p = str;
    while (*p) {
        const char *f = strstr(p, delim);
        if (!f) f = p + strlen(p);
        size_t slen = f - p;
        char *seg = (char *)malloc(slen + 1);
        memcpy(seg, p, slen); seg[slen] = 0;
        if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
        items[cnt++] = sw_val_string(seg);
        free(seg);
        if (*f == 0) break;
        p = f + dlen;
        /* Trailing delimiter → emit a final empty segment. This push
         * needs the SAME cap guard as the one above — without it, a
         * string that splits into exactly `cap` segments and ends
         * with the delimiter writes items[cap], one past the buffer.
         * That heap overflow corrupts the allocator and crashes later
         * in an unrelated malloc (the classic "trace trap"). */
        if (*p == 0) {
            if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
            items[cnt++] = sw_val_string("");
            break;
        }
    }
    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

/* string_trim(str) → str with leading/trailing whitespace removed */
static sw_val_t *_builtin_string_trim(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_string("");
    const char *s = a[0]->v.str;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n' || s[len-1] == '\r')) len--;
    char *buf = (char *)malloc(len + 1);
    memcpy(buf, s, len); buf[len] = 0;
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* string_upper(str) → UPPERCASE */
static sw_val_t *_builtin_string_upper(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_string("");
    size_t len = strlen(a[0]->v.str);
    char *buf = (char *)malloc(len + 1);
    for (size_t i = 0; i <= len; i++)
        buf[i] = (a[0]->v.str[i] >= 'a' && a[0]->v.str[i] <= 'z') ? a[0]->v.str[i] - 32 : a[0]->v.str[i];
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* string_lower(str) → lowercase */
static sw_val_t *_builtin_string_lower(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_string("");
    size_t len = strlen(a[0]->v.str);
    char *buf = (char *)malloc(len + 1);
    for (size_t i = 0; i <= len; i++)
        buf[i] = (a[0]->v.str[i] >= 'A' && a[0]->v.str[i] <= 'Z') ? a[0]->v.str[i] + 32 : a[0]->v.str[i];
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* string_starts_with(str, prefix) → 'true' | 'false' */
static sw_val_t *_builtin_string_starts_with(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("false");
    size_t plen = strlen(a[1]->v.str);
    return sw_val_atom(strncmp(a[0]->v.str, a[1]->v.str, plen) == 0 ? "true" : "false");
}

/* string_ends_with(str, suffix) → 'true' | 'false' */
static sw_val_t *_builtin_string_ends_with(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("false");
    size_t slen = strlen(a[0]->v.str);
    size_t plen = strlen(a[1]->v.str);
    if (plen > slen) return sw_val_atom("false");
    return sw_val_atom(strcmp(a[0]->v.str + slen - plen, a[1]->v.str) == 0 ? "true" : "false");
}

/* === Agent Utilities === */

/* string_truncate(str, max_len) → truncated string
 * Replaces the trunc() pattern duplicated across .sw files */
static sw_val_t *_builtin_string_truncate(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_INT)
        return n >= 1 ? a[0] : sw_val_string("");
    const char *s = a[0]->v.str;
    int64_t max_len = a[1]->v.i;
    if (max_len <= 0) return sw_val_string("");
    size_t slen = strlen(s);
    if ((int64_t)slen <= max_len) return a[0];
    char *buf = (char *)malloc((size_t)max_len + 1);
    memcpy(buf, s, (size_t)max_len);
    buf[max_len] = 0;
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* clean_json(str) → str with markdown code fences stripped
 * LLMs often wrap JSON in ```json ... ``` — this strips that */
static sw_val_t *_builtin_clean_json(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_string("");
    const char *s = (a[0]->type == SW_VAL_STRING) ? a[0]->v.str : "";
    size_t slen = strlen(s);
    char *buf = (char *)malloc(slen + 1);
    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    /* Strip leading ```json or ```JSON or ``` */
    if (strncmp(s, "```json", 7) == 0) s += 7;
    else if (strncmp(s, "```JSON", 7) == 0) s += 7;
    else if (strncmp(s, "```", 3) == 0) s += 3;
    /* Skip whitespace/newline after opening fence */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    /* Copy content */
    size_t len = strlen(s);
    memcpy(buf, s, len + 1);
    /* Strip trailing ``` */
    while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\t' || buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
    if (len >= 3 && buf[len-1] == '`' && buf[len-2] == '`' && buf[len-3] == '`') len -= 3;
    while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\t' || buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
    buf[len] = 0;
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* strip_html(html) → plain text, pure C (no shell/sed dependency)
 * Strips script/style/nav blocks, then all HTML tags, collapses whitespace.
 * Returns first 8000 chars of cleaned text. */
static sw_val_t *_builtin_strip_html(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_string("");
    const char *html = a[0]->v.str;
    size_t hlen = strlen(html);
    if (hlen == 0) return sw_val_string("");

    /* First pass: copy html, zeroing out script/style/nav blocks */
    char *work = (char *)malloc(hlen + 1);
    memcpy(work, html, hlen + 1);

    const char *block_tags[] = {"script", "style", "nav", "header", "footer", NULL};
    for (int t = 0; block_tags[t]; t++) {
        char open_tag[32], close_tag[32];
        snprintf(open_tag, sizeof(open_tag), "<%s", block_tags[t]);
        snprintf(close_tag, sizeof(close_tag), "</%s>", block_tags[t]);
        size_t ctlen = strlen(close_tag);
        char *p = work;
        while ((p = strcasestr(p, open_tag)) != NULL) {
            /* Verify it's actually a tag (next char is space, >, or end) */
            char after = p[strlen(open_tag)];
            if (after != ' ' && after != '>' && after != '\t' && after != '\n' && after != 0) {
                p++; continue;
            }
            char *end = strcasestr(p, close_tag);
            if (end) {
                memset(p, ' ', (size_t)(end - p) + ctlen);
                p = end + ctlen;
            } else {
                /* No closing tag — zero out rest */
                memset(p, ' ', strlen(p));
                break;
            }
        }
    }

    /* Second pass: strip all HTML tags, decode common entities, collapse whitespace */
    size_t cap = 8001;
    char *out = (char *)malloc(cap);
    size_t olen = 0;
    int in_tag = 0;
    int last_was_space = 1;

    for (size_t i = 0; i < hlen && olen < cap - 1; i++) {
        char c = work[i];
        if (c == '<') { in_tag = 1; continue; }
        if (c == '>') { in_tag = 0; continue; }
        if (in_tag) continue;

        /* Decode common HTML entities */
        if (c == '&' && i + 2 < hlen) {
            if (strncmp(work + i, "&amp;", 5) == 0) { c = '&'; i += 4; }
            else if (strncmp(work + i, "&lt;", 4) == 0) { c = '<'; i += 3; }
            else if (strncmp(work + i, "&gt;", 4) == 0) { c = '>'; i += 3; }
            else if (strncmp(work + i, "&quot;", 6) == 0) { c = '"'; i += 5; }
            else if (strncmp(work + i, "&apos;", 6) == 0) { c = '\''; i += 5; }
            else if (strncmp(work + i, "&#39;", 5) == 0) { c = '\''; i += 4; }
            else if (strncmp(work + i, "&nbsp;", 6) == 0) { c = ' '; i += 5; }
            else if (strncmp(work + i, "&#x27;", 6) == 0) { c = '\''; i += 5; }
        }

        /* Collapse whitespace */
        int is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (is_ws) {
            if (!last_was_space) { out[olen++] = ' '; last_was_space = 1; }
        } else {
            out[olen++] = c;
            last_was_space = 0;
        }
    }
    out[olen] = 0;
    free(work);
    sw_val_t *r = sw_val_string(out);
    free(out);
    return r;
}

/* === LiveView HTTP/WS Builtins === */

/* http_listen(port) → 'ok' | 'error' — starts HTTP server, handler = calling process */
static sw_val_t *_builtin_http_listen(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    sw_io_init();
    uint16_t port = (uint16_t)a[0]->v.i;
    sw_process_t *bp = sw_http_listen(port, sw_self());
    if (!bp) return sw_val_atom("error");
    return sw_val_atom("ok");
}

/* http_respond(conn, status, headers, body) → 'ok' | 'error' */
static sw_val_t *_builtin_http_respond(sw_val_t **a, int n) {
    if (n < 4) return sw_val_atom("error");
    if (!a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    if (!a[1] || a[1]->type != SW_VAL_INT) return sw_val_atom("error");
    int conn_id = (int)a[0]->v.i;
    int status = (int)a[1]->v.i;
    const char *headers = (a[2] && a[2]->type == SW_VAL_STRING) ? a[2]->v.str : "";
    const char *body = (a[3] && a[3]->type == SW_VAL_STRING) ? a[3]->v.str : "";
    int rc = sw_http_respond(conn_id, status, headers, body);
    return sw_val_atom(rc == 0 ? "ok" : "error");
}

/* ws_send(conn, data) → 'ok' | 'error' — send WebSocket text frame */
static sw_val_t *_builtin_ws_send(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("error");
    int conn_id = (int)a[0]->v.i;
    int rc = sw_ws_send_text(conn_id, a[1]->v.str, (uint32_t)strlen(a[1]->v.str));
    return sw_val_atom(rc == 0 ? "ok" : "error");
}

/* ws_close(conn) → 'ok' | 'error' */
static sw_val_t *_builtin_ws_close(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    int rc = sw_ws_close((int)a[0]->v.i);
    return sw_val_atom(rc == 0 ? "ok" : "error");
}

/* ws_set_handler(conn, pid) → 'ok' | 'error' — transfer WS to spawned view process */
static sw_val_t *_builtin_ws_set_handler(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT ||
        !a[1] || a[1]->type != SW_VAL_PID) return sw_val_atom("error");
    int rc = sw_ws_set_handler((int)a[0]->v.i, a[1]->v.pid);
    return sw_val_atom(rc == 0 ? "ok" : "error");
}

/* live_js() → string containing client-side LiveView JavaScript */
static sw_val_t *_builtin_live_js(sw_val_t **a, int n) {
    (void)a; (void)n;
    return sw_val_string(sw_liveview_js());
}

/* ================================================================
 * Phase 15: Feature Expansion — URL, Static Files, PubSub,
 * Telemetry, Circuit Breaker, Streaming LLM, ETS Introspection
 * ================================================================ */

/* === A3: URL Query Parameter Parsing === */

static int _url_decode_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* query_parse(path) → map of query params
 * e.g. "/search?q=hello&page=2" → %{q: "hello", page: "2"} */
static sw_val_t *_builtin_query_parse(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return sw_val_map_new(NULL, NULL, 0);
    const char *path = a[0]->v.str;
    const char *qmark = strchr(path, '?');
    if (!qmark) return sw_val_map_new(NULL, NULL, 0);

    const char *qs = qmark + 1;
    int cap = 16, cnt = 0;
    sw_val_t **keys = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    sw_val_t **vals = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);

    while (*qs) {
        /* Find key */
        const char *eq = strchr(qs, '=');
        const char *amp = strchr(qs, '&');
        if (!eq || (amp && amp < eq)) {
            /* Key with no value — skip to next */
            if (amp) { qs = amp + 1; continue; }
            break;
        }

        /* Decode key */
        size_t klen = eq - qs;
        char *key = (char *)malloc(klen + 1);
        size_t ko = 0;
        for (size_t i = 0; i < klen; i++) {
            if (qs[i] == '%' && i + 2 < klen) {
                int h = _url_decode_hex(qs[i+1]);
                int l = _url_decode_hex(qs[i+2]);
                if (h >= 0 && l >= 0) { key[ko++] = (char)(h * 16 + l); i += 2; continue; }
            }
            if (qs[i] == '+') { key[ko++] = ' '; continue; }
            key[ko++] = qs[i];
        }
        key[ko] = '\0';

        /* Decode value */
        const char *vs = eq + 1;
        size_t vlen = amp ? (size_t)(amp - vs) : strlen(vs);
        char *val = (char *)malloc(vlen + 1);
        size_t vo = 0;
        for (size_t i = 0; i < vlen; i++) {
            if (vs[i] == '%' && i + 2 < vlen) {
                int h = _url_decode_hex(vs[i+1]);
                int l = _url_decode_hex(vs[i+2]);
                if (h >= 0 && l >= 0) { val[vo++] = (char)(h * 16 + l); i += 2; continue; }
            }
            if (vs[i] == '+') { val[vo++] = ' '; continue; }
            val[vo++] = vs[i];
        }
        val[vo] = '\0';

        if (cnt >= cap) { cap *= 2; keys = realloc(keys, sizeof(sw_val_t *) * cap); vals = realloc(vals, sizeof(sw_val_t *) * cap); }
        keys[cnt] = sw_val_atom(key);
        vals[cnt] = sw_val_string(val);
        cnt++;
        free(key);
        free(val);

        if (!amp) break;
        qs = amp + 1;
    }

    sw_val_t *r = sw_val_map_new(keys, vals, cnt);
    free(keys);
    free(vals);
    return r;
}

/* === A4: Static File Serving === */

static const char *_mime_for_ext(const char *ext) {
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, "js") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, "json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "woff2") == 0) return "font/woff2";
    if (strcasecmp(ext, "woff") == 0) return "font/woff";
    if (strcasecmp(ext, "txt") == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/* http_serve_file(conn, filepath) → 'ok' | 'error'
 * Sends file contents as HTTP response with correct MIME type. */
static sw_val_t *_builtin_http_serve_file(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("error");
    int conn_id = (int)a[0]->v.i;
    const char *filepath = a[1]->v.str;

    /* Directory traversal protection */
    if (strstr(filepath, "..")) return sw_val_atom("error");

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        sw_http_respond(conn_id, 404, "Content-Type: text/plain\r\n", "Not Found");
        return sw_val_atom("error");
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0 || sz > 50 * 1024 * 1024) { /* 50MB limit */
        fclose(fp);
        sw_http_respond(conn_id, 413, "Content-Type: text/plain\r\n", "File Too Large");
        return sw_val_atom("error");
    }

    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    size_t rd = fread(data, 1, (size_t)sz, fp);
    fclose(fp);

    /* Determine MIME from extension */
    const char *dot = strrchr(filepath, '.');
    const char *mime = _mime_for_ext(dot ? dot + 1 : NULL);

    /* Send response via sw_http_respond_raw (binary safe) */
    char ct_hdr[256];
    snprintf(ct_hdr, sizeof(ct_hdr), "Content-Type: %s\r\n", mime);

    int rc = sw_http_respond_raw(conn_id, 200, ct_hdr, data, (uint32_t)rd);
    free(data);
    return sw_val_atom(rc == 0 ? "ok" : "error");
}

/* === B1: PubSub / Broadcast === */

#include "swarmrt_phase5.h"

/* pubsub_subscribe(topic) → 'ok' */
static sw_val_t *_builtin_pubsub_subscribe(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");
    sw_pg_join(a[0]->v.str, sw_self());
    return sw_val_atom("ok");
}

/* pubsub_broadcast(topic, event, payload) → 'ok'
 * Sends {'pubsub', topic, event, payload} to all subscribers */
static sw_val_t *_builtin_pubsub_broadcast(sw_val_t **a, int n) {
    if (n < 3 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");
    sw_val_t *items[4];
    items[0] = sw_val_atom("pubsub");
    items[1] = a[0]; /* topic */
    items[2] = a[1]; /* event */
    items[3] = a[2]; /* payload */
    sw_val_t *tuple = sw_val_tuple(items, 4);
    sw_pg_dispatch(a[0]->v.str, SW_TAG_NONE, tuple);
    return sw_val_atom("ok");
}

/* pubsub_unsubscribe(topic) → 'ok' */
static sw_val_t *_builtin_pubsub_unsubscribe(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");
    sw_pg_leave(a[0]->v.str, sw_self());
    return sw_val_atom("ok");
}

/* === B2: Telemetry === */

/* telemetry_emit(event, measurements_map) → 'ok'
 * Broadcasts {'telemetry', event, measurements} to "telemetry:<event>" topic */
static sw_val_t *_builtin_telemetry_emit(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");
    char topic[256];
    snprintf(topic, sizeof(topic), "telemetry:%s", a[0]->v.str);
    sw_val_t *items[3];
    items[0] = sw_val_atom("telemetry");
    items[1] = a[0]; /* event */
    items[2] = a[1]; /* measurements */
    sw_val_t *tuple = sw_val_tuple(items, 3);
    sw_pg_dispatch(topic, SW_TAG_NONE, tuple);
    return sw_val_atom("ok");
}

/* telemetry_subscribe(event) → 'ok' */
static sw_val_t *_builtin_telemetry_subscribe(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");
    char topic[256];
    snprintf(topic, sizeof(topic), "telemetry:%s", a[0]->v.str);
    sw_pg_join(topic, sw_self());
    return sw_val_atom("ok");
}

/* telemetry_unsubscribe(event) → 'ok' */
static sw_val_t *_builtin_telemetry_unsubscribe(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");
    char topic[256];
    snprintf(topic, sizeof(topic), "telemetry:%s", a[0]->v.str);
    sw_pg_leave(topic, sw_self());
    return sw_val_atom("ok");
}

/* === B3: Circuit Breaker === */

#define _BREAKER_MAX 32

typedef enum {
    _BREAKER_CLOSED,
    _BREAKER_OPEN,
    _BREAKER_HALF_OPEN
} _breaker_state_t;

typedef struct {
    char name[64];
    _breaker_state_t state;
    int failure_count;
    int max_failures;       /* threshold to open */
    uint64_t reset_timeout_ms;
    uint64_t opened_at;     /* timestamp when breaker opened */
    int active;
} _breaker_t;

static _breaker_t _breakers[_BREAKER_MAX];
static pthread_mutex_t _breaker_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t _breaker_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* breaker_new(name, opts) → 'ok'
 * opts: %{max_failures: 5, reset_timeout_ms: 30000} */
static sw_val_t *_builtin_breaker_new(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");
    const char *name = a[0]->v.str;
    int max_f = 5;
    uint64_t reset_ms = 30000;
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_MAP) {
        for (int i = 0; i < a[1]->v.map.count; i++) {
            const char *k = a[1]->v.map.keys[i]->v.str;
            sw_val_t *v = a[1]->v.map.vals[i];
            if (strcmp(k, "max_failures") == 0 && v->type == SW_VAL_INT) max_f = (int)v->v.i;
            else if (strcmp(k, "reset_timeout_ms") == 0 && v->type == SW_VAL_INT) reset_ms = (uint64_t)v->v.i;
        }
    }

    pthread_mutex_lock(&_breaker_lock);
    /* Find existing or free slot */
    int slot = -1;
    for (int i = 0; i < _BREAKER_MAX; i++) {
        if (_breakers[i].active && strcmp(_breakers[i].name, name) == 0) { slot = i; break; }
        if (!_breakers[i].active && slot < 0) slot = i;
    }
    if (slot < 0) { pthread_mutex_unlock(&_breaker_lock); return sw_val_atom("error"); }
    _breaker_t *b = &_breakers[slot];
    strncpy(b->name, name, 63);
    b->state = _BREAKER_CLOSED;
    b->failure_count = 0;
    b->max_failures = max_f;
    b->reset_timeout_ms = reset_ms;
    b->opened_at = 0;
    b->active = 1;
    pthread_mutex_unlock(&_breaker_lock);
    return sw_val_atom("ok");
}

static int _breaker_is_error(sw_val_t *result) {
    if (!result || result->type == SW_VAL_NIL) return 1;
    if (result->type == SW_VAL_ATOM && strcmp(result->v.str, "error") == 0) return 1;
    if (result->type == SW_VAL_TUPLE && result->v.tuple.count >= 1 &&
        result->v.tuple.items[0]->type == SW_VAL_ATOM &&
        strcmp(result->v.tuple.items[0]->v.str, "error") == 0) return 1;
    return 0;
}

/* breaker_call(name, fun) → result | {:error, :circuit_open} */
static sw_val_t *_builtin_breaker_call(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");

    pthread_mutex_lock(&_breaker_lock);
    _breaker_t *b = NULL;
    for (int i = 0; i < _BREAKER_MAX; i++) {
        if (_breakers[i].active && strcmp(_breakers[i].name, a[0]->v.str) == 0) { b = &_breakers[i]; break; }
    }
    if (!b) { pthread_mutex_unlock(&_breaker_lock); return sw_val_atom("error"); }

    uint64_t now = _breaker_now_ms();

    /* Check state transitions */
    if (b->state == _BREAKER_OPEN) {
        if (now - b->opened_at >= b->reset_timeout_ms) {
            b->state = _BREAKER_HALF_OPEN;
        } else {
            pthread_mutex_unlock(&_breaker_lock);
            sw_val_t *items[2];
            items[0] = sw_val_atom("error");
            items[1] = sw_val_atom("circuit_open");
            return sw_val_tuple(items, 2);
        }
    }

    _breaker_state_t prev_state = b->state;
    pthread_mutex_unlock(&_breaker_lock);

    /* Execute the function */
    sw_val_t *result = sw_val_apply(a[1], NULL, 0);

    pthread_mutex_lock(&_breaker_lock);
    /* Re-find breaker (safe) */
    b = NULL;
    for (int i = 0; i < _BREAKER_MAX; i++) {
        if (_breakers[i].active && strcmp(_breakers[i].name, a[0]->v.str) == 0) { b = &_breakers[i]; break; }
    }
    if (!b) { pthread_mutex_unlock(&_breaker_lock); return result; }

    if (_breaker_is_error(result)) {
        b->failure_count++;
        if (prev_state == _BREAKER_HALF_OPEN || b->failure_count >= b->max_failures) {
            b->state = _BREAKER_OPEN;
            b->opened_at = _breaker_now_ms();
        }
    } else {
        /* Success */
        if (prev_state == _BREAKER_HALF_OPEN) {
            b->state = _BREAKER_CLOSED;
            b->failure_count = 0;
        } else if (b->state == _BREAKER_CLOSED) {
            b->failure_count = 0;
        }
    }
    pthread_mutex_unlock(&_breaker_lock);
    return result;
}

/* breaker_state(name) → :closed | :open | :half_open */
static sw_val_t *_builtin_breaker_state(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM))
        return sw_val_atom("error");
    pthread_mutex_lock(&_breaker_lock);
    for (int i = 0; i < _BREAKER_MAX; i++) {
        if (_breakers[i].active && strcmp(_breakers[i].name, a[0]->v.str) == 0) {
            /* Check for auto-transition to half_open */
            if (_breakers[i].state == _BREAKER_OPEN &&
                _breaker_now_ms() - _breakers[i].opened_at >= _breakers[i].reset_timeout_ms)
                _breakers[i].state = _BREAKER_HALF_OPEN;
            const char *s;
            switch (_breakers[i].state) {
                case _BREAKER_CLOSED:   s = "closed"; break;
                case _BREAKER_OPEN:     s = "open"; break;
                case _BREAKER_HALF_OPEN: s = "half_open"; break;
                default: s = "unknown"; break;
            }
            pthread_mutex_unlock(&_breaker_lock);
            return sw_val_atom(s);
        }
    }
    pthread_mutex_unlock(&_breaker_lock);
    return sw_val_atom("error");
}

/* === C1: Streaming LLM Output === */

typedef struct {
    sw_process_t *caller;
    char *prompt;
    char *model;
    char *api_key;
    char *url;
    int max_tokens;
    double temperature;
} _llm_stream_ctx_t;

static void _llm_stream_entry(void *raw) {
    _llm_stream_ctx_t *ctx = (_llm_stream_ctx_t *)raw;

    /* Escape prompt for JSON */
    size_t plen = strlen(ctx->prompt);
    size_t esc_cap = plen * 2 + 64;
    char *esc = (char *)malloc(esc_cap);
    size_t ep = 0;
    for (size_t i = 0; i < plen && ep < esc_cap - 6; i++) {
        switch (ctx->prompt[i]) {
            case '"':  esc[ep++] = '\\'; esc[ep++] = '"'; break;
            case '\\': esc[ep++] = '\\'; esc[ep++] = '\\'; break;
            case '\n': esc[ep++] = '\\'; esc[ep++] = 'n'; break;
            case '\r': esc[ep++] = '\\'; esc[ep++] = 'r'; break;
            case '\t': esc[ep++] = '\\'; esc[ep++] = 't'; break;
            default:   esc[ep++] = ctx->prompt[i]; break;
        }
    }
    esc[ep] = '\0';

    /* Build JSON body */
    size_t body_cap = ep + 512;
    char *body = (char *)malloc(body_cap);
    snprintf(body, body_cap,
        "{\"model\":\"%s\",\"max_tokens\":%d,\"temperature\":%.2f,\"stream\":true,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
        ctx->model, ctx->max_tokens, ctx->temperature, esc);
    free(esc);

    /* Build curl command for SSE streaming */
    size_t cmd_cap = strlen(body) + strlen(ctx->url) + strlen(ctx->api_key) + 512;
    char *cmd = (char *)malloc(cmd_cap);
    /* Write body to temp file for safety */
    char tmpf[256];
    snprintf(tmpf, sizeof(tmpf), "%s/sw_llm_stream_%d_%u.json", sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    FILE *tf = fopen(tmpf, "w");
    if (tf) { fputs(body, tf); fclose(tf); }
    free(body);

    snprintf(cmd, cmd_cap,
        "curl -sS -N --connect-timeout 30 --max-time 300 "
        "-H 'Content-Type: application/json' "
        "-H 'Authorization: Bearer %s' "
        "-d @%s '%s' 2>/dev/null",
        ctx->api_key, tmpf, ctx->url);

    FILE *fp = sw_popen(cmd, "r");
    free(cmd);
    swbs_unlink(tmpf);

    if (!fp) {
        sw_val_t *items[2];
        items[0] = sw_val_atom("llm_done");
        items[1] = sw_val_string("error: failed to start curl");
        sw_send_tagged(ctx->caller, SW_TAG_NONE, sw_val_tuple(items, 2));
        goto cleanup;
    }

    /* Read SSE lines, extract delta content */
    char line[4096];
    size_t full_cap = 65536, full_len = 0;
    char *full_text = (char *)malloc(full_cap);
    full_text[0] = '\0';

    while (fgets(line, sizeof(line), fp)) {
        /* Skip empty lines and non-data lines */
        if (strncmp(line, "data: ", 6) != 0) continue;
        char *data = line + 6;

        /* Check for stream end */
        if (strncmp(data, "[DONE]", 6) == 0) break;

        /* Extract delta.content from JSON chunk */
        const char *dc = strstr(data, "\"delta\"");
        if (!dc) continue;
        const char *ct = strstr(dc, "\"content\"");
        if (!ct) continue;
        ct += 9;
        while (*ct && (*ct == ':' || *ct == ' ' || *ct == '\t')) ct++;
        if (*ct != '"') continue;

        /* Parse the string value */
        ct++;
        char token[4096];
        size_t ti = 0;
        while (*ct && *ct != '"' && ti < sizeof(token) - 1) {
            if (*ct == '\\' && ct[1]) {
                ct++;
                switch (*ct) {
                    case 'n': token[ti++] = '\n'; break;
                    case 't': token[ti++] = '\t'; break;
                    case '"': token[ti++] = '"'; break;
                    case '\\': token[ti++] = '\\'; break;
                    default: token[ti++] = *ct; break;
                }
            } else {
                token[ti++] = *ct;
            }
            ct++;
        }
        token[ti] = '\0';

        if (ti > 0) {
            /* Accumulate full text */
            if (full_len + ti >= full_cap) {
                full_cap = (full_len + ti) * 2;
                full_text = (char *)realloc(full_text, full_cap);
            }
            memcpy(full_text + full_len, token, ti);
            full_len += ti;
            full_text[full_len] = '\0';

            /* Send token to caller */
            sw_val_t *items[2];
            items[0] = sw_val_atom("llm_token");
            items[1] = sw_val_string(token);
            sw_send_tagged(ctx->caller, SW_TAG_NONE, sw_val_tuple(items, 2));
        }
    }

    sw_pclose(fp);

    /* Send completion message */
    {
        sw_val_t *items[2];
        items[0] = sw_val_atom("llm_done");
        items[1] = sw_val_string(full_text);
        sw_send_tagged(ctx->caller, SW_TAG_NONE, sw_val_tuple(items, 2));
    }
    free(full_text);

cleanup:
    free(ctx->prompt);
    free(ctx->model);
    free(ctx->api_key);
    free(ctx->url);
    free(ctx);
}

/* llm_stream(prompt, opts) → pid
 * Spawns background process that sends {'llm_token', text} and {'llm_done', full_text} */
static sw_val_t *_builtin_llm_stream(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();

    _llm_stream_ctx_t *ctx = (_llm_stream_ctx_t *)calloc(1, sizeof(_llm_stream_ctx_t));
    ctx->caller = sw_self();
    ctx->prompt = strdup(a[0]->v.str);
    ctx->model = strdup("otonomy-orc");
    ctx->url = strdup("https://otonomy-inference-production.up.railway.app/v1/chat/completions");
    ctx->max_tokens = 4096;
    ctx->temperature = 0.7;
    ctx->api_key = NULL;

    /* Parse opts map */
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_MAP) {
        for (int i = 0; i < a[1]->v.map.count; i++) {
            const char *k = a[1]->v.map.keys[i]->v.str;
            sw_val_t *v = a[1]->v.map.vals[i];
            if (strcmp(k, "model") == 0 && v->type == SW_VAL_STRING) { free(ctx->model); ctx->model = strdup(v->v.str); }
            else if (strcmp(k, "api_key") == 0 && v->type == SW_VAL_STRING) { ctx->api_key = strdup(v->v.str); }
            else if (strcmp(k, "url") == 0 && v->type == SW_VAL_STRING) { free(ctx->url); ctx->url = strdup(v->v.str); }
            else if (strcmp(k, "max_tokens") == 0 && v->type == SW_VAL_INT) ctx->max_tokens = (int)v->v.i;
            else if (strcmp(k, "temperature") == 0) {
                if (v->type == SW_VAL_FLOAT) ctx->temperature = v->v.f;
                else if (v->type == SW_VAL_INT) ctx->temperature = (double)v->v.i;
            }
        }
    }

    /* Resolve API key from env if not provided */
    if (!ctx->api_key) {
        const char *k = getenv("OTONOMY_API_KEY");
        if (!k) k = getenv("OPENAI_API_KEY");
        ctx->api_key = strdup(k ? k : "");
    }

    sw_process_t *p = sw_spawn(_llm_stream_entry, ctx);
    return p ? sw_val_pid(p) : sw_val_nil();
}

/* === D1: ETS List/Count Builtins === */

/* ets_list(table_id) → list of keys in that table */
static sw_val_t *_builtin_ets_list(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_list(NULL, 0);
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_list(NULL, 0);
    _vets_table_t *t = &_vets_tables[id];

    int cap = 64, cnt = 0;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);

    pthread_rwlock_rdlock(&t->lock);
    for (int b = 0; b < _VETS_BUCKETS; b++) {
        _vets_entry_t *e = t->buckets[b];
        while (e) {
            if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
            items[cnt++] = e->key;
            e = e->next;
        }
    }
    pthread_rwlock_unlock(&t->lock);

    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

/* ets_count(table_id) → int */
static sw_val_t *_builtin_ets_count(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_int(-1);
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_int(-1);
    _vets_table_t *t = &_vets_tables[id];

    int count = 0;
    pthread_rwlock_rdlock(&t->lock);
    for (int b = 0; b < _VETS_BUCKETS; b++) {
        for (_vets_entry_t *e = t->buckets[b]; e; e = e->next) count++;
    }
    pthread_rwlock_unlock(&t->lock);

    return sw_val_int((int64_t)count);
}

/* === PDF builtins === */
#include "swarmrt_pdf.h"

/* pdf_text(path) → string | nil
 * pdf_text(path, %{pages: [0,1]}) → string | nil */
static sw_val_t *_builtin_pdf_text(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING || !a[0]->v.str)
        return sw_val_nil();

    /* Read file */
    FILE *fp = fopen(a[0]->v.str, "rb");
    if (!fp) return sw_val_nil();
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 100 * 1024 * 1024) { fclose(fp); return sw_val_nil(); } /* 100MB limit */
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(fp); return sw_val_nil(); }
    fread(buf, 1, sz, fp);
    fclose(fp);

    char *text = NULL;
    size_t text_len = 0;
    int rc;

    /* Check for page selection (second arg is map with "pages" key) */
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_MAP) {
        /* Look for pages key — simplified: extract pages from map */
        /* For now, extract all text */
        rc = sw_pdf_extract_text(buf, sz, &text, &text_len);
    } else {
        rc = sw_pdf_extract_text(buf, sz, &text, &text_len);
    }

    free(buf);
    if (rc != SW_PDF_OK || !text) { free(text); return sw_val_nil(); }
    sw_val_t *r = sw_val_string(text);
    free(text);
    return r;
}

/* pdf_pages(path) → int | nil */
static sw_val_t *_builtin_pdf_pages(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING || !a[0]->v.str)
        return sw_val_nil();

    FILE *fp = fopen(a[0]->v.str, "rb");
    if (!fp) return sw_val_nil();
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 100 * 1024 * 1024) { fclose(fp); return sw_val_nil(); }
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(fp); return sw_val_nil(); }
    fread(buf, 1, sz, fp);
    fclose(fp);

    int count = 0;
    int rc = sw_pdf_page_count(buf, sz, &count);
    free(buf);
    if (rc != SW_PDF_OK) return sw_val_nil();
    return sw_val_int(count);
}

/* pdf_meta(path) → %{title, author, ...} | nil */
static sw_val_t *_builtin_pdf_meta(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING || !a[0]->v.str)
        return sw_val_nil();

    FILE *fp = fopen(a[0]->v.str, "rb");
    if (!fp) return sw_val_nil();
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 100 * 1024 * 1024) { fclose(fp); return sw_val_nil(); }
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(fp); return sw_val_nil(); }
    fread(buf, 1, sz, fp);
    fclose(fp);

    sw_pdf_meta_t meta;
    int rc = sw_pdf_metadata(buf, sz, &meta);
    free(buf);
    if (rc != SW_PDF_OK) return sw_val_nil();

    /* Build a map with title, author, subject, creator, creation_date */
    sw_val_t *keys[5], *vals[5];
    int mc = 0;
    if (meta.title) { keys[mc] = sw_val_string("title"); vals[mc] = sw_val_string(meta.title); mc++; }
    if (meta.author) { keys[mc] = sw_val_string("author"); vals[mc] = sw_val_string(meta.author); mc++; }
    if (meta.subject) { keys[mc] = sw_val_string("subject"); vals[mc] = sw_val_string(meta.subject); mc++; }
    if (meta.creator) { keys[mc] = sw_val_string("creator"); vals[mc] = sw_val_string(meta.creator); mc++; }
    if (meta.creation_date) { keys[mc] = sw_val_string("creation_date"); vals[mc] = sw_val_string(meta.creation_date); mc++; }
    sw_pdf_meta_free(&meta);
    if (mc == 0) return sw_val_map_new(NULL, NULL, 0);
    return sw_val_map_new(keys, vals, mc);
}

/* ============================================================
 * Phase 16: Interactive CLI primitives (swarm-code)
 * ============================================================ */

/* read_line(prompt?) → string | nil
 *
 * Proper line editor that handles bracketed paste (multiline paste as a
 * single input), backspace, Ctrl+D for EOF, and up-arrow history.
 *
 * Under the hood this puts stdin into non-canonical (raw) mode via
 * termios so we can read one keystroke at a time. The terminal is
 * restored via atexit() when the process exits. Bracketed paste is
 * enabled via the \e[?2004h escape sequence — modern terminals
 * (Terminal.app, iTerm2, Alacritty, kitty, WezTerm) all support it.
 *
 * In raw mode we handle echo ourselves:
 *   - printable chars → append + echo
 *   - backspace (0x7f/0x08) → pop + "\b \b"
 *   - Enter (0x0a/0x0d) → emit newline, return buffer
 *   - Ctrl+D on empty line (0x04) → return nil (EOF)
 *   - Ctrl+C (0x03) → kernel-handled (ISIG still set)
 *   - ESC[200~...ESC[201~ → bracketed paste; collect verbatim
 *   - ESC[A / ESC[B → up/down for simple history recall
 *
 * Works for non-terminal stdin too (piped input): if tcgetattr fails,
 * we fall back to the old fgetc-until-newline loop so piped scripts
 * still work for tests.
 */

/* termios / _sw_rl_state_t already declared near the top of this
 * file so http_post_stream's interrupt path can see them. */
#include <unistd.h>

static void _sw_rl_restore(void) {
    if (_sw_rl.saved_ok) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &_sw_rl.saved);
        fputs("\x1b[?2004l", stdout);
        fflush(stdout);
        _sw_rl.saved_ok = 0;
    }
}

static int _sw_rl_setup(void) {
    if (_sw_rl.saved_ok) return 1;
    if (!isatty(STDIN_FILENO)) return 0;
    if (tcgetattr(STDIN_FILENO, &_sw_rl.saved) != 0) return 0;
    struct termios raw = _sw_rl.saved;
    /* Non-canonical, no echo. Keep ISIG so Ctrl+C stays. */
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~(ICRNL); /* don't translate CR to NL — we handle both */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return 0;
    _sw_rl.saved_ok = 1;
    atexit(_sw_rl_restore);
    fputs("\x1b[?2004h", stdout);
    fflush(stdout);
    return 1;
}

static void _sw_rl_history_push(const char *line) {
    if (!line || !*line) return;
    /* Skip duplicates of the last entry */
    if (_sw_rl.count > 0 && strcmp(_sw_rl.entries[_sw_rl.count - 1], line) == 0) return;
    if (_sw_rl.count == SW_RL_HIST_MAX) {
        free(_sw_rl.entries[0]);
        memmove(_sw_rl.entries, _sw_rl.entries + 1, sizeof(char*) * (SW_RL_HIST_MAX - 1));
        _sw_rl.count--;
    }
    _sw_rl.entries[_sw_rl.count++] = strdup(line);
}

/* Redraw the current buffer: clear line, reprint prompt + buf, position cursor.
 * `cursor` is the byte offset within buf where the caret should end up. */
static void _sw_rl_redraw(const char *prompt, const char *buf, size_t len, size_t cursor) {
    fputs("\r\x1b[K", stdout);
    if (prompt) fputs(prompt, stdout);
    fwrite(buf, 1, len, stdout);
    if (cursor < len) {
        size_t back = len - cursor;
        printf("\x1b[%zuD", (size_t)back);
    }
    fflush(stdout);
}

static sw_val_t *_builtin_read_line(sw_val_t **a, int n) {
    const char *prompt = (n >= 1 && a[0] && a[0]->type == SW_VAL_STRING) ? a[0]->v.str : NULL;
    if (prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }

    /* Fall back to canonical line read if not a TTY (piped input, tests). */
    if (!_sw_rl_setup()) {
        size_t cap = 256, len = 0;
        char *buf = (char *)malloc(cap);
        int c;
        while ((c = fgetc(stdin)) != EOF && c != '\n') {
            if (len + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
            buf[len++] = (char)c;
        }
        if (c == EOF && len == 0) { free(buf); return sw_val_nil(); }
        buf[len] = '\0';
        sw_val_t *r = sw_val_string(buf);
        free(buf);
        return r;
    }

    /* Raw-mode line editor. */
    size_t cap = 512, len = 0, cursor = 0;
    char *buf = (char *)malloc(cap);
    buf[0] = '\0';
    int hist_idx = _sw_rl.count; /* one past the end = "new line" */
    /* When a multi-line paste lands in the buffer, we can't do simple
     * cursor-position redraws (they'd clobber earlier lines). Once this
     * flag is set, fall back to append-only behavior. */
    int has_newline = 0;

    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            if (len == 0) { free(buf); return sw_val_nil(); }
            break;
        }
        /* Ctrl+D (EOT) on empty line = EOF; otherwise forward-delete. */
        if (c == 4) {
            if (len == 0) { free(buf); fputc('\n', stdout); fflush(stdout); return sw_val_nil(); }
            if (!has_newline && cursor < len) {
                memmove(buf + cursor, buf + cursor + 1, len - cursor - 1);
                len--;
                buf[len] = '\0';
                _sw_rl_redraw(prompt, buf, len, cursor);
            }
            continue;
        }
        /* Enter (LF or CR) = submit. */
        if (c == '\n' || c == '\r') {
            fputc('\n', stdout);
            fflush(stdout);
            break;
        }
        /* Backspace (DEL or BS). */
        if (c == 127 || c == 8) {
            if (len > 0 && cursor > 0) {
                if (has_newline || cursor == len) {
                    /* Simple append-mode backspace: just pop the last char. */
                    len--;
                    cursor = len;
                    buf[len] = '\0';
                    fputs("\b \b", stdout);
                    fflush(stdout);
                } else {
                    /* Delete char before cursor, shift tail left. */
                    memmove(buf + cursor - 1, buf + cursor, len - cursor);
                    len--;
                    cursor--;
                    buf[len] = '\0';
                    _sw_rl_redraw(prompt, buf, len, cursor);
                }
            }
            continue;
        }
        /* Ctrl+A: jump to start of line. */
        if (c == 1) {
            if (!has_newline && cursor > 0) {
                cursor = 0;
                _sw_rl_redraw(prompt, buf, len, cursor);
            }
            continue;
        }
        /* Ctrl+E: jump to end of line. */
        if (c == 5) {
            if (!has_newline && cursor < len) {
                cursor = len;
                _sw_rl_redraw(prompt, buf, len, cursor);
            }
            continue;
        }
        /* Ctrl+K: kill from cursor to end of line. */
        if (c == 11) {
            if (!has_newline && cursor < len) {
                len = cursor;
                buf[len] = '\0';
                _sw_rl_redraw(prompt, buf, len, cursor);
            }
            continue;
        }
        /* Ctrl+U: clear line. */
        if (c == 21) {
            len = 0;
            cursor = 0;
            buf[0] = '\0';
            has_newline = 0;
            _sw_rl_redraw(prompt, buf, len, cursor);
            continue;
        }
        /* Ctrl+W: kill word before cursor. */
        if (c == 23) {
            if (!has_newline && cursor > 0) {
                size_t end = cursor;
                /* Skip trailing spaces */
                while (end > 0 && buf[end - 1] == ' ') end--;
                /* Skip the word */
                while (end > 0 && buf[end - 1] != ' ') end--;
                size_t removed = cursor - end;
                if (removed > 0) {
                    memmove(buf + end, buf + cursor, len - cursor);
                    len -= removed;
                    cursor = end;
                    buf[len] = '\0';
                    _sw_rl_redraw(prompt, buf, len, cursor);
                }
            }
            continue;
        }
        /* ESC: distinguish bare Esc (clear line) from escape sequences
         * (arrows, bracketed paste). Bare Esc has no follow-up byte;
         * sequences send the next byte within a couple of ms. */
        if (c == 27) {
            fd_set _erfds;
            FD_ZERO(&_erfds);
            FD_SET(STDIN_FILENO, &_erfds);
            struct timeval _etv = {0, 50000};  /* 50 ms */
            int _hasmore = select(STDIN_FILENO + 1, &_erfds, NULL, NULL, &_etv);
            if (_hasmore <= 0) {
                /* Bare Esc: clear current buffer without submitting.
                 * Matches Claude Code's "Esc clears input" behavior. */
                len = 0;
                cursor = 0;
                has_newline = 0;
                if (len + 1 > cap) { /* no-op, just for clarity */ }
                buf[0] = '\0';
                _sw_rl_redraw(prompt, buf, len, cursor);
                continue;
            }
            int c1 = fgetc(stdin);
            if (c1 != '[') continue;
            int c2 = fgetc(stdin);
            /* Arrow keys: ESC [ A (up) / B (down) / C (right) / D (left) */
            if (c2 == 'A') {
                if (_sw_rl.count > 0 && hist_idx > 0) {
                    hist_idx--;
                    const char *h = _sw_rl.entries[hist_idx];
                    size_t hlen = strlen(h);
                    if (hlen + 1 > cap) { while (hlen + 1 > cap) cap *= 2; buf = (char*)realloc(buf, cap); }
                    memcpy(buf, h, hlen);
                    buf[hlen] = '\0';
                    len = hlen;
                    cursor = hlen;
                    has_newline = (memchr(buf, '\n', len) != NULL);
                    _sw_rl_redraw(prompt, buf, len, cursor);
                }
                continue;
            }
            if (c2 == 'B') {
                if (hist_idx < _sw_rl.count) {
                    hist_idx++;
                    if (hist_idx == _sw_rl.count) {
                        /* Back to new empty line */
                        len = 0;
                        cursor = 0;
                        buf[0] = '\0';
                        has_newline = 0;
                    } else {
                        const char *h = _sw_rl.entries[hist_idx];
                        size_t hlen = strlen(h);
                        if (hlen + 1 > cap) { while (hlen + 1 > cap) cap *= 2; buf = (char*)realloc(buf, cap); }
                        memcpy(buf, h, hlen);
                        buf[hlen] = '\0';
                        len = hlen;
                        cursor = hlen;
                        has_newline = (memchr(buf, '\n', len) != NULL);
                    }
                    _sw_rl_redraw(prompt, buf, len, cursor);
                }
                continue;
            }
            /* Right arrow */
            if (c2 == 'C') {
                if (!has_newline && cursor < len) {
                    cursor++;
                    fputs("\x1b[C", stdout);
                    fflush(stdout);
                }
                continue;
            }
            /* Left arrow */
            if (c2 == 'D') {
                if (!has_newline && cursor > 0) {
                    cursor--;
                    fputs("\x1b[D", stdout);
                    fflush(stdout);
                }
                continue;
            }
            /* Home: ESC [ H */
            if (c2 == 'H') {
                if (!has_newline && cursor > 0) {
                    cursor = 0;
                    _sw_rl_redraw(prompt, buf, len, cursor);
                }
                continue;
            }
            /* End: ESC [ F */
            if (c2 == 'F') {
                if (!has_newline && cursor < len) {
                    cursor = len;
                    _sw_rl_redraw(prompt, buf, len, cursor);
                }
                continue;
            }
            /* ESC [ 1 ~ (Home alt), ESC [ 3 ~ (Delete), ESC [ 4 ~ (End alt) */
            if (c2 == '1' || c2 == '3' || c2 == '4' || c2 == '7' || c2 == '8') {
                int c3 = fgetc(stdin);
                if (c3 == '~') {
                    if (c2 == '3') {
                        /* Forward delete */
                        if (!has_newline && cursor < len) {
                            memmove(buf + cursor, buf + cursor + 1, len - cursor - 1);
                            len--;
                            buf[len] = '\0';
                            _sw_rl_redraw(prompt, buf, len, cursor);
                        }
                    } else if (c2 == '1' || c2 == '7') {
                        /* Home */
                        if (!has_newline && cursor > 0) {
                            cursor = 0;
                            _sw_rl_redraw(prompt, buf, len, cursor);
                        }
                    } else if (c2 == '4' || c2 == '8') {
                        /* End */
                        if (!has_newline && cursor < len) {
                            cursor = len;
                            _sw_rl_redraw(prompt, buf, len, cursor);
                        }
                    }
                    continue;
                }
                /* Bracketed paste start: ESC [ 2 0 0 ~ is handled below */
                if (c2 == '2' && c3 == '0') {
                    /* unreachable here — handled in dedicated branch */
                }
                continue;
            }
            /* Bracketed paste start: ESC [ 2 0 0 ~ */
            if (c2 == '2') {
                int c3 = fgetc(stdin);
                int c4 = fgetc(stdin);
                int c5 = fgetc(stdin);
                if (c3 == '0' && c4 == '0' && c5 == '~') {
                    /* Collect paste content verbatim until ESC [ 2 0 1 ~ */
                    /* Note: paste always goes to end of buffer — we don't
                     * try to insert at cursor for multi-line pastes. */
                    cursor = len;
                    for (;;) {
                        int pc = fgetc(stdin);
                        if (pc == EOF) break;
                        if (pc == 27) {
                            int p1 = fgetc(stdin);
                            if (p1 == '[') {
                                int p2 = fgetc(stdin);
                                if (p2 == '2') {
                                    int p3 = fgetc(stdin);
                                    int p4 = fgetc(stdin);
                                    int p5 = fgetc(stdin);
                                    if (p3 == '0' && p4 == '1' && p5 == '~') goto paste_done;
                                }
                            }
                            continue;
                        }
                        /* Append to buffer; echo so user sees what was pasted. */
                        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                        buf[len++] = (char)pc;
                        buf[len] = '\0';
                        if (pc == '\n' || pc == '\r') {
                            has_newline = 1;
                            /* Echo newline and an indent for visual clarity */
                            fputc('\n', stdout);
                            if (prompt) {
                                /* Align continuation lines with the prompt width — 2 spaces is a reasonable default */
                                fputs("  ", stdout);
                            }
                        } else {
                            fputc(pc, stdout);
                        }
                    }
                    paste_done:
                    cursor = len;
                    fflush(stdout);
                    continue;
                }
                /* Not a paste marker; discard the sequence. */
                continue;
            }
            /* Other escape sequences — ignore. */
            continue;
        }
        /* Printable character */
        if (c >= 0x20 && c != 0x7f) {
            if (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            if (has_newline || cursor == len) {
                /* Append at end — cheap path, no redraw. */
                buf[len++] = (char)c;
                cursor = len;
                buf[len] = '\0';
                fputc(c, stdout);
                fflush(stdout);
            } else {
                /* Insert at cursor position, shift tail right. */
                memmove(buf + cursor + 1, buf + cursor, len - cursor);
                buf[cursor] = (char)c;
                len++;
                cursor++;
                buf[len] = '\0';
                _sw_rl_redraw(prompt, buf, len, cursor);
            }
        }
        /* All other control chars: ignore */
    }

    buf[len] = '\0';
    if (len > 0) _sw_rl_history_push(buf);
    sw_val_t *r = sw_val_string(buf);
    free(buf);
    return r;
}

/* read_choice(header, options) → int index (or -1 on Esc/cancel)
 *
 * Interactive arrow-key picker. Prints the header, then renders the
 * option list with ❯ marking the current selection, accepts up/down
 * arrow keys + Enter to confirm + Esc to cancel + 1-9 numeric
 * shortcuts. Inspired by Claude Code's CustomSelect component.
 *
 * The rendered lines are repainted in place using cursor-up +
 * clear-to-EOL, so moving between options doesn't scroll the
 * terminal. Expected to be called only from the Reader process to
 * avoid racing with the main input loop on stdin.
 */
static sw_val_t *_builtin_read_choice(sw_val_t **a, int n) {
    if (n < 2) return sw_val_int(-1);
    const char *header = (a[0] && a[0]->type == SW_VAL_STRING) ? a[0]->v.str : "";
    sw_val_t *opts = a[1];
    if (!opts || opts->type != SW_VAL_LIST || opts->v.tuple.count == 0)
        return sw_val_int(-1);

    int count = opts->v.tuple.count;
    const char **labels = (const char **)malloc(sizeof(char *) * count);
    for (int i = 0; i < count; i++) {
        sw_val_t *v = opts->v.tuple.items[i];
        labels[i] = (v && v->type == SW_VAL_STRING) ? v->v.str : "";
    }

    /* Non-TTY fallback: can't do interactive picker, return first option. */
    if (!_sw_rl_setup() || !isatty(STDIN_FILENO)) {
        free(labels);
        return sw_val_int(0);
    }

    /* Print header once, then reserve `count` lines for the picker. */
    if (*header) {
        fputs(header, stdout);
        fputc('\n', stdout);
    }
    /* Pre-print blank lines so we're guaranteed to have space below us
     * before we start moving the cursor back up. */
    for (int i = 0; i < count; i++) fputc('\n', stdout);
    fflush(stdout);

    int cur = 0;
    int result = -1;

    for (;;) {
        /* Move cursor back up `count` lines to the first option line. */
        fprintf(stdout, "\x1b[%dA", count);
        for (int i = 0; i < count; i++) {
            fputs("\r\x1b[K", stdout);  /* clear line */
            if (i == cur) {
                fprintf(stdout,
                    "  \x1b[38;5;124m\x1b[1m❯\x1b[0m \x1b[1m%d. %s\x1b[0m\n",
                    i + 1, labels[i]);
            } else {
                fprintf(stdout,
                    "    \x1b[38;5;244m%d. %s\x1b[0m\n",
                    i + 1, labels[i]);
            }
        }
        fflush(stdout);

        int c = fgetc(stdin);
        if (c == EOF) { result = -1; break; }
        /* Ctrl+C, Ctrl+D = cancel */
        if (c == 3 || c == 4) { result = -1; break; }
        /* Enter = confirm current selection */
        if (c == '\n' || c == '\r') { result = cur; break; }
        /* Numeric shortcut 1..9 */
        if (c >= '1' && c <= '9') {
            int idx = c - '1';
            if (idx < count) { result = idx; break; }
            continue;
        }
        /* ESC — could be bare escape (cancel) or arrow sequence */
        if (c == 27) {
            /* Peek for more bytes via select with a short timeout. Bare
             * Esc has no follow-up; arrow keys send ESC [ A immediately. */
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            struct timeval tv = {0, 50000};  /* 50 ms */
            int has_more = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
            if (has_more <= 0) { result = -1; break; }  /* bare Esc */
            int c1 = fgetc(stdin);
            if (c1 != '[') continue;
            int c2 = fgetc(stdin);
            if (c2 == 'A') { if (cur > 0) cur--; }         /* up */
            else if (c2 == 'B') { if (cur < count - 1) cur++; } /* down */
            /* other escapes (home/end/etc) ignored */
            continue;
        }
        /* j/k vim-style */
        if (c == 'j') { if (cur < count - 1) cur++; continue; }
        if (c == 'k') { if (cur > 0) cur--; continue; }
        /* anything else: ignore */
    }

    free(labels);
    return sw_val_int(result);
}

/* print_inline(args...) → 'ok'
 * Like print, but without a trailing newline. */
static sw_val_t *_builtin_print_inline(sw_val_t **a, int n) {
    for (int i = 0; i < n; i++) {
        if (i) printf(" ");
        sw_val_print(a[i]);
    }
    fflush(stdout);
    return sw_val_atom("ok");
}

/* sys_exit(code?) → never returns
 * Cleanly terminate the process. Essential for CLIs because the
 * runtime's main loop otherwise spins forever waiting for processes. */
static sw_val_t *_builtin_sys_exit(sw_val_t **a, int n) {
    int code = 0;
    if (n >= 1 && a[0] && a[0]->type == SW_VAL_INT) code = (int)a[0]->v.i;
    fflush(stdout);
    fflush(stderr);
    exit(code);
    return sw_val_nil(); /* unreachable */
}

/* ============================================================
 * WebSocket CLIENT — for talking to chrome's CDP (and anything else
 *                     that speaks RFC 6455)
 * ============================================================
 *
 * The existing ws_send / ws_close / ws_set_handler builtins above
 * are SERVER-side (they operate on connections accepted by the HTTP
 * server). What we need for CDP is the inverse — a client that does
 * the RFC 6455 handshake against a remote server, sends/receives
 * masked text frames, and gets us a JSON-over-WS pipe to chrome.
 *
 * Builtins exposed:
 *   wsc_connect(ws_url)          → int handle, or nil
 *   wsc_send(handle, text)       → 'ok' | 'error'
 *   wsc_recv(handle, timeout_ms) → string | nil
 *   wsc_close(handle)            → 'ok'
 *
 * The handle is a small integer index into a fixed pool — ample for
 * a single browser session (one WS per page target). No
 * Sec-WebSocket-Accept validation in v1: we just accept any 101.
 * Chrome plays well; tighten later if we point this at adversarial
 * servers. */

#include <netdb.h>

#define _SW_WSC_MAX 16

typedef struct {
    int fd;
    int used;
} _sw_wsc_slot_t;
static _sw_wsc_slot_t _sw_wsc[_SW_WSC_MAX] = {0};

static int _sw_wsc_alloc_slot(int fd) {
    for (int i = 0; i < _SW_WSC_MAX; i++) {
        if (!_sw_wsc[i].used) {
            _sw_wsc[i].fd = fd;
            _sw_wsc[i].used = 1;
            return i;
        }
    }
    return -1;
}

static void _sw_wsc_free_slot(int handle) {
    if (handle >= 0 && handle < _SW_WSC_MAX) {
        _sw_wsc[handle].used = 0;
        _sw_wsc[handle].fd = -1;
    }
}

/* Local base64 encoder — swarmrt_http.c has its own but it's static.
 * Inlined here so we don't have to relax that file's encapsulation
 * for one caller. Standard table; output is null-terminated. */
static void _sw_b64_encode(const uint8_t *in, int len, char *out) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = tbl[(in[i] >> 2) & 0x3f];
        out[j++] = tbl[((in[i] & 0x03) << 4) | ((in[i+1] >> 4) & 0x0f)];
        out[j++] = tbl[((in[i+1] & 0x0f) << 2) | ((in[i+2] >> 6) & 0x03)];
        out[j++] = tbl[in[i+2] & 0x3f];
    }
    if (i < len) {
        out[j++] = tbl[(in[i] >> 2) & 0x3f];
        if (i + 1 < len) {
            out[j++] = tbl[((in[i] & 0x03) << 4) | ((in[i+1] >> 4) & 0x0f)];
            out[j++] = tbl[(in[i+1] & 0x0f) << 2];
        } else {
            out[j++] = tbl[(in[i] & 0x03) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = 0;
}

/* Send exactly `len` bytes; loops until done or error. */
static int _sw_wsc_send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Recv exactly `len` bytes (blocking). */
static int _sw_wsc_recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n == 0) return -1;       /* peer closed */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* wsc_connect(url) — parse ws://host[:port][/path], TCP connect, do
 * the upgrade handshake, store fd in our slot pool. */
static sw_val_t *_builtin_wsc_connect(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    const char *url = a[0]->v.str;
    if (strncmp(url, "ws://", 5) != 0) return sw_val_nil();
    const char *p = url + 5;

    char host[256] = {0};
    int port = 80;
    char path[2048] = "/";

    /* Find host[:port] up to first / */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    const char *host_end = slash ? slash : (p + strlen(p));
    if (colon && colon < host_end) {
        size_t hl = (size_t)(colon - p);
        if (hl >= sizeof(host)) return sw_val_nil();
        memcpy(host, p, hl); host[hl] = 0;
        port = atoi(colon + 1);
    } else {
        size_t hl = (size_t)(host_end - p);
        if (hl >= sizeof(host)) return sw_val_nil();
        memcpy(host, p, hl); host[hl] = 0;
    }
    if (slash) snprintf(path, sizeof(path), "%s", slash);

    /* Resolve + connect TCP. */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return sw_val_nil();
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return sw_val_nil(); }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return sw_val_nil();
    }
    freeaddrinfo(res);

    /* Generate 16 random bytes → base64 = Sec-WebSocket-Key. */
    uint8_t key_raw[16];
    for (int i = 0; i < 16; i++) key_raw[i] = (uint8_t)(rand() & 0xff);
    char key_b64[32] = {0};
    _sw_b64_encode(key_raw, 16, key_b64);

    /* Handshake request. */
    char req[4096];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, port, key_b64);
    if (_sw_wsc_send_all(fd, req, (size_t)rl) < 0) { close(fd); return sw_val_nil(); }

    /* Read response headers until "\r\n\r\n". 4KB cap is plenty. */
    char resp[4096] = {0};
    size_t rlen = 0;
    while (rlen < sizeof(resp) - 1) {
        ssize_t got = recv(fd, resp + rlen, sizeof(resp) - 1 - rlen, 0);
        if (got <= 0) { close(fd); return sw_val_nil(); }
        rlen += (size_t)got;
        resp[rlen] = 0;
        if (strstr(resp, "\r\n\r\n")) break;
    }
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) { close(fd); return sw_val_nil(); }

    int handle = _sw_wsc_alloc_slot(fd);
    if (handle < 0) { close(fd); return sw_val_nil(); }
    return sw_val_int(handle);
}

/* wsc_send(handle, text) — text frame, masked (clients MUST mask). */
static sw_val_t *_builtin_wsc_send(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || !a[1]) return sw_val_atom("error");
    if (a[0]->type != SW_VAL_INT || a[1]->type != SW_VAL_STRING) return sw_val_atom("error");
    int handle = (int)a[0]->v.i;
    if (handle < 0 || handle >= _SW_WSC_MAX || !_sw_wsc[handle].used) return sw_val_atom("error");
    int fd = _sw_wsc[handle].fd;

    const char *text = a[1]->v.str;
    size_t len = strlen(text);

    uint8_t hdr[14];
    int hlen = 0;
    hdr[hlen++] = 0x81;  /* FIN + opcode=text */
    if (len < 126) {
        hdr[hlen++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        hdr[hlen++] = 0x80 | 126;
        hdr[hlen++] = (uint8_t)(len >> 8);
        hdr[hlen++] = (uint8_t)(len & 0xff);
    } else {
        hdr[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[hlen++] = (uint8_t)((uint64_t)len >> (i * 8));
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xff);
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;
    if (_sw_wsc_send_all(fd, hdr, (size_t)hlen) < 0) return sw_val_atom("error");

    /* Mask payload in 4KB chunks. */
    char chunk[4096];
    size_t off = 0;
    while (off < len) {
        size_t cn = len - off;
        if (cn > sizeof(chunk)) cn = sizeof(chunk);
        for (size_t i = 0; i < cn; i++) chunk[i] = text[off + i] ^ mask[(off + i) & 3];
        if (_sw_wsc_send_all(fd, chunk, cn) < 0) return sw_val_atom("error");
        off += cn;
    }
    return sw_val_atom("ok");
}

/* wsc_recv(handle, timeout_ms) — receive one text frame.
 * Returns the payload as a string, or nil on timeout / error / close.
 * Auto-replies to ping with pong. Skips control frames except close. */
static sw_val_t *_builtin_wsc_recv(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_nil();
    int handle = (int)a[0]->v.i;
    if (handle < 0 || handle >= _SW_WSC_MAX || !_sw_wsc[handle].used) return sw_val_nil();
    int fd = _sw_wsc[handle].fd;

    int timeout_ms = -1;  /* default: block forever */
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_INT) timeout_ms = (int)a[1]->v.i;

    /* Wait for fd to be readable. */
    if (timeout_ms >= 0) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) return sw_val_nil();
    }

    /* Read frame header: opcode/flags + payload-len byte. */
    uint8_t hdr[2];
    if (_sw_wsc_recv_all(fd, hdr, 2) < 0) return sw_val_nil();
    int opcode = hdr[0] & 0x0f;
    int masked = hdr[1] & 0x80;
    uint64_t plen = hdr[1] & 0x7f;
    if (plen == 126) {
        uint8_t ext[2];
        if (_sw_wsc_recv_all(fd, ext, 2) < 0) return sw_val_nil();
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (_sw_wsc_recv_all(fd, ext, 8) < 0) return sw_val_nil();
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }
    uint8_t mask[4] = {0};
    if (masked) {
        if (_sw_wsc_recv_all(fd, mask, 4) < 0) return sw_val_nil();
    }
    char *payload = (char *)malloc((size_t)plen + 1);
    if (!payload) return sw_val_nil();
    if (_sw_wsc_recv_all(fd, payload, (size_t)plen) < 0) { free(payload); return sw_val_nil(); }
    if (masked) {
        for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
    }
    payload[plen] = 0;

    if (opcode == 0x8) {  /* close */
        free(payload);
        return sw_val_nil();
    }
    if (opcode == 0x9) {  /* ping → pong with same payload */
        uint8_t ph[2] = {0x8a, 0x80 | (uint8_t)(plen < 126 ? plen : 0)};
        _sw_wsc_send_all(fd, ph, 2);
        uint8_t pmask[4] = {0,0,0,0};
        _sw_wsc_send_all(fd, pmask, 4);
        if (plen > 0 && plen < 126) _sw_wsc_send_all(fd, payload, (size_t)plen);
        free(payload);
        /* Recurse to get the next real frame. */
        return _builtin_wsc_recv(a, n);
    }
    if (opcode != 0x1 && opcode != 0x0) {
        /* Binary or unknown — return nil for v1. */
        free(payload);
        return sw_val_nil();
    }

    sw_val_t *r = sw_val_string(payload);
    free(payload);
    return r;
}

/* wsc_close(handle) — send close frame, close socket, free slot. */
static sw_val_t *_builtin_wsc_close(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    int handle = (int)a[0]->v.i;
    if (handle < 0 || handle >= _SW_WSC_MAX || !_sw_wsc[handle].used) return sw_val_atom("error");
    int fd = _sw_wsc[handle].fd;
    /* Best-effort close frame. */
    uint8_t close_frame[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
    _sw_wsc_send_all(fd, close_frame, sizeof(close_frame));
    close(fd);
    _sw_wsc_free_slot(handle);
    return sw_val_atom("ok");
}

/* ============================================================
 * chrome_launch — find a Chromium binary, spawn detached with
 *                 --remote-debugging-port set, wait for the port
 *                 to answer /json/version. Return the port as int
 *                 (so caller can build URLs) or nil on failure.
 * ============================================================
 *
 * Looked-for binaries (first hit wins):
 *   macOS:  /Applications/Google Chrome.app/Contents/MacOS/Google Chrome
 *           /Applications/Chromium.app/Contents/MacOS/Chromium
 *           /Applications/Brave Browser.app/Contents/MacOS/Brave Browser
 *           /Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge
 *   Linux:  /usr/bin/google-chrome
 *           /usr/bin/chromium  /usr/bin/chromium-browser
 *           /snap/bin/chromium
 *
 * Args we always pass:
 *   --remote-debugging-port=<port>
 *   --user-data-dir=/tmp/swc-chrome-<port>     (isolated profile)
 *   --no-first-run --no-default-browser-check  (no welcome modals)
 *   --disable-dev-shm-usage --disable-gpu      (headless friendly)
 *   --headless=new                             (unless headless='false') */

static const char *_sw_chrome_candidates[] = {
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    "/Applications/Arc.app/Contents/MacOS/Arc",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
    "/snap/bin/chromium",
    NULL
};

/* Held across calls so we don't re-glob Playwright's cache every time.
 * NULL = haven't searched; "" = searched and found nothing (don't retry). */
static char _sw_chrome_cached[1024] = {0};
static int _sw_chrome_cache_set = 0;

/* Resolve a Chromium binary. Search order:
 *   1. SWARM_CODE_CHROME env var (explicit override, never cached)
 *   2. Standard install paths (table above)
 *   3. Playwright's cache at ~/Library/Caches/ms-playwright/chromium-*
 *      (so users who've ever run `npx playwright install` get free
 *      browser availability — no extra download)
 *   4. Linux Playwright cache at ~/.cache/ms-playwright/chromium-*
 * Returns a static buffer or NULL. */
static const char *_sw_find_chrome(void) {
    /* (1) env override always wins, no caching. */
    const char *env = getenv("SWARM_CODE_CHROME");
    if (env && *env && access(env, X_OK) == 0) return env;

    if (_sw_chrome_cache_set) {
        return _sw_chrome_cached[0] ? _sw_chrome_cached : NULL;
    }

    /* (2) standard install paths */
    for (int i = 0; _sw_chrome_candidates[i]; i++) {
        if (access(_sw_chrome_candidates[i], X_OK) == 0) {
            snprintf(_sw_chrome_cached, sizeof(_sw_chrome_cached), "%s",
                     _sw_chrome_candidates[i]);
            _sw_chrome_cache_set = 1;
            return _sw_chrome_cached;
        }
    }

    /* (3,4) Playwright caches — glob via shell because there's a version
     * number in the path we don't know in advance. Newest wins. */
    const char *home = getenv("HOME");
    if (!home) home = "";
    char glob_cmd[2048];
    snprintf(glob_cmd, sizeof(glob_cmd),
        "ls -t "
        "\"%s/Library/Caches/ms-playwright/chromium-\"*\"/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing\" "
        "\"%s/Library/Caches/ms-playwright/chromium-\"*\"/chrome-mac/Chromium.app/Contents/MacOS/Chromium\" "
        "\"%s/.cache/ms-playwright/chromium-\"*\"/chrome-linux/chrome\" "
        "2>/dev/null | head -n 1",
        home, home, home);
    FILE *p = popen(glob_cmd, "r");
    if (p) {
        char line[1024];
        if (fgets(line, sizeof(line), p)) {
            size_t l = strlen(line);
            while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
            if (l > 0 && access(line, X_OK) == 0) {
                snprintf(_sw_chrome_cached, sizeof(_sw_chrome_cached), "%s", line);
                _sw_chrome_cache_set = 1;
                pclose(p);
                return _sw_chrome_cached;
            }
        }
        pclose(p);
    }

    _sw_chrome_cache_set = 1;   /* mark as searched-and-empty */
    return NULL;
}

/* chrome_launch(port?, headless?) — port default 9222, headless default 'true'.
 * Returns the port as int on success, nil on failure. Idempotent: if a
 * chrome is already listening on that port, we accept it. */
static sw_val_t *_builtin_chrome_launch(sw_val_t **a, int n) {
    int port = 9222;
    int headless = 1;
    if (n >= 1 && a[0] && a[0]->type == SW_VAL_INT) port = (int)a[0]->v.i;
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_ATOM &&
        strcmp(a[1]->v.str, "false") == 0) headless = 0;

    /* If something already answers on that port, accept it. */
    char check[256];
    snprintf(check, sizeof(check),
        "curl -sf -m 1 http://127.0.0.1:%d/json/version > /dev/null 2>&1", port);
    if (system(check) == 0) return sw_val_int(port);

    const char *bin = _sw_find_chrome();
    if (!bin) return sw_val_nil();

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "\"%s\" --remote-debugging-port=%d "
        "--user-data-dir=/tmp/swc-chrome-%d "
        "--no-first-run --no-default-browser-check "
        "--disable-dev-shm-usage --disable-gpu "
        "%s "
        "about:blank "
        "> /tmp/swc-chrome-%d.log 2>&1 &",
        bin, port, port, headless ? "--headless=new" : "", port);

    if (system(cmd) != 0) return sw_val_nil();

    /* Poll up to 10 seconds for the debug port to come up. */
    for (int i = 0; i < 100; i++) {
        if (system(check) == 0) return sw_val_int(port);
        usleep(100000);
    }
    return sw_val_nil();
}

/* ============================================================
 * String + Base64 helpers (added 2026-05-15)
 * ============================================================
 * Three high-leverage builtins that kept getting reinvented in
 * userland: substring search, base64 encode/decode. */

/* string_index_of(haystack, needle) → int (-1 if not found, 0-based byte
 * offset otherwise). Naive O(n*m) scan; fine for the prose-sized strings
 * agent code passes around. */
static sw_val_t *_builtin_string_index_of(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || !a[1]) return sw_val_int(-1);
    if (a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_STRING)
        return sw_val_int(-1);
    const char *hay = a[0]->v.str;
    const char *needle = a[1]->v.str;
    if (!*needle) return sw_val_int(0);
    const char *hit = strstr(hay, needle);
    if (!hit) return sw_val_int(-1);
    return sw_val_int((int64_t)(hit - hay));
}

/* base64_encode(bytes_or_string) → string (no newlines, padded with '=').
 * The existing `base64_encode` C helper in swarmrt_http.c is static; this
 * exposes it (locally re-implemented above in _sw_b64_encode) as a
 * builtin so sw code can encode without shelling out. */
static sw_val_t *_builtin_base64_encode(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_string("");
    const char *src = a[0]->v.str;
    size_t slen = strlen(src);
    size_t out_cap = (slen / 3 + 1) * 4 + 1;
    char *out = (char *)malloc(out_cap);
    _sw_b64_encode((const uint8_t *)src, (int)slen, out);
    sw_val_t *r = sw_val_string(out);
    free(out);
    return r;
}

/* base64_decode(string) → string (decoded bytes; nil on malformed input).
 * Tolerates whitespace and missing padding. Output is null-terminated so
 * sw_val_string is safe even for binary payloads — but binary data with
 * embedded NULs will be truncated at the first NUL when treated as a
 * string. For PNG/JPG decoding, write to file via file_write. */
static sw_val_t *_builtin_base64_decode(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    const char *src = a[0]->v.str;
    size_t slen = strlen(src);
    /* Inverse table: char → 0..63, or -1 for skip, or -2 for invalid. */
    static const signed char dec[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
    };
    size_t out_cap = (slen / 4 + 1) * 3 + 1;
    char *out = (char *)malloc(out_cap);
    size_t out_len = 0;
    int buf = 0, bits = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        signed char v = dec[c];
        if (v < 0) {
            /* unknown char — treat as bad input */
            free(out);
            return sw_val_nil();
        }
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[out_len++] = (char)((buf >> bits) & 0xff);
        }
    }
    out[out_len] = 0;
    sw_val_t *r = sw_val_string(out);
    free(out);
    return r;
}

#endif /* SWARMRT_BUILTINS_STUDIO_H */
