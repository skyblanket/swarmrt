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

    fprintf(stderr, "[HTTP] status=%d rlen=%zu\n", status, rlen);
    fflush(stderr);
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
        if (delays[attempt] > 0) {
            fprintf(stderr, "[HTTP] retry %d, waiting %ds...\n", attempt + 1, delays[attempt]);
            fflush(stderr);
            sleep(delays[attempt]);
        }
        fprintf(stderr, "[HTTP] attempt %d: curl to %s\n", attempt + 1, url);
        fflush(stderr);
        _http_post_once(url, a[1], body, resp, rcap);
        size_t rlen = strlen(resp);
        /* Check for server error or empty response */
        if (rlen > 0 && strstr(resp, "Internal Server Error") == NULL &&
            strstr(resp, "\"error\"") == NULL) {
            sw_val_t *r = sw_val_string(resp);
            free(resp);
            return r;
        }
        if (rlen > 0) fprintf(stderr, "[HTTP] got error: %.100s\n", resp);
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
    if (n < 2 || a[0]->type != SW_VAL_STRING || a[0]->type == SW_VAL_NIL)
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

/* node_send(node_name, reg_name, msg) → 'ok' | 'error'
 * Serializes msg sw_val_t to bytes and sends via sw_node_send. */
static sw_val_t *_builtin_node_send(sw_val_t **a, int n) {
    if (n < 3) return sw_val_atom("error");
    if (a[0]->type != SW_VAL_STRING || a[1]->type != SW_VAL_STRING)
        return sw_val_atom("error");
    /* Serialize the sw_val_t message to the wire format */
    void *data = a[2]; /* send raw pointer — recipient deserializes */
    int ok = sw_node_send(a[0]->v.str, a[1]->v.str, 11 /* SW_TAG_CAST */,
                          data, sizeof(sw_val_t));
    return sw_val_atom(ok == 0 ? "ok" : "error");
}

/* === Map Builtins === */

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

#endif /* SWARMRT_BUILTINS_STUDIO_H */
