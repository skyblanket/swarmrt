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
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>

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
        case SW_VAL_MAP: return sw_val_string("map");
        default: return sw_val_string("unknown");
    }
}

static sw_val_t *_builtin_to_string(sw_val_t **a, int n) {
    if (n < 1) return sw_val_string("");
    char buf[4096];
    switch (a[0]->type) {
        case SW_VAL_INT: snprintf(buf, sizeof(buf), "%lld", (long long)a[0]->v.i); break;
        case SW_VAL_FLOAT: snprintf(buf, sizeof(buf), "%g", a[0]->v.f); break;
        case SW_VAL_STRING: return a[0];
        case SW_VAL_ATOM: return sw_val_string(a[0]->v.str);
        case SW_VAL_NIL: return sw_val_string("nil");
        default: snprintf(buf, sizeof(buf), "<val:%d>", a[0]->type); break;
    }
    return sw_val_string(buf);
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
    return sw_val_int(lo + (int64_t)(arc4random_uniform((uint32_t)(hi - lo + 1))));
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
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
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

/* === HTTP POST via curl with retry === */

static sw_val_t *_http_post_once(const char *url, sw_val_t *headers, const char *body,
                                  char *resp_out, size_t resp_cap) {
    size_t cmdcap = strlen(body) + strlen(url) + 4096;
    char *cmd = (char *)malloc(cmdcap);
    int off = 0;
    off += snprintf(cmd + off, cmdcap - off, "curl -sS -X POST --connect-timeout 30 --max-time 120");

    if (headers && headers->type == SW_VAL_LIST) {
        for (int i = 0; i < headers->v.tuple.count; i++) {
            sw_val_t *h = headers->v.tuple.items[i];
            if (h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->v.str && h->v.tuple.items[1]->v.str)
                off += snprintf(cmd + off, cmdcap - off, " -H '%s: %s'",
                    h->v.tuple.items[0]->v.str, h->v.tuple.items[1]->v.str);
        }
    }

    char tmpf[256], outf[256];
    snprintf(tmpf, sizeof(tmpf), "/tmp/sw_http_%d_%u.json", getpid(), arc4random());
    snprintf(outf, sizeof(outf), "/tmp/sw_http_out_%d_%u.json", getpid(), arc4random());
    FILE *tf = fopen(tmpf, "w");
    if (tf) { fputs(body, tf); fclose(tf); }
    off += snprintf(cmd + off, cmdcap - off, " -d @%s '%s' -o %s 2>/dev/null", tmpf, url, outf);

    int status = system(cmd);
    free(cmd);
    unlink(tmpf);

    size_t rlen = 0;
    resp_out[0] = 0;
    FILE *fp = fopen(outf, "r");
    if (fp) {
        rlen = fread(resp_out, 1, resp_cap - 1, fp);
        resp_out[rlen] = 0;
        fclose(fp);
    }
    unlink(outf);

    return NULL; /* just use rlen via resp_out */
}

static sw_val_t *_builtin_http_post(sw_val_t **a, int n) {
    if (n < 3 || a[0]->type != SW_VAL_STRING || a[2]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *url = a[0]->v.str, *body = a[2]->v.str;
    size_t rcap = 524288;
    char *resp = (char *)malloc(rcap);

    /* Retry up to 3 times with exponential backoff */
    int delays[] = {0, 5, 15};
    for (int attempt = 0; attempt < 3; attempt++) {
        if (delays[attempt] > 0)
            sleep(delays[attempt]);
        _http_post_once(url, a[1], body, resp, rcap);
        size_t rlen = strlen(resp);
        /* Check for server error or empty response */
        if (rlen > 0 && strstr(resp, "Internal Server Error") == NULL &&
            strstr(resp, "\"error\"") == NULL) {
            sw_val_t *r = sw_val_string(resp);
            free(resp);
            return r;
        }
        (void)0; /* retry */
    }
    /* Return whatever we got after retries */
    sw_val_t *r = sw_val_string(resp);
    free(resp);
    return r;
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
static void _json_encode_val(sw_val_t *v, char *buf, size_t cap, size_t *pos);

/* node_send(node_name, reg_name, msg) → 'ok' | 'error'
 * Serializes msg sw_val_t to JSON and sends via sw_node_send. */
static sw_val_t *_builtin_node_send(sw_val_t **a, int n) {
    if (n < 3) return sw_val_atom("error");
    if (a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_STRING)
        return sw_val_atom("error");
    /* Serialize sw_val_t to JSON for cross-node transport */
    size_t cap = 262144;
    char *buf = (char *)malloc(cap);
    size_t pos = 0;
    _json_encode_val(a[2], buf, cap, &pos);
    buf[pos] = 0;
    int ok = sw_node_send(a[0]->v.str, a[1]->v.str, SW_TAG_NONE,
                          buf, (uint32_t)(pos + 1));
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

/* map_has_key(map, key) → 'true' | 'false' */
static sw_val_t *_builtin_map_has_key(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_MAP) return sw_val_atom("false");
    for (int i = 0; i < a[0]->v.map.count; i++)
        if (sw_val_equal(a[0]->v.map.keys[i], a[1])) return sw_val_atom("true");
    return sw_val_atom("false");
}

/* === Error mechanism for try/catch === */

/* error(reason) — sets thread-local error, caught by try/catch */
static sw_val_t *_builtin_error(sw_val_t **a, int n) {
    extern __thread sw_val_t *_sw_error;
    _sw_error = (n >= 1) ? a[0] : sw_val_string("error");
    return sw_val_nil();
}

/* ================================================================
 * Phase 13: Agent Stdlib Batteries
 *
 * http_get, shell, json_encode, json_decode, file_exists, file_list,
 * file_delete, after, every, llm_complete, string_split, string_trim,
 * string_upper, string_lower, string_starts_with, string_ends_with
 * ================================================================ */

/* === HTTP GET with retry === */

static sw_val_t *_builtin_http_get(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *url = a[0]->v.str;
    size_t rcap = 524288;
    char *resp = (char *)malloc(rcap);

    size_t cmdcap = strlen(url) + 4096;
    char *cmd = (char *)malloc(cmdcap);

    char outf[256];
    snprintf(outf, sizeof(outf), "/tmp/sw_http_out_%d_%u.json", getpid(), arc4random());

    int off = 0;
    off += snprintf(cmd + off, cmdcap - off,
        "curl -sS --connect-timeout 30 --max-time 120");

    /* Optional headers (second arg) */
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

    int delays[] = {0, 3, 10};
    for (int attempt = 0; attempt < 3; attempt++) {
        if (delays[attempt] > 0) sleep(delays[attempt]);
        system(cmd);
        resp[0] = 0;
        FILE *fp = fopen(outf, "r");
        if (fp) {
            size_t rlen = fread(resp, 1, rcap - 1, fp);
            resp[rlen] = 0;
            fclose(fp);
            if (rlen > 0) { unlink(outf); break; }
        }
    }
    unlink(outf);
    free(cmd);
    sw_val_t *r = sw_val_string(resp);
    free(resp);
    return r;
}

/* === Shell: run command, capture stdout === */

static sw_val_t *_builtin_shell(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *cmd = a[0]->v.str;
    FILE *fp = popen(cmd, "r");
    if (!fp) return sw_val_nil();

    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    while (len < cap - 1) {
        size_t rd = fread(buf + len, 1, cap - len - 1, fp);
        if (rd == 0) break;
        len += rd;
    }
    buf[len] = 0;
    int status = pclose(fp);

    /* Return {status, output} tuple */
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * 2);
    items[0] = sw_val_int(WEXITSTATUS(status));
    items[1] = sw_val_string(buf);
    sw_val_t *r = sw_val_tuple(items, 2);
    free(buf);
    free(items);
    return r;
}

/* === JSON encode: sw_val_t → JSON string === */

static void _json_encode_val(sw_val_t *v, char *buf, size_t cap, size_t *pos);

static void _json_append(char *buf, size_t cap, size_t *pos, const char *s) {
    size_t len = strlen(s);
    if (*pos + len < cap) { memcpy(buf + *pos, s, len); *pos += len; }
}

static void _json_encode_val(sw_val_t *v, char *buf, size_t cap, size_t *pos) {
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
        /* Escape string contents */
        for (const char *p = v->v.str; *p && *pos < cap - 6; p++) {
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
                        buf[(*pos)++] = *p;
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

/* json_encode(val) → JSON string */
static sw_val_t *_builtin_json_encode(sw_val_t **a, int n) {
    if (n < 1) return sw_val_string("null");
    size_t cap = 262144;
    char *buf = (char *)malloc(cap);
    size_t pos = 0;
    _json_encode_val(a[0], buf, cap, &pos);
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

static sw_val_t *_json_parse_string(const char **pp) {
    (*pp)++; /* skip opening " */
    size_t cap = 4096;
    char *buf = (char *)malloc(cap);
    size_t len = 0;
    while (**pp && **pp != '"') {
        if (**pp == '\\') {
            (*pp)++;
            switch (**pp) {
                case '"': case '\\': case '/': buf[len++] = **pp; break;
                case 'n': buf[len++] = '\n'; break;
                case 'r': buf[len++] = '\r'; break;
                case 't': buf[len++] = '\t'; break;
                default: buf[len++] = **pp; break;
            }
        } else {
            buf[len++] = **pp;
        }
        (*pp)++;
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

    /* Defaults */
    const char *model = "otonomy-orc";
    const char *api_key = NULL;
    const char *url = "https://otonomy-inference-production.up.railway.app/v1/chat/completions";
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
    if (!api_key) return sw_val_string("error: no API key (set OTONOMY_API_KEY or pass api_key in opts)");

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
        if (attempt > 0) sleep(2); /* 2s backoff between retries */

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
        /* Handle trailing delimiter */
        if (*p == 0) { items[cnt++] = sw_val_string(""); break; }
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
    snprintf(tmpf, sizeof(tmpf), "/tmp/sw_llm_stream_%d_%u.json", getpid(), arc4random());
    FILE *tf = fopen(tmpf, "w");
    if (tf) { fputs(body, tf); fclose(tf); }
    free(body);

    snprintf(cmd, cmd_cap,
        "curl -sS -N --connect-timeout 30 --max-time 300 "
        "-H 'Content-Type: application/json' "
        "-H 'Authorization: Bearer %s' "
        "-d @%s '%s' 2>/dev/null",
        ctx->api_key, tmpf, ctx->url);

    FILE *fp = popen(cmd, "r");
    free(cmd);
    unlink(tmpf);

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

    pclose(fp);

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

/* ets_list() → list of table IDs */
static sw_val_t *_builtin_ets_list(sw_val_t **a, int n) {
    (void)a; (void)n;
    int cap = 64, cnt = 0;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);

    /* Use sw_ets_list_tids from swarmrt_ets.c */
    uint32_t tids[256];
    int ntids = sw_ets_list_tids(tids, 256);
    for (int i = 0; i < ntids; i++) {
        if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
        items[cnt++] = sw_val_int((int64_t)tids[i]);
    }

    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

/* ets_count(table_id) → int */
static sw_val_t *_builtin_ets_count(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_int(-1);
    int count = sw_ets_info_count((sw_ets_tid_t)a[0]->v.i);
    return sw_val_int((int64_t)count);
}

#endif /* SWARMRT_BUILTINS_STUDIO_H */
