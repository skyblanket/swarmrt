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
#include "swarmrt_phase4.h"   /* sw_dynsup_* — DynamicSupervisor (runtime start_child) */
#include "swarmrt_http.h"
#include "swarmrt_varena.h"   /* varena->total_bytes — process_info 'memory' stat */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#ifndef _WIN32
  #include <sys/select.h>
  #include <sys/ioctl.h>
#endif
#include "swarmrt_platform.h"
#include "swarmrt_audio.h"   /* G.711 mu-law / PCM16 / resample (base64 in/out) */
#include <sqlite3.h>

/* Optional OpenSSL for wss:// (WebSocket-over-TLS) in the WS client.
 *
 * On Linux -lssl/-lcrypto are already linked (Makefile + swc.c), so TLS
 * is on by default there. On macOS the build historically uses Apple
 * CommonCrypto and does NOT link OpenSSL, so wss is OFF unless the build
 * opts in with -DSWARMRT_TLS (and links Homebrew openssl@3). When TLS is
 * unavailable, wsc_connect_tls() still works for ws:// + custom headers
 * and returns nil for wss:// with a one-line stderr note — additive,
 * never breaks the build. */
#ifndef SWARMRT_TLS
#  ifndef __APPLE__
#    define SWARMRT_TLS 1
#  endif
#endif
#ifdef SWARMRT_TLS
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#  include <openssl/evp.h>   /* EVP_PKEY ED25519 + EVP_DigestVerify (ed25519_verify) */
#endif
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
  #include <signal.h>
  #include <fcntl.h>
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
        case SW_VAL_BYTES: {
            for (size_t i = 0; i < v->v.bytes.len; i++) { h ^= v->v.bytes.data[i]; h *= 1099511628211ULL; }
            break;
        }
        case SW_VAL_TUPLE: {
            for (int i = 0; i < v->v.tuple.count; i++) h ^= _vets_hash_val(v->v.tuple.items[i]) * (i + 1);
            break;
        }
        case SW_VAL_PID: {
            /* Hash by the NUMERIC pid id — must match _vets_key_eq, which
             * compares pids by id. Hashing the pointer here (the old default)
             * broke the eq⇒same-bucket invariant: two equal pids that are
             * different arena-copied sw_val_t landed in different buckets yet
             * compared equal, so a pid-keyed put never found the prior entry
             * and the table accumulated duplicates + mislooked-up. */
            uint64_t k = v->v.pid ? v->v.pid->pid : 0;
            for (int i = 0; i < 8; i++) { h ^= (k & 0xff); h *= 1099511628211ULL; k >>= 8; }
            break;
        }
        default: {
            /* FLOAT / BOOL / NIL / LIST / MAP: no value-hash + eq is pointer
             * identity (below), but the compiled path deep-copies the key on
             * insert, so a later lookup key never shares the stored pointer —
             * these key types do NOT reliably match. ETS keys must be scalars
             * (int/atom/string/bytes/pid) or tuples of those (documented in
             * SW_LANGUAGE.md). Kept pointer-based rather than made a hard error
             * to avoid a behavior break; the contract is the guard. */
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
        case SW_VAL_BYTES:  /* NUL-safe: length-first then memcmp */
            return a->v.bytes.len == b->v.bytes.len &&
                   memcmp(a->v.bytes.data, b->v.bytes.data, a->v.bytes.len) == 0;
        case SW_VAL_TUPLE:
            if (a->v.tuple.count != b->v.tuple.count) return 0;
            for (int i = 0; i < a->v.tuple.count; i++)
                if (!_vets_key_eq(a->v.tuple.items[i], b->v.tuple.items[i])) return 0;
            return 1;
        case SW_VAL_PID: /* compare by numeric pid id (see sw_val_equal) */
            return (a->v.pid ? a->v.pid->pid : 0) == (b->v.pid ? b->v.pid->pid : 0);
        default: return a == b;
    }
}

/* Fully recursive free of a global-heap value tree that a long-lived owner (an
 * ETS entry, a timer closure) deep-copied via sw_val_deep_copy_global and is now
 * discarding. Mirrors deep_copy_rec's allocations (every node val_alloc'd;
 * strings/atoms and bytes own their buffers; tuple/list/map/fun element arrays
 * are malloc'd). Unlike sw_val_free it recurses into MAP keys/vals and FUN
 * captures, because such a value is a deep_copy_global tree — fully independent,
 * no interned or shared nodes (verified: every sw_val_* constructor allocates
 * fresh) — so the WHOLE graph must go. Caller must ensure no one else holds the
 * pointer (ETS copies values OUT to readers; a one-shot timer frees only after
 * its single apply returns). */
static void _sw_free_global_val(sw_val_t *v) {
    if (!v) return;
    switch (v->type) {
    case SW_VAL_STRING: case SW_VAL_ATOM:
        free(v->v.str); break;
    case SW_VAL_BYTES:
        free(v->v.bytes.data); break;
    case SW_VAL_TUPLE: case SW_VAL_LIST:
        for (int i = 0; i < v->v.tuple.count; i++) _sw_free_global_val(v->v.tuple.items[i]);
        free(v->v.tuple.items); break;
    case SW_VAL_MAP:
        for (int i = 0; i < v->v.map.count; i++) {
            _sw_free_global_val(v->v.map.keys[i]);
            _sw_free_global_val(v->v.map.vals[i]);
        }
        free(v->v.map.keys); free(v->v.map.vals); break;
    case SW_VAL_FUN:
        for (int i = 0; i < v->v.fun.ncaptures; i++) _sw_free_global_val(v->v.fun.captures[i]);
        free(v->v.fun.captures); break;
    default: break;
    }
    free(v);
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
        /* GC v1: ETS is a cross-process store outliving the inserter, so its
         * keys/values must live on the global heap (deep-copied), not in the
         * inserting process's arena which is freed on its exit. */
        if (_vets_key_eq(e->key, a[1])) { _sw_free_global_val(e->value); e->value = sw_val_deep_copy_global(a[2]); pthread_rwlock_unlock(&t->lock); return sw_val_atom("ok"); }  /* free old to bound replace churn */
        e = e->next;
    }
    _vets_entry_t *ne = (_vets_entry_t *)malloc(sizeof(_vets_entry_t));
    ne->key = sw_val_deep_copy_global(a[1]); ne->value = sw_val_deep_copy_global(a[2]); ne->next = t->buckets[bucket];
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
    while (e) {
        if (_vets_key_eq(e->key, a[1])) {
            /* Copy OUT into the caller's arena (BEAM ETS semantics) so the table
             * can own + free its stored copy without dangling this reader. Done
             * under the read lock — a writer (replace/delete/take) needs the write
             * lock, so the stored value can't be freed mid-copy. */
            sw_val_t *v = sw_val_deep_copy_local(e->value);
            pthread_rwlock_unlock(&t->lock);
            return v;
        }
        e = e->next;
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
            _vets_entry_t *dead = *pp; *pp = dead->next;
            _sw_free_global_val(dead->value); _sw_free_global_val(dead->key); free(dead);  /* free stored value+key, not just the entry */
            pthread_rwlock_unlock(&t->lock); return sw_val_atom("ok");
        }
        pp = &(*pp)->next;
    }
    pthread_rwlock_unlock(&t->lock);
    return sw_val_atom("ok");
}

static sw_val_t *_builtin_ets_update_counter(sw_val_t **a, int n) {
    if (n < 4 || a[0]->type != SW_VAL_INT || a[2]->type != SW_VAL_INT || a[3]->type != SW_VAL_INT)
        return sw_val_atom("error");
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_atom("error");
    _vets_table_t *t = &_vets_tables[id];
    uint32_t bucket = _vets_hash_val(a[1]);
    int64_t delta = a[2]->v.i;
    int64_t initial = a[3]->v.i;

    pthread_rwlock_wrlock(&t->lock);
    _vets_entry_t *e = t->buckets[bucket];
    while (e) {
        if (_vets_key_eq(e->key, a[1])) {
            if (e->value && e->value->type == SW_VAL_INT) {
                /* Allocate a NEW int for the entry — mutating the old
                 * one in place would corrupt any caller still holding
                 * the previous reference. */
                int64_t result = e->value->v.i + delta;  /* read int BEFORE freeing */
                _sw_free_global_val(e->value);
                e->value = sw_val_deep_copy_global(sw_val_int(result));  /* GC v1: ETS store on global heap; free old to bound churn */
                pthread_rwlock_unlock(&t->lock);
                return sw_val_int(result);
            }
            pthread_rwlock_unlock(&t->lock);
            return sw_val_atom("error");
        }
        e = e->next;
    }
    /* Not found — seed at `initial`, apply `delta`, store the result.
     * Return a FRESH int (not the entry's value pointer); otherwise a
     * later update on the same key mutates the caller's earlier
     * binding. Matches Erlang's update_counter/4 semantics. */
    int64_t seeded = initial + delta;
    sw_val_t *stored = sw_val_deep_copy_global(sw_val_int(seeded));  /* GC v1: ETS store on global heap */
    _vets_entry_t *ne = (_vets_entry_t *)malloc(sizeof(_vets_entry_t));
    ne->key = sw_val_deep_copy_global(a[1]); ne->value = stored; ne->next = t->buckets[bucket];
    t->buckets[bucket] = ne;
    pthread_rwlock_unlock(&t->lock);
    return sw_val_int(seeded);
}

static sw_val_t *_builtin_ets_cas(sw_val_t **a, int n) {
    if (n < 4 || a[0]->type != SW_VAL_INT) return sw_val_atom("false");
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_atom("false");
    _vets_table_t *t = &_vets_tables[id];
    uint32_t bucket = _vets_hash_val(a[1]);

    pthread_rwlock_wrlock(&t->lock);
    _vets_entry_t *e = t->buckets[bucket];
    while (e) {
        if (_vets_key_eq(e->key, a[1])) {
            if (_vets_key_eq(e->value, a[2])) {  /* reads old value before freeing */
                _sw_free_global_val(e->value);
                e->value = sw_val_deep_copy_global(a[3]);  /* GC v1: ETS store on global heap; free old to bound churn */
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

static sw_val_t *_builtin_ets_take(sw_val_t **a, int n) {
    if (n < 2 || a[0]->type != SW_VAL_INT) return sw_val_nil();
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_nil();
    _vets_table_t *t = &_vets_tables[id];
    uint32_t bucket = _vets_hash_val(a[1]);

    pthread_rwlock_wrlock(&t->lock);
    _vets_entry_t **pp = &t->buckets[bucket];
    while (*pp) {
        if (_vets_key_eq((*pp)->key, a[1])) {
            _vets_entry_t *dead = *pp;
            /* Copy OUT to the caller, then free the table's stored value + key +
             * entry (the entry is removed; bound the memory). */
            sw_val_t *out = dead->value ? sw_val_deep_copy_local(dead->value) : sw_val_nil();
            *pp = dead->next;
            _sw_free_global_val(dead->value); _sw_free_global_val(dead->key); free(dead);
            pthread_rwlock_unlock(&t->lock);
            return out;
        }
        pp = &(*pp)->next;
    }
    pthread_rwlock_unlock(&t->lock);
    return sw_val_nil();
}

static sw_val_t *_builtin_ets_update(sw_val_t **a, int n) {
    if (n < 3 || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    int id = (int)a[0]->v.i;
    if (id < 0 || id >= _VETS_MAX_TABLES || !_vets_tables[id].active) return sw_val_atom("error");

    /* Third argument must be a compiled lambda (cfunc pointer).
     * Interpreter-path lambdas (body AST) are handled by the ETS dispatch
     * block inside interp_extra_builtin() in swarmrt_lang.c, which has
     * access to eval().  Here we only ever see AOT-compiled closures. */
    sw_val_t *fn = a[2];
    if (!fn || fn->type != SW_VAL_FUN || !fn->v.fun.cfunc) return sw_val_atom("error");

    _vets_table_t *t = &_vets_tables[id];
    uint32_t bucket = _vets_hash_val(a[1]);

    /* Snapshot the current value under a read lock. */
    pthread_rwlock_rdlock(&t->lock);
    sw_val_t *old_val = NULL;
    _vets_entry_t *e = t->buckets[bucket];
    while (e) {
        /* Snapshot a COPY: the lambda runs OUTSIDE the lock (below), and another
         * process could replace/delete this key meanwhile and free the stored
         * value — passing the table's pointer to the lambda would be a UAF. */
        if (_vets_key_eq(e->key, a[1])) { old_val = sw_val_deep_copy_local(e->value); break; }
        e = e->next;
    }
    pthread_rwlock_unlock(&t->lock);

    if (!old_val) return sw_val_atom("error");  /* key not found */

    /* Call the compiled lambda outside any lock. */
    sw_val_t *new_val = sw_val_apply(fn, &old_val, 1);
    if (!new_val) return sw_val_atom("error");

    /* Write the result back under write lock; re-scan because lock was
     * dropped during the function call. */
    pthread_rwlock_wrlock(&t->lock);
    e = t->buckets[bucket];
    while (e) {
        if (_vets_key_eq(e->key, a[1])) {
            _sw_free_global_val(e->value);
            e->value = sw_val_deep_copy_global(new_val);  /* GC v1: ETS store on global heap; free old to bound churn */
            pthread_rwlock_unlock(&t->lock);
            return new_val;
        }
        e = e->next;
    }
    pthread_rwlock_unlock(&t->lock);
    return sw_val_atom("error");  /* key disappeared between read and write */
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

/* os_args() -> list of command-line argument strings (argv[0] first).
 * sw_prog_argc/argv are filled in by the generated program's main();
 * see swarmrt_codegen.c. Returns [] when unset. */
static sw_val_t *_builtin_os_args(sw_val_t **a, int n) {
    (void)a; (void)n;
    if (sw_prog_argc <= 0 || !sw_prog_argv) return sw_val_list(NULL, 0);
    sw_val_t **items = malloc(sizeof(sw_val_t *) * sw_prog_argc);
    for (int i = 0; i < sw_prog_argc; i++)
        items[i] = sw_val_string(sw_prog_argv[i] ? sw_prog_argv[i] : "");
    sw_val_t *r = sw_val_list(items, sw_prog_argc);
    free(items);
    return r;
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
        case SW_VAL_FUN: return sw_val_string("fun");
        case SW_VAL_MAP: return sw_val_string("map");
        case SW_VAL_BYTES: return sw_val_string("bytes");
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

/* === Math (libm-backed; -lm is linked) ===================================
 * Each accepts an int OR float arg (coerced to double) and returns a float,
 * except the rounding family (floor/ceil/round) which returns an int — the
 * common case is "give me a whole number back". lib/Math.sw wraps these as
 * Math.sqrt(x) etc. and adds pure-sw min/max/clamp/pi. */
static double _sw_to_double(sw_val_t *v) {
    if (!v) return 0.0;
    if (v->type == SW_VAL_INT)   return (double)v->v.i;
    if (v->type == SW_VAL_FLOAT) return v->v.f;
    return 0.0;
}

/* to_float(x) → float; to_int(x) → int. Parse a string/atom, or coerce a
 * number; nil on a non-numeric string. Shared impl with the interpreter
 * (sw_coerce_*), so the backends can't drift. */
static sw_val_t *_builtin_to_float(sw_val_t **a, int n) {
    if (n < 1) return sw_val_nil();
    return sw_coerce_float(a[0]);
}
static sw_val_t *_builtin_to_int(sw_val_t **a, int n) {
    if (n < 1) return sw_val_nil();
    return sw_coerce_int(a[0]);
}
/* uuid() → random RFC-4122 v4 string; now_iso() → current UTC ISO-8601. */
static sw_val_t *_builtin_uuid(sw_val_t **a, int n) { (void)a; (void)n; return sw_make_uuid(); }
static sw_val_t *_builtin_now_iso(sw_val_t **a, int n) { (void)a; (void)n; return sw_now_iso(); }

static sw_val_t *_builtin_math_sqrt(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_float(0.0);
    return sw_val_float(sqrt(_sw_to_double(a[0])));
}
static sw_val_t *_builtin_math_sin(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_float(0.0);
    return sw_val_float(sin(_sw_to_double(a[0])));
}
static sw_val_t *_builtin_math_cos(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_float(0.0);
    return sw_val_float(cos(_sw_to_double(a[0])));
}
static sw_val_t *_builtin_math_pow(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || !a[1]) return sw_val_float(0.0);
    return sw_val_float(pow(_sw_to_double(a[0]), _sw_to_double(a[1])));
}
static sw_val_t *_builtin_math_exp(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_float(0.0);
    return sw_val_float(exp(_sw_to_double(a[0])));
}
static sw_val_t *_builtin_math_log(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_float(0.0);
    return sw_val_float(log(_sw_to_double(a[0])));
}
/* floor/ceil/round → int (the value still fits a double exactly for the
 * magnitudes sw programs use). */
static sw_val_t *_builtin_math_floor(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_int(0);
    return sw_val_int((int64_t)floor(_sw_to_double(a[0])));
}
static sw_val_t *_builtin_math_ceil(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_int(0);
    return sw_val_int((int64_t)ceil(_sw_to_double(a[0])));
}
static sw_val_t *_builtin_math_round(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_int(0);
    return sw_val_int((int64_t)llround(_sw_to_double(a[0])));
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

/* ord(s) → int. The byte value (0..255) of the FIRST byte of s. Empty
 * string → -1. The char→int primitive tokenizers/hashers reach for. */
static sw_val_t *_builtin_ord(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_int(-1);
    const unsigned char *s = (const unsigned char *)a[0]->v.str;
    return sw_val_int(s[0] ? (int64_t)s[0] : -1);
}

/* codepoint_at(s, i) → int. The byte value (0..255) at byte index i of s,
 * or -1 if i is out of range. Byte-level by design — exactly what hashers
 * and byte-pair tokenizers need; no UTF-8 decoding. */
static sw_val_t *_builtin_codepoint_at(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_INT) return sw_val_int(-1);
    const unsigned char *s = (const unsigned char *)a[0]->v.str;
    int64_t i = a[1]->v.i;
    size_t len = strlen((const char *)s);
    if (i < 0 || (size_t)i >= len) return sw_val_int(-1);
    return sw_val_int((int64_t)s[i]);
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

/* file_read_bytes(path) → bytes | nil. Binary-safe sibling of file_read:
 * returns a length-carrying SW_VAL_BYTES (survives embedded NULs) and has
 * no ~1MB cap — the whole file is read. The flagship use-case is PCM/WAV/
 * protocol frames, which file_read truncates at the first 0x00. */
static sw_val_t *_builtin_file_read_bytes(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    FILE *fp = fopen(a[0]->v.str, "rb");
    if (!fp) return sw_val_nil();
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return sw_val_nil(); }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    sw_val_t *r = sw_val_bytes(buf, got);
    free(buf);
    return r;
}

/* file_write_bytes(path, bytes) → 'ok' | 'error'. Binary-safe sibling of
 * file_write: writes the exact byte count of a SW_VAL_BYTES (NULs and all)
 * with no truncation. */
static sw_val_t *_builtin_file_write_bytes(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_BYTES)
        return sw_val_atom("error");
    FILE *fp = fopen(a[0]->v.str, "wb");
    if (!fp) return sw_val_atom("error");
    size_t len = a[1]->v.bytes.len;
    size_t wr = len ? fwrite(a[1]->v.bytes.data, 1, len, fp) : 0;
    int ok = (fclose(fp) == 0) && (wr == len);
    return sw_val_atom(ok ? "ok" : "error");
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
    /* Async-redraw cooperation: while a raw-mode read_line is showing
     * a line, `active` is set and the cur_* fields point at the live
     * editor state. print_above() uses them to wipe the input line,
     * print above it, and redraw it — so output from other processes
     * never clobbers what the user is mid-typing. */
    volatile int active;
    const char *cur_prompt;
    char **cur_buf;
    size_t *cur_len;
    size_t *cur_cursor;
    /* Multi-line redraw bookkeeping (F10): the physical terminal-line
     * geometry the MOST RECENT redraw produced, so the NEXT redraw (from
     * any of the concurrent writers — keystroke, print_above, shell
     * progress, http-stream stall) can wipe ALL prior physical lines
     * instead of only the single \r\x1b[K line. `last_rows` is the total
     * physical rows the render occupied (>=1 once anything was drawn);
     * `caret_row` is the caret's 0-based physical row from the top of the
     * render. Both are 0 until the first redraw. */
    int last_rows;
    int caret_row;
} _sw_rl_state_t;

static _sw_rl_state_t _sw_rl = {0};

/* Serialises terminal writes between the line editor's own redraws
 * and print_above() calls arriving from other processes. */
static pthread_mutex_t _sw_term_lock = PTHREAD_MUTEX_INITIALIZER;

/* Forward decl — defined alongside read_line near the bottom of this
 * file. Needed up here so shell()'s progress wipe can redraw the
 * input box back below its output without eating what the user typed. */
static void _sw_rl_redraw_unlocked(const char *prompt, const char *buf, size_t len, size_t cursor);
/* Erase ALL physical rows of the active input render (F10). External writers
 * (shell progress, print_above, http-stream stall) call this before printing
 * their own "above" output, then re-call _sw_rl_redraw_unlocked. Defined near
 * read_line; forward-declared here for the shell()-progress wipe above it. */
static void _sw_rl_wipe_unlocked(void);

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

/* Like _sw_popen_pid, but execs argv DIRECTLY — no /bin/sh, no command
 * string. This is the injection-safe path: the URL, headers and any other
 * caller-supplied strings arrive as literal argv elements, so shell
 * metacharacters ('`$();|&<>' and embedded quotes) in untrusted input can
 * never be interpreted. argv must be NULL-terminated; argv[0] is the
 * program (resolved via PATH by execvp). Child stderr is sent to /dev/null
 * Child stderr goes to `stderr_path` if non-NULL (callers that want to
 * surface curl transport errors), else to /dev/null (matching the old
 * `2>/dev/null`). Returns the same {fp, pid} so _sw_pkill_close /
 * _sw_popen_pid_close / the select loop all work unchanged. */
static _sw_popen_pid_t _sw_popen_argv(char *const argv[], const char *stderr_path) {
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
        int errfd = stderr_path
            ? open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
            : open("/dev/null", O_WRONLY);
        if (errfd >= 0) { dup2(errfd, STDERR_FILENO); close(errfd); }
        /* Own process group so killpg() on interrupt takes down the
         * child without signaling us. */
        setpgid(0, 0);
        execvp(argv[0], argv);
        _exit(127);
    }
    /* parent */
    close(pipefd[1]);
    r.fp = fdopen(pipefd[0], "r");
    r.pid = pid;
#else
    (void)argv;
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
 * Pending-input ring buffer — type-ahead captured mid-turn
 * ============================================================
 *
 * Keys the user presses while an LLM stream or a tool is running used to be
 * silently discarded at the stdin watch sites. Instead we deposit the raw
 * bytes here (a fixed 8 KB, byte-oriented, drop-oldest ring, mutex-guarded so
 * a worker/scheduler thread and the main reader never race) and the next
 * read_line() drains it as a seed for the edit buffer (CC-style queued input).
 *
 * Builtins:
 *   stdin_pending_push(str)  → 'true' | 'false'  (append bytes; false on bad arg)
 *   stdin_take_pending()     → string | nil      (atomic drain of the whole ring)
 */
#define _SW_PENDING_CAP 8192
static unsigned char _sw_pending_buf[_SW_PENDING_CAP];
static size_t _sw_pending_len = 0;
static pthread_mutex_t _sw_pending_lock = PTHREAD_MUTEX_INITIALIZER;

/* Append n bytes, dropping the OLDEST bytes if the ring would overflow. */
static void _sw_pending_push_bytes(const unsigned char *data, size_t n) {
    if (!data || n == 0) return;
    pthread_mutex_lock(&_sw_pending_lock);
    if (n >= _SW_PENDING_CAP) {
        /* The new data alone exceeds the ring: keep only its newest tail. */
        memcpy(_sw_pending_buf, data + (n - _SW_PENDING_CAP), _SW_PENDING_CAP);
        _sw_pending_len = _SW_PENDING_CAP;
        pthread_mutex_unlock(&_sw_pending_lock);
        return;
    }
    if (_sw_pending_len + n > _SW_PENDING_CAP) {
        size_t drop = (_sw_pending_len + n) - _SW_PENDING_CAP;
        memmove(_sw_pending_buf, _sw_pending_buf + drop, _sw_pending_len - drop);
        _sw_pending_len -= drop;
    }
    memcpy(_sw_pending_buf + _sw_pending_len, data, n);
    _sw_pending_len += n;
    pthread_mutex_unlock(&_sw_pending_lock);
}

/* Atomically drain the ring into a freshly malloc'd NUL-terminated buffer.
 * Returns NULL (and *out_len = 0) when the ring is empty. Caller frees. */
static char *_sw_pending_take(size_t *out_len) {
    if (out_len) *out_len = 0;
    pthread_mutex_lock(&_sw_pending_lock);
    if (_sw_pending_len == 0) { pthread_mutex_unlock(&_sw_pending_lock); return NULL; }
    size_t n = _sw_pending_len;
    char *r = (char *)malloc(n + 1);
    if (!r) { pthread_mutex_unlock(&_sw_pending_lock); return NULL; }
    memcpy(r, _sw_pending_buf, n);
    r[n] = '\0';
    _sw_pending_len = 0;
    pthread_mutex_unlock(&_sw_pending_lock);
    if (out_len) *out_len = n;
    return r;
}

/* A bracketed-paste OPEN (ESC[200~) has just been consumed off `fd` mid-stream.
 * Drain the paste BODY up to and including the ESC[201~ close, pushing only the
 * body (never the framing) into the pending-input ring. A rolling match of the
 * 6-byte close sequence lets us keep body bytes that merely resemble the close
 * prefix; a short idle timeout bounds the loop so a truncated/never-closed
 * paste can never hang the interrupt watcher. */
#ifndef _WIN32
static void _sw_stdin_drain_paste_body(int fd) {
    static const unsigned char CLOSE[6] = {0x1b, '[', '2', '0', '1', '~'};
    unsigned char body[4096];
    size_t blen = 0;
    int mp = 0;                 /* bytes of CLOSE currently matched (held back) */
    for (int guard = 0; guard < 5000000; guard++) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = {0, 500000};   /* 500 ms idle → assume paste ended */
        if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) break;
        unsigned char b;
        if (read(fd, &b, 1) != 1) break;
        if (b == CLOSE[mp]) {
            if (++mp == 6) break;          /* full ESC[201~ → done (framing dropped) */
            continue;                      /* hold: might complete the close */
        }
        /* Mismatch: the mp held bytes were genuine body after all. */
        for (int k = 0; k < mp; k++) {
            body[blen++] = CLOSE[k];
            if (blen == sizeof(body)) { _sw_pending_push_bytes(body, blen); blen = 0; }
        }
        mp = 0;
        if (b == CLOSE[0]) { mp = 1; continue; }  /* b may start a fresh close */
        body[blen++] = b;
        if (blen == sizeof(body)) { _sw_pending_push_bytes(body, blen); blen = 0; }
    }
    if (blen > 0) _sw_pending_push_bytes(body, blen);
}
#endif

/* Decide whether a byte just read from stdin is a GENUINE interrupt request
 * (a bare Esc, or Ctrl-C) versus the FIRST byte of an escape sequence — arrow
 * keys, F-keys, Home/End, alt-combos and bracketed-paste ALL begin with 0x1b.
 * For an escape sequence we drain the rest and return 0, so a stray arrow key
 * or a paste can never spuriously abort a stream or kill a running tool. This
 * is the single source of truth for every stdin interrupt-watch site (LLM
 * stream, shell_managed, read_key). `first` is the byte already read off `fd`
 * (a raw-mode tty). A bare Esc is confirmed by a 50ms peek finding no follow-up. */
static int _sw_stdin_is_interrupt(int fd, unsigned char first) {
#ifdef _WIN32
    (void)fd;
    return (first == 0x03 || first == 0x1b) ? 1 : 0;
#else
    if (first == 0x03) return 1;          /* Ctrl-C */
    if (first != 0x1b) return 0;          /* not Esc → not an interrupt */
    fd_set pf;
    FD_ZERO(&pf);
    FD_SET(fd, &pf);
    struct timeval ptv;
    ptv.tv_sec = 0;
    ptv.tv_usec = 50000;  /* 50ms — bare Esc has no follow-up byte */
    int pr = select(fd + 1, &pf, NULL, NULL, &ptv);
    if (pr == 0) return 1;   /* clean timeout: no follow-up byte → genuine bare Esc */
    if (pr < 0)  return 0;   /* select error/EINTR: do NOT guess "interrupt" — a
                              * spurious abort is worse than a missed Esc (user re-presses) */
    /* pr > 0: a follow-up byte is pending → escape sequence; drain it below. */
    unsigned char intro;
    if (read(fd, &intro, 1) != 1) return 0;
    if (intro == '[' || intro == 'O') {
        /* CSI/SS3: drain params/intermediates up to a final byte (0x40-0x7e),
         * peeking 0ms between bytes so we never block. Capture the params so a
         * bracketed-paste OPEN (ESC[200~) is recognised and its BODY diverted
         * into the pending-input ring instead of being discarded byte-by-byte. */
        int guard = 0;
        unsigned char seq = 0;
        char params[8];
        int plen = 0;
        int got_final = 0;
        while (guard < 24) {
            fd_set sf;
            FD_ZERO(&sf);
            FD_SET(fd, &sf);
            struct timeval z;
            z.tv_sec = 0;
            z.tv_usec = 0;
            if (select(fd + 1, &sf, NULL, NULL, &z) <= 0) break;
            if (read(fd, &seq, 1) != 1) break;
            guard++;
            if (seq >= 0x40 && seq <= 0x7e) { got_final = 1; break; }
            if (plen < (int)sizeof(params) - 1) params[plen++] = (char)seq;
        }
        params[plen] = '\0';
        if (intro == '[' && got_final && seq == '~' && strcmp(params, "200") == 0)
            _sw_stdin_drain_paste_body(fd);   /* ESC[200~ … ESC[201~ → ring */
    }
    /* else: Esc + one byte (Alt-combo) — `intro` already consumed. */
    return 0;  /* escape sequence → NOT an interrupt */
#endif
}

/* After an explicit user interrupt, clear queued interrupt keys before the
 * caller returns to the agent loop. Without this, a double-press/held Esc can
 * remain in the tty input queue and the next http_post_stream immediately
 * aborts with "[Request interrupted by user]" even though the user already
 * released the key. Bare Esc bytes are discarded; any OTHER queued bytes are
 * legitimate type-ahead and are diverted into the pending-input ring so the
 * next read_line can seed them rather than losing them to the flush. */
static void _sw_stdin_flush_pending_interrupts(int fd) {
#ifndef _WIN32
    if (fd < 0) return;
    /* Drain queued input after an explicit interrupt. Bare Esc bytes are the
     * held/repeated interrupt key and stay discarded (the original bug: a
     * lingering Esc made the next stream abort instantly); every OTHER byte is
     * legitimate type-ahead the user entered and is preserved in the
     * pending-input ring so the next read_line seeds it. We read (rather than
     * tcflush) so we can inspect and selectively keep bytes. read() only fires
     * after select() reports the fd readable, so it never blocks. */
    unsigned char chunk[256];
    for (int guard = 0; guard < 64; guard++) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {0, 0};
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) break;
        ssize_t rn = read(fd, chunk, sizeof(chunk));
        if (rn <= 0) break;
        unsigned char keep[256];
        int kn = 0;
        for (ssize_t i = 0; i < rn; i++) {
            if (chunk[i] == 0x1b) {
                /* Held Esc → discard. If the Esc INTRODUCES a CSI/SS3 escape
                 * sequence (arrow/F-key queued behind the held key), swallow
                 * the whole sequence — its remainder ("[A") is key framing,
                 * not type-ahead, and would otherwise be seeded into the next
                 * read_line prompt as literal text. */
                if (i + 1 < rn && (chunk[i + 1] == '[' || chunk[i + 1] == 'O')) {
                    i++;                                 /* consume the intro */
                    while (i + 1 < rn) {
                        unsigned char fin = chunk[++i];  /* params → final    */
                        if (fin >= 0x40 && fin <= 0x7e) break;
                    }
                }
                continue;
            }
            keep[kn++] = chunk[i];
        }
        if (kn > 0) _sw_pending_push_bytes(keep, (size_t)kn);
    }
#else
    (void)fd;
#endif
}

/* stdin_pending_push(str) → 'true' | 'false'. Append the string's bytes to the
 * pending-input ring (drop-oldest on overflow). 'false' on a missing/non-string
 * or empty argument. */
static sw_val_t *_builtin_stdin_pending_push(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_atom("false");
    const char *s = a[0]->v.str;
    size_t len = strlen(s);
    if (len == 0) return sw_val_atom("false");
    _sw_pending_push_bytes((const unsigned char *)s, len);
    return sw_val_atom("true");
}

/* stdin_take_pending() → string | nil. Atomically drain the whole ring. */
static sw_val_t *_builtin_stdin_take_pending(sw_val_t **a, int n) {
    (void)a; (void)n;
    size_t len = 0;
    char *drained = _sw_pending_take(&len);
    if (!drained) return sw_val_nil();
    sw_val_t *r = sw_val_string(drained);
    free(drained);
    return r;
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
/* Guards slot allocation in subprocess_spawn — swarm-code's MCP boot
 * spawns servers from concurrent worker processes, so two spawn calls
 * can run on different scheduler threads at once. Without this lock
 * they race to claim the same free slot and clobber each other's
 * pipes (one server then receives no input and hangs). */
static pthread_mutex_t _sw_subproc_lock = PTHREAD_MUTEX_INITIALIZER;

static sw_val_t *_builtin_subprocess_spawn(sw_val_t **a, int n) {
#ifdef _WIN32
    (void)a; (void)n;
    return sw_val_int(-1);
#else
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_int(-1);

    /* Hold the lock across slot scan + claim + fork. fork() under the
     * lock is safe here: the child execs immediately and never touches
     * the mutex, and the parent always unlocks. */
    pthread_mutex_lock(&_sw_subproc_lock);
    int slot = -1;
    for (int i = 0; i < _SW_SUBPROC_MAX; i++) if (!_sw_subprocs[i].active) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&_sw_subproc_lock); return sw_val_int(-1); }

    int p_in[2], p_out[2];
    if (pipe(p_in) != 0) {
        pthread_mutex_unlock(&_sw_subproc_lock);
        return sw_val_int(-1);
    }
    if (pipe(p_out) != 0) {
        close(p_in[0]); close(p_in[1]);
        pthread_mutex_unlock(&_sw_subproc_lock);
        return sw_val_int(-1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(p_in[0]); close(p_in[1]); close(p_out[0]); close(p_out[1]);
        pthread_mutex_unlock(&_sw_subproc_lock);
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
    pthread_mutex_unlock(&_sw_subproc_lock);
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

    /* Body via temp file — keeps the JSON out of argv entirely (and out
     * of any shell). curl reads it with -d @file. */
    char tmpf[256];
    snprintf(tmpf, sizeof(tmpf),
        "%s/sw_http_%d_%u.json", sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    FILE *tf = fopen(tmpf, "w");
    if (tf) { fputs(body, tf); fclose(tf); }
    char body_arg[300];
    snprintf(body_arg, sizeof(body_arg), "@%s", tmpf);

    /* Build a curl ARGV — executed via _sw_popen_argv (execvp, no shell).
     * The URL and header values are caller-supplied and may contain shell
     * metacharacters; as literal argv elements they can never be
     * interpreted. (The old path single-quoted them into a /bin/sh string
     * with no escaping — an embedded quote broke out into the shell.) */
    sw_val_t *headers = a[1];
    int nhdr = (headers && headers->type == SW_VAL_LIST) ? headers->v.tuple.count : 0;
    /* 8 fixed + 2 per header + (-d, body_arg, url) + NULL */
    char **argv = (char **)malloc(sizeof(char *) * (8 + 2 * nhdr + 3 + 1));
    char **hdr_strs = (char **)malloc(sizeof(char *) * (nhdr > 0 ? nhdr : 1));
    int argc = 0, nhdr_alloc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-sS";
    argv[argc++] = "-X";
    argv[argc++] = "POST";
    argv[argc++] = "--connect-timeout";
    argv[argc++] = "30";
    argv[argc++] = "--max-time";
    argv[argc++] = "300";
    if (headers && headers->type == SW_VAL_LIST) {
        for (int i = 0; i < headers->v.tuple.count; i++) {
            sw_val_t *h = headers->v.tuple.items[i];
            if (h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->v.str && h->v.tuple.items[1]->v.str) {
                const char *hk = h->v.tuple.items[0]->v.str;
                const char *hv = h->v.tuple.items[1]->v.str;
                size_t hl = strlen(hk) + strlen(hv) + 3;
                char *hs = (char *)malloc(hl);
                snprintf(hs, hl, "%s: %s", hk, hv);
                hdr_strs[nhdr_alloc++] = hs;
                argv[argc++] = "-H";
                argv[argc++] = hs;
            }
        }
    }
    argv[argc++] = "-d";
    argv[argc++] = body_arg;
    argv[argc++] = (char *)url;  /* execvp won't modify it */
    argv[argc] = NULL;

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

        _sw_popen_pid_t ch = _sw_popen_argv(argv, NULL);
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
                if (nb == 1 && _sw_stdin_is_interrupt(stdin_fd, ib)) {
                    interrupted = 1;
                    _sw_stdin_flush_pending_interrupts(stdin_fd);
                    fputs("\n  \x1b[38;5;208m⏸ interrupted by user\x1b[0m\n", stdout);
                    fflush(stdout);
                    _sw_pkill_close(ch);
                    ch.fp = NULL; ch.pid = -1;
                    done = 1; break;
                }
                /* Non-interrupt keystrokes are legitimate type-ahead the user
                 * entered mid-stream: preserve them in the pending-input ring
                 * so the next read_line seeds them. Arrow/F-key framing and any
                 * bracketed-paste body were already drained (the paste body
                 * pushed to the ring) by _sw_stdin_is_interrupt; a lone ESC or
                 * other control byte lands here but is dropped at seed time. */
                else if (nb == 1) {
                    _sw_pending_push_bytes(&ib, 1);
                }
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
    for (int i = 0; i < nhdr_alloc; i++) free(hdr_strs[i]);
    free(hdr_strs);
    free(argv);

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
 *   - Wrapping is word-aware: visible bytes accumulate in a pending
 *     word buffer and commit only at a whitespace boundary, so a wrap
 *     never splits a word. A word wider than the line is hard-broken.
 */

typedef struct {
    int term_w;
    int right_margin;
    int col;
    int in_ansi;
    int line_started;
    /* Pending-word buffer — visible bytes (and any ANSI sequences
     * embedded in them) collect here and flush at the next space /
     * tab / newline, so soft-wrap breaks between words, not within. */
    char word[256];
    int  word_len;
    int  word_cols;     /* display columns: ANSI + UTF-8 conts excluded */
    int  word_in_ansi;  /* mid-ANSI-escape while filling word[] */
    int  rows;          /* physical newlines emitted (incl. soft-wraps) */
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

#ifndef _WIN32
static int _sw_term_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return (int)ws.ws_row;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return (int)ws.ws_row;
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd >= 0) {
        int row = 0;
        if (ioctl(tty_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
            row = (int)ws.ws_row;
        close(tty_fd);
        if (row > 0) return row;
    }
    return 24;
}
#else
static int _sw_term_rows(void) { return 24; }
#endif

/* sw builtin: term_rows() → int  (number of rows in the current tty) */
static sw_val_t *_builtin_term_rows(sw_val_t **a, int n) {
    (void)a; (void)n;
    return sw_val_int((int64_t)_sw_term_rows());
}

/* sw builtin: stdout_is_tty() → 'true' | 'false'. Distinguishes a real
 * terminal from a pipe/redirect so \r-rewrite UI (stream ticker, inline
 * clears) can fall back to plain sequential output when captured —
 * term_cols() can't be used for this: it falls back to /dev/tty. */
static sw_val_t *_builtin_stdout_is_tty(sw_val_t **a, int n) {
    (void)a; (void)n;
    return sw_val_atom(isatty(fileno(stdout)) ? "true" : "false");
}

/* Physical terminal rows the most recent (non-subagent) http_post_stream
 * emitted for the assistant CONTENT region — set at end-of-stream and read
 * by the markdown repaint so it clears exactly the streamed prose. Reasoning
 * is written raw (not through the column-aware emitter) so it is not counted. */
static int _sw_last_content_rows = 0;

/* Forward decls — these JSON helpers are defined later in the file, but the
 * streaming SSE escape loops (above their definition) need them to decode
 * \uXXXX escapes inline. */
static int _json_parse_hex4(const char **pp);
static int _utf8_encode(unsigned int cp, char *buf);

/* sw builtin: stream_content_rows() → int  (physical rows the last streamed
 * assistant content occupied; 0 when none was streamed). */
static sw_val_t *_builtin_stream_content_rows(sw_val_t **a, int n) {
    (void)a; (void)n;
    return sw_val_int((int64_t)_sw_last_content_rows);
}

static void _sw_stream_init(_sw_stream_state_t *s) {
    int w = _sw_term_cols();
    s->term_w = w;
    s->right_margin = (w > 10) ? (w - 2) : w;
    s->col = 0;
    s->in_ansi = 0;
    s->line_started = 0;
    s->word_len = 0;
    s->word_cols = 0;
    s->word_in_ansi = 0;
    s->rows = 0;
}

static void _sw_stream_reset_line(_sw_stream_state_t *s) {
    /* Called after \r\e[K clears the spinner line: cursor is at col 0
     * and we need to re-emit the 2-space indent before the next byte.
     * The word buffer is intentionally preserved — its bytes were
     * never emitted, so a word mid-built across a spinner repaint just
     * continues and flushes cleanly afterward. */
    s->col = 0;
    s->line_started = 0;
    s->in_ansi = 0;
}

/* Lazy leading indent: emit 2 spaces at the start of a line, deferred
 * until a visible byte is actually about to land. */
static void _sw_stream_indent(_sw_stream_state_t *s) {
    if (!s->line_started) {
        fputs("  ", stdout);
        s->col = 2;
        s->line_started = 1;
    }
}

/* Commit the pending word to stdout. Wraps to a fresh indented line
 * first if the word would overrun the right margin AND the current
 * line already carries real content (col past the indent). */
static void _sw_stream_flush_word(_sw_stream_state_t *s) {
    if (s->word_len == 0) return;
    _sw_stream_indent(s);
    if (s->col > 2 && s->col + s->word_cols > s->right_margin) {
        fputs("\n  ", stdout);
        s->rows++;
        s->col = 2;
    }
    fwrite(s->word, 1, (size_t)s->word_len, stdout);
    s->col += s->word_cols;
    s->word_len = 0;
    s->word_cols = 0;
    s->word_in_ansi = 0;
}

static void _sw_stream_emit(_sw_stream_state_t *s, char c) {
    if (c == '\n') {
        _sw_stream_flush_word(s);
        fputc('\n', stdout);
        s->rows++;
        s->col = 0;
        s->line_started = 0;
        return;
    }
    if (c == '\r') {
        _sw_stream_flush_word(s);
        fputc('\r', stdout);
        s->col = 0;
        return;
    }
    if (c == ' ' || c == '\t') {
        /* Whitespace ends a word. Commit it, then place the space —
         * unless we're already at the margin, where the space would
         * just be an invisible trailing char, so wrap instead. */
        _sw_stream_flush_word(s);
        if (s->col > 2 && s->col >= s->right_margin) {
            fputs("\n  ", stdout);
            s->rows++;
            s->col = 2;
        } else {
            _sw_stream_indent(s);
            fputc(c, stdout);
            s->col++;
        }
        return;
    }
    /* Word byte — a visible char or part of an embedded ANSI escape.
     * Buffer it; ANSI bytes and UTF-8 continuation bytes add no cols. */
    int is_cont = ((unsigned char)c & 0xC0) == 0x80;
    if (s->word_in_ansi) {
        if (s->word_len < (int)sizeof(s->word)) s->word[s->word_len++] = c;
        if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) s->word_in_ansi = 0;
        return;
    }
    if (c == '\x1b') {
        s->word_in_ansi = 1;
        if (s->word_len < (int)sizeof(s->word)) s->word[s->word_len++] = c;
        return;
    }
    if (s->word_len < (int)sizeof(s->word)) s->word[s->word_len++] = c;
    if (!is_cont) s->word_cols++;
    /* A word wider than the line can't be wrapped whole — hard-break
     * it by committing the chunk we have. Also flush before the buffer
     * fills (only when not mid-escape, so a sequence stays intact). */
    if (s->word_cols >= s->right_margin - 2 ||
        (s->word_len >= (int)sizeof(s->word) - 4 && !s->word_in_ansi)) {
        _sw_stream_flush_word(s);
    }
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

    /* Live throughput readout, derived from the char-based token estimate
     * already passed in. Computed only once the sample is stable so early
     * ticks don't flash an absurd rate. Approximate (a heartbeat/feel
     * signal), not server telemetry — same basis as the token counter. */
    char rate_s[24];
    rate_s[0] = '\0';
    if (tokens >= 25 && elapsed_ms >= 300) {
        int tps = (int)(((uint64_t)tokens * 1000ULL) / elapsed_ms);
        if (tps > 0) snprintf(rate_s, sizeof(rate_s), " · %d tok/s", tps);
    }

    /* Only show token counter once we've accumulated something worth showing. */
    if (tokens >= 25) {
        printf("\r\x1b[K %s%s%s %s%s… (%s · ↑ %d tokens%s · esc to interrupt)%s",
               c_brand, frames[frame], c_reset,
               c_dim, verb, elapsed_s, tokens, rate_s, c_reset);
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
    sw_send_value(target, SW_TAG_NONE, msg);   /* GC v1: copy off sender arena */
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

/* ============================================================
 * Native tool-call streaming — SSE delta.tool_calls reassembly
 * ============================================================
 *
 * OpenAI-compatible servers stream tool calls fragmented across many
 * `data:` frames. The first frame for a given call carries `index`,
 * `id`, and `function.name`; every later frame appends a slice of
 * `function.arguments`. This is TCP segment reassembly — `index` is
 * the sequence key, the argument fragments are payload appended in
 * arrival order.
 *
 * The argument fragments need no un-escaping: each SSE frame's
 * `arguments` value is independently JSON-escaped, and JSON escaping
 * is a context-free per-character map, so esc(a)++esc(b) == esc(a++b).
 * We therefore concatenate the RAW (escaped) bytes and emit them
 * straight into the synthetic response's `arguments` string. */
#define SW_MAX_TOOL_CALLS 64

/* http_post_stream scratch-buffer sizes. These buffers are heap-
 * allocated, not stack: the builtin runs deep in the agent call chain
 * and the swarmrt per-process stack is only 64 KB — 16K+8K+8K+4K of
 * stack arrays here overflowed it (SIGBUS in the prologue). */
#define SW_HPS_LINE_CAP 16384
#define SW_HPS_READ_CAP 4096
#define SW_HPS_TOK_CAP  8192

typedef struct {
    int    used;
    char   id[160];
    char   name[160];
    char  *args;            /* growable, raw JSON-escaped bytes */
    size_t args_len, args_cap;
} _sw_toolcall_t;

/* Return ptr to the bracket/brace that matches the open one at `p`,
 * honouring JSON string boundaries + escapes. NULL if unbalanced. */
static const char *_sw_match_bracket(const char *p) {
    int depth = 0, instr = 0;
    for (; *p; p++) {
        if (instr) {
            if (*p == '\\') { if (*(p + 1)) p++; continue; }
            if (*p == '"') instr = 0;
            continue;
        }
        if (*p == '"') { instr = 1; continue; }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { depth--; if (depth == 0) return p; }
    }
    return NULL;
}

/* `p` points just past a JSON string's opening quote; return ptr to
 * its closing (unescaped) quote, or NULL. */
static const char *_sw_json_str_end(const char *p) {
    for (; *p; p++) {
        if (*p == '\\') { if (*(p + 1)) p++; continue; }
        if (*p == '"') return p;
    }
    return NULL;
}

/* Parse the `tool_calls` array out of one SSE `data:` JSON line and
 * fold each fragment into the per-index accumulator set. */
static void _sw_parse_tc_deltas(const char *json, _sw_toolcall_t *tcs, int *tc_count) {
    const char *arr = strstr(json, "\"tool_calls\":");
    if (!arr) return;
    arr += 13;
    while (*arr == ' ') arr++;
    if (*arr != '[') return;            /* e.g. "tool_calls":null */
    const char *arr_end = _sw_match_bracket(arr);
    const char *q = arr + 1;
    while (*q && (!arr_end || q < arr_end)) {
        if (*q != '{') { q++; continue; }
        const char *obj_end = _sw_match_bracket(q);
        if (!obj_end) break;

        int idx = 0;
        const char *ip = strstr(q, "\"index\":");
        if (ip && ip < obj_end) idx = (int)strtol(ip + 8, NULL, 10);
        if (idx < 0 || idx >= SW_MAX_TOOL_CALLS) { q = obj_end + 1; continue; }
        _sw_toolcall_t *tc = &tcs[idx];
        tc->used = 1;
        if (idx + 1 > *tc_count) *tc_count = idx + 1;

        const char *idp = strstr(q, "\"id\":\"");
        if (idp && idp < obj_end && tc->id[0] == '\0') {
            const char *s = idp + 6;
            const char *e = _sw_json_str_end(s);
            if (e && e < obj_end) {
                size_t l = (size_t)(e - s);
                if (l > sizeof(tc->id) - 1) l = sizeof(tc->id) - 1;
                memcpy(tc->id, s, l); tc->id[l] = '\0';
            }
        }
        const char *np = strstr(q, "\"name\":\"");
        if (np && np < obj_end && tc->name[0] == '\0') {
            const char *s = np + 8;
            const char *e = _sw_json_str_end(s);
            if (e && e < obj_end) {
                size_t l = (size_t)(e - s);
                if (l > sizeof(tc->name) - 1) l = sizeof(tc->name) - 1;
                memcpy(tc->name, s, l); tc->name[l] = '\0';
            }
        }
        const char *ap = strstr(q, "\"arguments\":\"");
        if (ap && ap < obj_end) {
            const char *s = ap + 13;
            const char *e = _sw_json_str_end(s);
            if (e && e <= obj_end) {
                size_t l = (size_t)(e - s);
                if (tc->args_cap == 0) {
                    tc->args_cap = (l + 1 < 256) ? 256 : l + 64;
                    tc->args = (char *)malloc(tc->args_cap);
                    tc->args_len = 0;
                }
                if (tc->args_len + l + 1 > tc->args_cap) {
                    while (tc->args_len + l + 1 > tc->args_cap) tc->args_cap *= 2;
                    tc->args = (char *)realloc(tc->args, tc->args_cap);
                }
                memcpy(tc->args + tc->args_len, s, l);
                tc->args_len += l;
                tc->args[tc->args_len] = '\0';
            }
        }
        q = obj_end + 1;
    }
}

/* Build the tagged return for http_post_stream (cross-slice CONTRACT). The
 * sw caller `case`-branches on the leading atom:
 *   {'ok,    body}            — success; `body` is the OpenAI-shaped response
 *                               string (choices[0].message.{content,
 *                               reasoning_content,tool_calls}, plus usage), so
 *                               existing extract paths work after unwrapping.
 *   {'error, status, body}    — failure; `status` is the HTTP status int (0
 *                               for connection-refused / timeout / no-response
 *                               / parse-empty — anything where no HTTP code
 *                               was seen), `body` is the server's error body
 *                               or a human-readable transport reason.
 * Slice B's failure classifier keys on `status`: 0 or 5xx/408/429 → transient
 * (backoff-retry), 4xx (esp 400) / JSON-parse-error body → FATAL (stop, don't
 * resend the identical body). The 3-tuple error shape lets it tell those apart
 * — bare 2-tuples collapsed every failure together. */
static sw_val_t *_sw_hps_ok(const char *json) {
    sw_val_t *items[2];
    items[0] = sw_val_atom("ok");
    items[1] = sw_val_string(json ? json : "");
    return sw_val_tuple(items, 2);
}
static sw_val_t *_sw_hps_err3(int status, const char *body) {
    sw_val_t *items[3];
    items[0] = sw_val_atom("error");
    items[1] = sw_val_int((int64_t)status);
    items[2] = sw_val_string(body ? body : "");
    return sw_val_tuple(items, 3);
}
/* Back-compat shim for the early-return setup failures (bad args, temp-file
 * write, curl spawn): no HTTP exchange happened, so status is 0. */
static sw_val_t *_sw_hps_err(const char *reason) { return _sw_hps_err3(0, reason); }

/*
 * http_post_stream(url, headers_list, body, [target_pid, name]) → tagged tuple
 *
 * POSTs a JSON body with "stream": true to an OpenAI-compatible endpoint,
 * parses the Server-Sent-Events stream token-by-token, prints each
 * delta.content to stdout as it arrives (for visual streaming), and
 * returns a TAGGED tuple the caller branches on (cross-slice CONTRACT):
 *    {'ok,    "<openai-json>"}        on success
 *    {'error, status_int, "<body>"}   on curl-failure / non-2xx / parse-failure
 *                                     (status_int = HTTP code, or 0 when no
 *                                      HTTP code was seen — conn refused,
 *                                      timeout, stall, empty/parse failure)
 * The `ok` json keeps the minimal OpenAI response shape so the existing
 * extract_content path works after unwrapping:
 *    {"choices":[{"message":{"role":"assistant","content":"..."}}]}
 *
 * SSE parsing accepts both "data: {...}" and spec-legal "data:{...}"
 * (the space after the colon is optional per the WHATWG SSE spec).
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
        return _sw_hps_err("http_post_stream: bad arguments (need url, headers, body)");
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
    if (!bf) return _sw_hps_err("http_post_stream: cannot write request body to temp file");
    fputs(body, bf);
    fclose(bf);

    /* Redirect curl's stderr to a temp file so we can surface real errors
     * (ECONNREFUSED, NXDOMAIN, TLS failures, etc) instead of silently
     * returning empty content and leaving the user to guess. Previously
     * we piped stderr to /dev/null which hid every transport failure. */
    char err_file[256];
    snprintf(err_file, sizeof(err_file), "%s/sw_stream_err_%d_%u.txt",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());

    /* Prefill-aware stall guard (F3c). A long reasoning/prefill phase is
     * SILENT — the server emits zero bytes for many seconds while it builds
     * the KV cache — but a stream that goes dead MID-generation is a real
     * failure. curl's --speed-time can't tell the two apart (it counts from
     * the start of the transfer, so a slow first byte trips it), so we drop
     * curl's speed guard entirely and enforce TWO distinct deadlines in the
     * select loop below:
     *   - SWARM_CODE_PREFILL_TIMEOUT_MS : generous FIRST-BYTE / TTFT budget,
     *     measured from request send until the first stream byte arrives.
     *     Default 300_000 ms (5 min) — covers long prefills on slow/local
     *     models without killing a healthy-but-thinking request.
     *   - SWARM_CODE_STREAM_STALL_MS    : inter-byte stall guard, ARMED ONLY
     *     AFTER the first byte. Aborts if no byte arrives for this long once
     *     generation has started. Default 120_000 ms (2 min).
     * Either firing kills the child and is surfaced as a transport timeout
     * (curl exit 28 semantics) so the existing trunc_marker + sw retry path
     * handles recovery unchanged. */
    long prefill_timeout_ms = 300000;   /* 5 min TTFT budget */
    {
        const char *e = getenv("SWARM_CODE_PREFILL_TIMEOUT_MS");
        if (e) {
            long ms = atol(e);
            if (ms > 0) prefill_timeout_ms = (ms < 10000) ? 10000 : ms;
        }
    }
    long stall_ms = 120000;             /* 2 min inter-byte stall guard */
    {
        const char *stall_env = getenv("SWARM_CODE_STREAM_STALL_MS");
        if (stall_env) {
            long ms = atol(stall_env);
            if (ms > 0) stall_ms = (ms < 10000) ? 10000 : ms;
        }
    }

    /* Build a curl ARGV (no shell — url/headers can't inject) with -N for
     * unbuffered streaming.
     * --keepalive-time 30: send TCP keepalives so flaky long-distance
     *   routes (api.z.ai, sushi, anything overseas) don't silently drop
     *   an idle stream during long reasoning chains.
     * --retry 2 --retry-delay 1 --retry-connrefused --retry-all-errors:
     *   curl auto-retries connection failures BEFORE any data arrives;
     *   does NOT restart an already-streaming response (so safe for
     *   streaming). Catches transient SSL/connect timeouts (curl 28/35).
     * --max-time 1800: hard ceiling at 30 min — long reasoning is fine
     *   but eventually we want to surface a failure rather than hang.
     * NOTE (F3c): we deliberately DO NOT pass curl --speed-limit/--speed-time
     *   anymore — its stall window starts at transfer start, so a long silent
     *   prefill (zero bytes during reasoning) tripped it. The prefill-aware
     *   TTFT + inter-byte stall guards are enforced in the select loop below.
     * curl stderr is redirected to err_file by _sw_popen_argv. */
    char body_arg[300];
    snprintf(body_arg, sizeof(body_arg), "@%s", body_file);
    int nhdr = (headers && headers->type == SW_VAL_LIST) ? headers->v.tuple.count : 0;
    /* 19 fixed flags + 2 per header + (--data-binary, body_arg, url) + NULL.
     * The --write-out flag + format routes the final HTTP status code to
     * stderr (err_file) so we can detect non-2xx responses without
     * polluting the SSE stdout. */
    char **argv = (char **)malloc(sizeof(char *) * (19 + 2 * nhdr + 3 + 1));
    char **hdr_strs = (char **)malloc(sizeof(char *) * (nhdr > 0 ? nhdr : 1));
    int argc = 0, nhdr_alloc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-sS";
    argv[argc++] = "-N";
    argv[argc++] = "-X";
    argv[argc++] = "POST";
    argv[argc++] = "--connect-timeout";
    argv[argc++] = "30";
    argv[argc++] = "--max-time";
    argv[argc++] = "1800";
    argv[argc++] = "--keepalive-time";
    argv[argc++] = "30";
    argv[argc++] = "--retry";
    argv[argc++] = "2";
    argv[argc++] = "--retry-delay";
    argv[argc++] = "1";
    argv[argc++] = "--retry-connrefused";
    argv[argc++] = "--retry-all-errors";
    /* (F3c) No --speed-limit/--speed-time: the prefill-aware first-byte and
     * inter-byte stall guards are enforced in the select loop below so a
     * silent prefill is never mistaken for a dead stream. */
    /* %{stderr} routes the write-out to stderr (curl >= 7.63), which we
     * already redirect to err_file. We parse "SW_HTTP_CODE:NNN" back out
     * after the stream closes to branch ok/error on non-2xx statuses. */
    argv[argc++] = "--write-out";
    argv[argc++] = "%{stderr}SW_HTTP_CODE:%{http_code}\\n";
    if (headers && headers->type == SW_VAL_LIST) {
        for (int i = 0; i < headers->v.tuple.count; i++) {
            sw_val_t *h = headers->v.tuple.items[i];
            if (h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->v.str && h->v.tuple.items[1]->v.str) {
                const char *hk = h->v.tuple.items[0]->v.str;
                const char *hv = h->v.tuple.items[1]->v.str;
                size_t hl = strlen(hk) + strlen(hv) + 3;
                char *hs = (char *)malloc(hl);
                snprintf(hs, hl, "%s: %s", hk, hv);
                hdr_strs[nhdr_alloc++] = hs;
                argv[argc++] = "-H";
                argv[argc++] = hs;
            }
        }
    }
    argv[argc++] = "--data-binary";
    argv[argc++] = body_arg;
    argv[argc++] = (char *)url;
    argv[argc] = NULL;

    _sw_popen_pid_t ch = _sw_popen_argv(argv, err_file);
    FILE *pp = ch.fp;
    for (int i = 0; i < nhdr_alloc; i++) free(hdr_strs[i]);
    free(hdr_strs);
    free(argv);
    if (!pp) { swbs_unlink(body_file); swbs_unlink(err_file); return _sw_hps_err("http_post_stream: failed to spawn curl"); }

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
    /* Prefill-aware stall guard state (F3c). `first_byte_ms` stays 0 until the
     * first stream byte arrives; before then we enforce the generous TTFT
     * budget (prefill_timeout_ms). `last_byte_ms` is the wall-clock of the
     * most recent byte; once first_byte_ms is set we enforce the inter-byte
     * stall guard (stall_ms) against it. `stall_fired` records that one of
     * the two deadlines killed the child so we surface it like curl exit 28. */
    uint64_t first_byte_ms = 0;
    uint64_t last_byte_ms = t_start;
    int stall_fired = 0;

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
     * spinner ticking during dead air. Heap-allocated (see SW_HPS_*). */
    char *line = (char *)malloc(SW_HPS_LINE_CAP);
    size_t line_len = 0;
    char *readbuf = (char *)malloc(SW_HPS_READ_CAP);
    /* Per-delta token scratch for content + reasoning. Hoisted out of
     * the loop and onto the heap to keep the stack frame small. */
    char *tok = (char *)malloc(SW_HPS_TOK_CAP);
    char *rtok = (char *)malloc(SW_HPS_TOK_CAP);
    int done = 0;
    const int spinner_tick_ms = 80;

    /* Capture the first chunk of any NON-SSE body (lines that don't begin
     * with "data:"). When a server returns a non-2xx, the body is usually
     * a plain JSON error object on stdout rather than an SSE stream — we
     * surface it as the {'error, reason} payload instead of returning an
     * empty shell. Bounded; we only need enough to be actionable. */
    char errbody[1024];
    size_t errbody_len = 0;

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

    /* Native function-calling: per-index tool-call accumulators.
     * Reassembled from delta.tool_calls fragments (see _sw_parse_tc_deltas).
     * Heap-allocated — the array is ~22 KB and this function already runs
     * close to the swarmrt green-thread stack limit (line[16384] etc.). */
    _sw_toolcall_t *tcs = (_sw_toolcall_t *)calloc(SW_MAX_TOOL_CALLS, sizeof(_sw_toolcall_t));
    int tc_count = 0;

    while (!done) {
        /* Routed-mode kill honor: when the sw side ESCs/timeouts a worker
         * that is blocked in THIS builtin, exit_proc only sets the async
         * kill_flag — without this check the curl child kept generating for
         * up to --max-time (30 min), flooding stale chunks and, on a
         * single-slot local server, queueing the retry BEHIND the zombie.
         * Poll the flag each loop pass (≤ one spinner tick of latency),
         * kill the child, and bail exactly like a user interrupt. */
        {
            sw_process_t *hps_self = sw_self();
            if (hps_self && hps_self->kill_flag) {
                interrupted = 1;
                _sw_pkill_close(ch);
                pp = NULL;
                ch.fp = NULL;
                ch.pid = -1;
                done = 1;
                break;
            }
        }
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

        /* Check stdin for an interrupt keystroke. A bare Esc or Ctrl+C
         * aborts the stream, kills the child, and returns the partial
         * buffer with an [Interrupted] marker the model sees next turn.
         * Escape SEQUENCES (arrow/F-keys/paste — all 0x1b-prefixed) are
         * drained by _sw_stdin_is_interrupt and ignored, not aborted. */
        if (stdin_fd >= 0 && FD_ISSET(stdin_fd, &rfds)) {
            unsigned char ib;
            ssize_t nb = read(stdin_fd, &ib, 1);
            if (nb == 1 && _sw_stdin_is_interrupt(stdin_fd, ib)) {
                interrupted = 1;
                _sw_stdin_flush_pending_interrupts(stdin_fd);
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
            /* Prefill-aware stall guard (F3c). On every spinner tick check
             * the two deadlines:
             *   - Before the first byte: generous TTFT budget. A long silent
             *     prefill is healthy, so this window is wide (default 5 min).
             *   - After the first byte: inter-byte stall guard. A mid-stream
             *     stall (server hung, route dropped) is a real failure, so a
             *     tighter window (default 2 min since the last byte) aborts.
             * On a hit we kill the child and break; the EOF/classify path
             * treats a non-zero curl exit as a transport timeout (exit 28
             * semantics) and appends the cut-off marker. */
            uint64_t now_ms = _sw_now_ms();
            if (first_byte_ms == 0) {
                if ((uint64_t)(now_ms - t_start) >= (uint64_t)prefill_timeout_ms) {
                    stall_fired = 1;
                }
            } else {
                if ((uint64_t)(now_ms - last_byte_ms) >= (uint64_t)stall_ms) {
                    stall_fired = 1;
                }
            }
            if (stall_fired) {
                if (spinner_drawn) {
                    fputs("\r\x1b[K", stdout);
                    spinner_drawn = 0;
                    _sw_stream_reset_line(&stream);
                }
                _sw_pkill_close(ch);
                pp = NULL;
                ch.fp = NULL;
                ch.pid = -1;
                done = 1;
                break;
            }
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
                uint64_t elapsed = now_ms - t_start;
                int est_tokens = (int)(buf_len / 4);
                _sw_spinner_draw(verb, elapsed, est_tokens);
            }
            t_next_tick = _sw_now_ms() + (uint64_t)spinner_tick_ms;
            continue;
        }

        ssize_t rn = read(pipe_fd, readbuf, SW_HPS_READ_CAP);
        if (rn < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        if (rn == 0) { done = 1; break; }

        /* Bytes arrived — feed the prefill-aware stall guard (F3c). The first
         * byte switches the guard from the wide TTFT budget to the tighter
         * inter-byte window; every byte resets the inter-byte clock. */
        {
            uint64_t now_b = _sw_now_ms();
            if (first_byte_ms == 0) first_byte_ms = now_b;
            last_byte_ms = now_b;
        }

        for (ssize_t ri = 0; ri < rn && !done; ri++) {
            char ch = readbuf[ri];
            if (line_len < SW_HPS_LINE_CAP - 1) line[line_len++] = ch;
            if (ch != '\n') continue;
            line[line_len] = '\0';
            size_t this_line_len = line_len;
            line_len = 0;

            /* SSE field parse. The WHATWG spec makes the space after the
             * colon OPTIONAL: "data: {...}" and "data:{...}" are both legal,
             * and a single leading space (if present) is stripped. We used
             * to hard-require the 6-byte "data: " prefix, which silently
             * dropped every chunk from spec-legal servers that emit
             * "data:{...}" with no space — the read loop ran but the content
             * extractor below never fired, returning empty content that was
             * indistinguishable from "model said nothing". Now we match the
             * 5-byte "data:" prefix and skip at most one optional space. */
            if (this_line_len < 5 || strncmp(line, "data:", 5) != 0) {
                /* Not an SSE data line. Stash it (bounded) as a candidate
                 * error body — non-2xx responses arrive as a plain JSON
                 * blob here rather than as "data:" chunks. Skip pure
                 * blank/separator lines so the error reason stays clean. */
                if (errbody_len + this_line_len + 1 < sizeof(errbody)) {
                    const char *src = line;
                    size_t cpn = this_line_len;
                    /* trim a trailing newline so multiple lines join readably */
                    while (cpn > 0 && (src[cpn-1] == '\n' || src[cpn-1] == '\r')) cpn--;
                    if (cpn > 0) {
                        memcpy(errbody + errbody_len, src, cpn);
                        errbody_len += cpn;
                        errbody[errbody_len++] = ' ';
                        errbody[errbody_len] = '\0';
                    }
                }
                continue;
            }
            const char *json = line + 5;
            if (*json == ' ') json++;
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
                    size_t rtok_len = 0;
                    while (*rp && *rp != '"' && rtok_len < SW_HPS_TOK_CAP - 4) {
                        if (*rp == '\\' && *(rp + 1)) {
                            char esc = *(rp + 1);
                            switch (esc) {
                                case 'n': rtok[rtok_len++] = '\n'; rp += 2; break;
                                case 't': rtok[rtok_len++] = '\t'; rp += 2; break;
                                case 'r': rtok[rtok_len++] = '\r'; rp += 2; break;
                                case '"': rtok[rtok_len++] = '"'; rp += 2; break;
                                case '\\': rtok[rtok_len++] = '\\'; rp += 2; break;
                                case '/': rtok[rtok_len++] = '/'; rp += 2; break;
                                case 'u': {
                                    /* \uXXXX — decode to UTF-8 (with surrogate
                                     * pairs) so accents/em-dashes/CJK/emoji in
                                     * reasoning don't arrive as literal uXXXX. */
                                    rp += 2; /* skip backslash + 'u' */
                                    int cp = _json_parse_hex4(&rp);
                                    if (cp < 0) { rtok[rtok_len++] = 'u'; break; }
                                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                                        rp[0] == '\\' && rp[1] == 'u') {
                                        const char *save = rp;
                                        rp += 2;
                                        int low = _json_parse_hex4(&rp);
                                        if (low >= 0xDC00 && low <= 0xDFFF) {
                                            unsigned int full = 0x10000 +
                                                (((unsigned int)(cp - 0xD800)) << 10) +
                                                (unsigned int)(low - 0xDC00);
                                            rtok_len += _utf8_encode(full, rtok + rtok_len);
                                            break;
                                        }
                                        rp = save;
                                    }
                                    rtok_len += _utf8_encode((unsigned int)cp, rtok + rtok_len);
                                    break;
                                }
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

            /* Native function-calling channel: reassemble fragmented
             * tool_calls. Runs BEFORE the content `continue` below — a
             * tool-call-only delta usually carries no `content` field. */
            if (tcs && strstr(json, "\"tool_calls\":")) {
                _sw_parse_tc_deltas(json, tcs, &tc_count);
            }

            const char *p = strstr(json, "\"content\":\"");
            if (!p) continue;
            p += 11;

            size_t tok_len = 0;
            while (*p && *p != '"' && tok_len < SW_HPS_TOK_CAP - 4) {
                if (*p == '\\' && *(p + 1)) {
                    char esc = *(p + 1);
                    switch (esc) {
                        case 'n': tok[tok_len++] = '\n'; p += 2; break;
                        case 't': tok[tok_len++] = '\t'; p += 2; break;
                        case 'r': tok[tok_len++] = '\r'; p += 2; break;
                        case '"': tok[tok_len++] = '"'; p += 2; break;
                        case '\\': tok[tok_len++] = '\\'; p += 2; break;
                        case '/': tok[tok_len++] = '/'; p += 2; break;
                        case 'u': {
                            /* \uXXXX — decode to UTF-8 (with surrogate pairs).
                             * Without this, non-ASCII content streamed by a
                             * server that \u-escapes (em-dash, accents, CJK,
                             * emoji) landed as literal uXXXX in the prose AND
                             * the stored assistant message. */
                            p += 2; /* skip backslash + 'u' */
                            int cp = _json_parse_hex4(&p);
                            if (cp < 0) { tok[tok_len++] = 'u'; break; }
                            if (cp >= 0xD800 && cp <= 0xDBFF &&
                                p[0] == '\\' && p[1] == 'u') {
                                const char *save = p;
                                p += 2;
                                int low = _json_parse_hex4(&p);
                                if (low >= 0xDC00 && low <= 0xDFFF) {
                                    unsigned int full = 0x10000 +
                                        (((unsigned int)(cp - 0xD800)) << 10) +
                                        (unsigned int)(low - 0xDC00);
                                    tok_len += _utf8_encode(full, tok + tok_len);
                                    break;
                                }
                                p = save;
                            }
                            tok_len += _utf8_encode((unsigned int)cp, tok + tok_len);
                            break;
                        }
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

    /* EOF — a final line with no trailing newline never hit the per-line
     * handler in the read loop. For non-2xx responses the JSON error body is
     * exactly that: a single line with no terminator. Flush it into errbody
     * (if it isn't an SSE "data:" chunk) so the {'error, ...} reason carries
     * the server's message instead of "(no response body)". */
    if (line_len > 0 && !(line_len >= 5 && strncmp(line, "data:", 5) == 0)) {
        size_t cpn = line_len;
        while (cpn > 0 && (line[cpn-1] == '\n' || line[cpn-1] == '\r')) cpn--;
        if (cpn > 0 && errbody_len + cpn + 1 < sizeof(errbody)) {
            memcpy(errbody + errbody_len, line, cpn);
            errbody_len += cpn;
            errbody[errbody_len++] = ' ';
            errbody[errbody_len] = '\0';
        }
    }
    line_len = 0;

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
        /* Commit the last word — content rarely ends on whitespace,
         * so without this the final word would stay buffered + lost. */
        _sw_stream_flush_word(&stream);
        /* Record the physical rows the CONTENT region occupied (one more
         * than the newlines emitted, since the last line has no trailing
         * emitter newline). The markdown repaint reads this via
         * stream_content_rows() to clear exactly the streamed prose. */
        _sw_last_content_rows = stream.rows + 1;
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
    /* The prefill-aware stall guard (F3c) already killed the child above, so
     * pp is NULL and we never read a real exit code. Synthesise curl's
     * timeout exit (28) so the failure classifier + trunc_marker treat a
     * TTFT/inter-byte stall identically to curl's own --max-time abort. */
    if (stall_fired) curl_exit = 28;
    swbs_unlink(body_file);

    /* Read curl's stderr once. It carries two things now:
     *   1. transport error text (ECONNREFUSED, NXDOMAIN, TLS, curl 28) and
     *   2. our "SW_HTTP_CODE:NNN" from --write-out '%{stderr}...'
     * We parse the status code out and strip that marker line so the
     * remaining text is just the human-readable transport error (if any). */
    char errbuf[1024];
    size_t erlen = 0;
    {
        FILE *ef = fopen(err_file, "r");
        if (ef) {
            erlen = fread(errbuf, 1, sizeof(errbuf) - 1, ef);
            fclose(ef);
        }
        errbuf[erlen] = '\0';
    }
    int http_code = -1;
    {
        char *m = strstr(errbuf, "SW_HTTP_CODE:");
        if (m) {
            http_code = (int)strtol(m + 13, NULL, 10);
            /* Excise the whole "SW_HTTP_CODE:NNN" token (+ trailing newline)
             * so it never leaks into the user-facing error text. */
            char *after = m + 13;
            while (*after >= '0' && *after <= '9') after++;
            if (*after == '\n') after++;
            size_t tail = strlen(after);
            memmove(m, after, tail + 1);
            erlen = strlen(errbuf);
        }
    }
    /* Strip trailing whitespace for cleaner display/return. */
    while (erlen > 0 && (errbuf[erlen - 1] == '\n' || errbuf[erlen - 1] == '\r' ||
                         errbuf[erlen - 1] == ' '  || errbuf[erlen - 1] == '\t')) {
        errbuf[--erlen] = '\0';
    }
    swbs_unlink(err_file);

    /* Classify failure. Three error families the agent can branch on:
     *   - curl_exit != 0           → transport/process failure (conn refused,
     *                                 DNS, TLS, timeout=28). errbuf has detail.
     *   - http_code >= 400         → server returned a non-2xx HTTP status;
     *                                 errbody (non-SSE body) carries the JSON
     *                                 error object the server sent.
     *   - buf_len == 0 && no usage → stream parsed but yielded no content AND
     *     && no tool calls           no tool calls: a parse failure or truly
     *                                 empty (we treat empty-without-tool-calls
     *                                 as a parse/empty error so the agent can
     *                                 retry instead of forwarding a blank turn).
     * Interrupt and finish_reason="length" are NOT errors — they're partial
     * successes that carry real (if truncated) content, so they stay {'ok,...}
     * with the truncation marker appended below. */
    int is_curl_fail = (curl_exit != 0 && !interrupted);
    int is_http_fail = (http_code >= 400);
    const char *fail_reason = NULL;
    /* The HTTP status threaded out per the CONTRACT: the real code on a
     * non-2xx, else 0 (transport/timeout/stall/parse — no HTTP code seen).
     * Slice B keys retry-vs-fatal on this. */
    int fail_status = 0;
    char fail_buf[1280];
    if (is_curl_fail) {
        snprintf(fail_buf, sizeof(fail_buf), "curl exit %d: %s", curl_exit,
                 erlen > 0 ? errbuf : "(no stderr captured — check the URL/endpoint)");
        fail_reason = fail_buf;
        fail_status = 0;   /* conn refused / DNS / TLS / timeout(28) / stall */
    } else if (is_http_fail) {
        snprintf(fail_buf, sizeof(fail_buf), "HTTP %d: %s", http_code,
                 errbody_len > 0 ? errbody : (erlen > 0 ? errbuf : "(no response body)"));
        fail_reason = fail_buf;
        fail_status = http_code;
    }

    /* Surface curl/HTTP failures on screen as before (visual parity with the
     * pre-tagged behavior) — the tagged {'error, ...} is what the agent
     * branches on, but the human still sees the warning inline. */
    if (fail_reason) {
        if (so.subagent) {
            /* Routed/subagent mode: a transport/HTTP failure is NOT content —
             * tag it 'stream_err' so the sw renderer paints a gated ⚠ line
             * instead of feeding it to the markdown/prose pipeline (which
             * rendered errors as assistant text and bumped the token count). */
            _stream_out_send_tagged(so.target, "stream_err", so.name, fail_reason);
        } else if (is_tty) {
            /* Only paint the inline ⚠ on a real terminal. */
            fprintf(stdout, "\n  \x1b[38;5;208m⚠ %s\x1b[0m\n", fail_reason);
            fflush(stdout);
        } else {
            /* Piped/redirected (headless -p capture, JSON-RPC stream): the
             * tagged {'error, ...} return value is the caller's channel, so
             * keep stdout clean — but still surface the detail on stderr so
             * it's debuggable via 2>. */
            fprintf(stderr, "swarm-code: %s\n", fail_reason);
        }
    }

    /* Append a marker the model will see in history if the turn was
     * cut short. Three cases:
     *   1. User hit ESC/Ctrl+C  → "[Request interrupted by user]"
     *   2. curl timed out (28)  → "[Response cut off by transport timeout]"
     *   3. finish_reason="length" → "[Response truncated at max_tokens limit — increase max_tokens and retry]"
     * Claude Code uses similar markers so the model knows the
     * previous response was incomplete. */
    /* Snapshot whether the stream produced anything real BEFORE we append
     * any truncation marker (the marker would otherwise mask an empty turn).
     * "Real" = streamed content OR at least one reassembled tool call. */
    int produced_output = (buf_len > 0) || (tc_count > 0);

    const char *trunc_marker = NULL;
    if (interrupted) {
        trunc_marker = "\n\n[Request interrupted by user]";
    } else if (stall_fired && first_byte_ms == 0) {
        trunc_marker = "\n\n[No response from the model before the first-byte (prefill) "
                       "timeout — the request never started streaming. Check the endpoint "
                       "is up, or raise SWARM_CODE_PREFILL_TIMEOUT_MS for a very slow model.]";
    } else if (stall_fired) {
        trunc_marker = "\n\n[Stream stalled mid-generation — no bytes arrived within the "
                       "inter-byte timeout. Retry, or raise SWARM_CODE_STREAM_STALL_MS.]";
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
         * In subagent/routed mode surface it as 'stream_err' — a status
         * marker, not content — so the parent paints a ⚠ line instead of
         * rendering it as assistant prose. */
        if (so.subagent) {
            _stream_out_send_tagged(so.target, "stream_err", so.name, trunc_marker);
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

    /* Assemble the native tool_calls array (if any streamed). The
     * argument bytes are already JSON-escaped — see _sw_parse_tc_deltas
     * — so they go in verbatim. Slots without a name never got a real
     * first delta and are skipped. The whole field is empty when the
     * turn produced no tool calls (the common chat-only case). */
    char *tcenc = NULL;
    size_t tcenc_len = 0;
    if (tc_count > 0) {
        size_t cap = 512;
        tcenc = (char *)malloc(cap);
        size_t l = (size_t)snprintf(tcenc, cap, ",\"tool_calls\":[");
        int emitted = 0;
        for (int i = 0; i < tc_count; i++) {
            _sw_toolcall_t *tc = &tcs[i];
            if (!tc->used || tc->name[0] == '\0') continue;
            const char *args = (tc->args && tc->args_len > 0) ? tc->args : "{}";
            char idbuf[32];
            const char *idstr = tc->id[0] ? tc->id : idbuf;
            if (!tc->id[0]) snprintf(idbuf, sizeof(idbuf), "call_%d", i);
            size_t need = strlen(args) + strlen(idstr) + strlen(tc->name) + 96;
            while (l + need > cap) { cap *= 2; tcenc = (char *)realloc(tcenc, cap); }
            l += (size_t)snprintf(tcenc + l, cap - l,
                "%s{\"id\":\"%s\",\"type\":\"function\","
                "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}",
                emitted ? "," : "", idstr, tc->name, args);
            emitted++;
        }
        if (emitted) {
            if (l + 2 > cap) { cap += 2; tcenc = (char *)realloc(tcenc, cap); }
            tcenc[l++] = ']';
            tcenc[l] = '\0';
            tcenc_len = l;
        } else {
            free(tcenc);
            tcenc = NULL;
        }
    }
    const char *tc_field = tcenc ? tcenc : "";

    size_t out_cap = ep + rep + tcenc_len + 512;
    char *out = (char *)malloc(out_cap);
    /* Include the usage block so sw callers can read real token counts
     * from the server. Missing values (no usage chunk seen) are omitted
     * rather than sent as 0 to distinguish "unknown" from "zero". */
    if (prompt_tokens >= 0) {
        snprintf(out, out_cap,
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"%s\",\"reasoning_content\":\"%s\"%s}}],"
            "\"usage\":{\"prompt_tokens\":%lld,\"completion_tokens\":%lld,\"total_tokens\":%lld}}",
            enc, renc, tc_field,
            prompt_tokens,
            completion_tokens >= 0 ? completion_tokens : 0,
            total_tokens >= 0 ? total_tokens : prompt_tokens + (completion_tokens >= 0 ? completion_tokens : 0));
    } else {
        snprintf(out, out_cap,
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"%s\",\"reasoning_content\":\"%s\"%s}}]}",
            enc, renc, tc_field);
    }

    /* Decide the tagged return per the CONTRACT. Precedence:
     *   1. curl/transport failure   → {'error, 0,      "curl exit N: ..."}
     *   2. non-2xx HTTP status      → {'error, status, "HTTP NNN: <body>"}
     *   3. stream parsed but empty  → {'error, 0,      "stream produced no..."}
     *      (no content AND no tool calls AND not a deliberate interrupt)
     *   4. otherwise                → {'ok, "<openai-json>"}
     * An interrupt with no content is still {'ok,...} carrying the
     * interrupt marker — the caller asked to stop, that's not a failure.
     * Status 0 = "no HTTP code seen" (transport/timeout/stall/parse-empty),
     * which Slice B treats as transient-but-bounded. */
    sw_val_t *result;
    if (fail_reason) {
        result = _sw_hps_err3(fail_status, fail_reason);
    } else if (!produced_output && !interrupted) {
        result = _sw_hps_err3(0,
            "stream produced no content and no tool calls "
            "(SSE parse yielded nothing — check the endpoint emits "
            "OpenAI-shaped 'data: {\"choices\":[{\"delta\":{\"content\":...}}]}' chunks)");
    } else {
        result = _sw_hps_ok(out);
    }
    free(enc);
    free(renc);
    if (tcenc) free(tcenc);
    if (tcs) {
        for (int i = 0; i < SW_MAX_TOOL_CALLS; i++) {
            if (tcs[i].args) free(tcs[i].args);
        }
        free(tcs);
    }
    free(reasoning);
    free(out);
    free(buffer);
    free(line);
    free(readbuf);
    free(tok);
    free(rtok);

    /* Tell the parent we're done — useful as a sentinel without having
     * to grep the chunk stream. */
    if (so.subagent) {
        sw_val_t *items[2];
        items[0] = sw_val_atom("stream_done");
        items[1] = sw_val_string(so.name);
        sw_val_t *msg = sw_val_tuple(items, 2);
        sw_send_value(so.target, SW_TAG_NONE, msg);   /* GC v1: copy off sender arena */
    }
    return result;
}

/* === Supervisor === */

typedef struct {
    sw_val_t *fn;
} _sup_child_closure_t;

/* Free a child-start closure (a deep_copy_global tree). Used as
 * sw_child_spec_t.free_start_arg so otp.c/phase4.c can reclaim the MASTER, and as
 * a child fiber's on_destroy hook to reclaim its private COPY. */
static void _free_sup_child_closure(void *raw) {
    _sup_child_closure_t *c = (_sup_child_closure_t *)raw;
    if (c) { _sw_free_global_val(c->fn); free(c); }
}

/* Make a fresh per-incarnation copy of a child-start closure (its own
 * deep_copy_global of fn). Used as sw_child_spec_t.copy_start_arg so each child
 * incarnation owns an independent closure it frees in process_destroy, leaving
 * the supervisor's master untouched. */
static void *_sup_copy_child_closure(void *raw) {
    _sup_child_closure_t *src = (_sup_child_closure_t *)raw;
    _sup_child_closure_t *cp = (_sup_child_closure_t *)malloc(sizeof(_sup_child_closure_t));
    cp->fn = (src && src->fn) ? sw_val_deep_copy_global(src->fn) : NULL;
    return cp;
}

static void _sup_child_entry(void *raw) {
    _sup_child_closure_t *c = (_sup_child_closure_t *)raw;
    /* `c` is THIS incarnation's private copy (made by _sup_copy_child_closure at
     * spawn). It is freed in process_destroy via on_destroy = _free_sup_child_closure,
     * armed by sw_spawn_link_dtor BEFORE this fiber was runnable — crash-safe (a
     * panic never returns here) AND pre-trampoline-kill-safe. The supervisor's
     * master is a separate allocation freed at child removal/teardown. */
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
        /* GC v1: the supervisor outlives the caller and re-applies fn on every
         * restart — deep-copy the closure to the global heap so it survives the
         * caller's arena being freed. */
        c->fn = fn_v ? sw_val_deep_copy_global(fn_v) : NULL;
        specs[valid].start_func = _sup_child_entry;
        specs[valid].start_arg = c;                              /* MASTER */
        specs[valid].copy_start_arg = _sup_copy_child_closure;   /* per-child copy */
        specs[valid].free_start_arg = _free_sup_child_closure;
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
    sup_spec.owns_children = 1;   /* heap specs[]: the supervisor frees it post-copy */

    sw_process_t *sup = sw_supervisor_start("sw_sup", &sup_spec);
    if (!sup) free(specs);   /* spawn failed: no supervisor to take ownership */
    return sup ? sw_val_pid(sup) : sw_val_nil();
}

/* === DynamicSupervisor (runtime start_child) ===
 *
 * A one_for_one supervisor that begins with zero children and accepts new
 * supervised children at runtime — one per incoming call / request. The
 * native runtime is sw_dynsup_* (swarmrt_phase4.c, already in the core lib
 * and fuzz-covered); these builtins are just the sw-level surface, mirroring
 * _builtin_supervise's child-spec shape exactly:
 *
 *   {name, fun() { ... }, :permanent | :temporary | :transient}
 */

/* dyn_supervisor()                       -> pid (max_restarts=3, max_seconds=5)
 * dyn_supervisor(max_restarts, max_secs) -> pid
 * Returns the supervisor pid, or nil on failure. */
static sw_val_t *_builtin_dyn_supervisor(sw_val_t **a, int n) {
    sw_dynsup_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.max_restarts = 3;
    spec.max_seconds = 5;
    spec.max_children = 0; /* unlimited up to SW_DYNSUP_MAX_CHILDREN */
    if (n >= 1 && a[0] && a[0]->type == SW_VAL_INT) spec.max_restarts = (uint32_t)a[0]->v.i;
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_INT) spec.max_seconds = (uint32_t)a[1]->v.i;

    sw_process_t *sup = sw_dynsup_start("sw_dynsup", &spec);
    return sup ? sw_val_pid(sup) : sw_val_nil();
}

/* sup_start_child(sup, {name, fn, restart}) -> child pid | nil
 * Builds one sw_child_spec_t from the tuple and asks the supervisor to
 * spawn+monitor it. Same closure contract as supervise(): fn is held by
 * reference and applied (and re-applied on restart) by the child process. */
static sw_val_t *_builtin_sup_start_child(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_PID || !a[0]->v.pid) return sw_val_nil();
    sw_val_t *child = a[1];
    if (!child || child->type != SW_VAL_TUPLE || child->v.tuple.count < 2) return sw_val_nil();

    sw_val_t *name_v = child->v.tuple.items[0];
    sw_val_t *fn_v = child->v.tuple.items[1];
    if (!fn_v || fn_v->type != SW_VAL_FUN) return sw_val_nil();

    sw_child_spec_t spec;
    memset(&spec, 0, sizeof(spec));

    if (name_v && (name_v->type == SW_VAL_ATOM || name_v->type == SW_VAL_STRING) && name_v->v.str)
        strncpy(spec.name, name_v->v.str, sizeof(spec.name) - 1);
    else
        spec.name[0] = '\0'; /* anonymous */

    _sup_child_closure_t *c = (_sup_child_closure_t *)malloc(sizeof(_sup_child_closure_t));
    /* GC v1: deep-copy the closure — the dynamic supervisor holds it for
     * restarts beyond the caller's lifetime (see static supervise above). */
    c->fn = fn_v ? sw_val_deep_copy_global(fn_v) : NULL;
    spec.start_func = _sup_child_entry;
    spec.start_arg = c;                              /* MASTER */
    spec.copy_start_arg = _sup_copy_child_closure;   /* per-child copy */
    spec.free_start_arg = _free_sup_child_closure;
    spec.restart = SW_PERMANENT;

    if (child->v.tuple.count >= 3) {
        sw_val_t *restart_v = child->v.tuple.items[2];
        if (restart_v && restart_v->type == SW_VAL_ATOM && restart_v->v.str) {
            if (strcmp(restart_v->v.str, "temporary") == 0) spec.restart = SW_TEMPORARY;
            else if (strcmp(restart_v->v.str, "transient") == 0) spec.restart = SW_TRANSIENT;
        }
    }

    sw_process_t *proc = sw_dynsup_start_child_proc(a[0]->v.pid, &spec);
    /* spec (incl. start_arg=MASTER + copy/free fn ptrs) is copied by value into
     * the supervisor's child node, which owns the master thereafter: it hands each
     * (re)started child a fresh copy (copy_start_arg) and frees the master
     * (free_start_arg) only on permanent removal / teardown. */
    return proc ? sw_val_pid(proc) : sw_val_nil();
}

/* sup_terminate_child(sup, child) -> 'ok' | 'error' */
static sw_val_t *_builtin_sup_terminate_child(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_PID || !a[0]->v.pid ||
        !a[1] || a[1]->type != SW_VAL_PID || !a[1]->v.pid)
        return sw_val_atom("error");
    int rc = sw_dynsup_terminate_child_proc(a[0]->v.pid, a[1]->v.pid);
    return sw_val_atom(rc == 0 ? "ok" : "error");
}

/* sup_count_children(sup) -> int */
static sw_val_t *_builtin_sup_count_children(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_PID || !a[0]->v.pid) return sw_val_int(0);
    return sw_val_int((int64_t)sw_dynsup_count_children_proc(a[0]->v.pid));
}

/* === Tool registry — self-defined sw tools, hot-loaded from source =========
 *
 * The AI-era headline: an agent writes a new tool AS sw source at runtime and
 * registers it, callable on the next turn with NO restart. Each tool keeps its
 * own parsed-module interpreter alive; tool_call invokes the tool's `run`
 * function. Each tool_call gets a FRESH per-call interpreter off the tool's
 * immutable AST (sw_lang_call_fresh) — so concurrent callers never share mutable
 * state and a fault never poisons the tool — and runs on a dedicated 8MB-stack
 * thread (the parser/eval recurse on the C stack, and a 128KB process fiber is
 * too small; the helper thread also makes the interpreter's pthread-bounds stack
 * guard measure the right stack, so deep tool recursion errors cleanly instead
 * of crashing, and process primitives degrade to nil since sw_self() is NULL
 * there). Re-defining swaps the AST and RETAINS the previous for tool_rollback.
 * Entry fn is `run`. Pure-logic tools only.
 */
#define SW_TOOL_MAX 256
#define SW_TOOL_HIST 16           /* per-tool audit-log depth (last N versions) */
typedef struct {
    char name[64];
    void *ast;                /* current parsed-module AST (immutable, shared) */
    void *prev_ast;           /* previous version, retained for rollback */
    char *src;                /* current source (for audit/history) */
    char *prev_src;
    uint64_t version;
    /* Audit log: every defined version kept as replayable source (ring of the
     * last SW_TOOL_HIST). Makes self-modification auditable: tool_history(name)
     * returns [{version, src}] so nothing the agent wrote about itself is lost. */
    char    *hist_src[SW_TOOL_HIST];
    uint64_t hist_ver[SW_TOOL_HIST];
    int      hist_count;      /* total versions ever recorded (>= entries kept) */
} sw_tool_entry_t;
static sw_tool_entry_t g_tools[SW_TOOL_MAX];
static int g_tool_count = 0;
static pthread_rwlock_t g_tool_lock = PTHREAD_RWLOCK_INITIALIZER;

static sw_tool_entry_t *_tool_find(const char *name) {
    for (int i = 0; i < g_tool_count; i++)
        if (strcmp(g_tools[i].name, name) == 0) return &g_tools[i];
    return NULL;
}

static sw_val_t *_tool_err(const char *reason) {
    sw_val_t *items[2] = { sw_val_atom("error"), sw_val_string(reason) };
    return sw_val_tuple(items, 2);
}

/* The recursive-descent parser uses ~14KB/frame (8KB by-value tokens), which
 * overflows a process fiber's modest stack on nested expressions. Parsing is
 * rare (once per tool_define) and produces a heap AST that's thread-agnostic,
 * so we run it on a dedicated 8MB-stack thread — the same headroom swc's own
 * main thread has, where the parser is known to handle any program. eval
 * (tool_call) stays on the fiber; tool bodies are modest. */
typedef struct { const char *src; void *ast; } _tool_parse_ctx;
static void *_tool_parse_thread(void *arg) {
    _tool_parse_ctx *c = (_tool_parse_ctx *)arg;
    c->ast = sw_lang_parse(c->src);
    return NULL;
}
static void *_tool_parse_big_stack(const char *src) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return sw_lang_parse(src);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    _tool_parse_ctx ctx = { src, NULL };
    pthread_t t;
    int rc = pthread_create(&t, &attr, _tool_parse_thread, &ctx);
    pthread_attr_destroy(&attr);
    if (rc != 0) return sw_lang_parse(src); /* fallback: current stack */
    pthread_join(t, NULL);
    return ctx.ast;
}

/* Run a tool's `run` on an 8MB-stack thread with a fresh per-call interpreter.
 * The result is a heap-owned sw_val tree (thread-agnostic), valid after join.
 * On the helper thread sw_self()==NULL, so process primitives degrade to nil
 * (a tool can't send/receive/spawn into the host scheduler). */
typedef struct { void *ast; const char *fn; sw_val_t **args; int n; sw_val_t *result; } _tool_eval_ctx;
static void *_tool_eval_thread(void *arg) {
    _tool_eval_ctx *c = (_tool_eval_ctx *)arg;
    c->result = sw_lang_call_fresh(c->ast, c->fn, c->args, c->n);
    return NULL;
}
static sw_val_t *_tool_eval_big_stack(void *ast, const char *fn, sw_val_t **args, int n) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return sw_lang_call_fresh(ast, fn, args, n);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    _tool_eval_ctx ctx = { ast, fn, args, n, NULL };
    pthread_t t;
    int rc = pthread_create(&t, &attr, _tool_eval_thread, &ctx);
    pthread_attr_destroy(&attr);
    if (rc != 0) return sw_lang_call_fresh(ast, fn, args, n); /* fallback */
    pthread_join(t, NULL);
    return ctx.result;
}

/* tool_define(name, src) -> 'ok' | {'error', reason}
 * Parse `src` (a module defining `fn run(...)`) and register it under `name`.
 * Re-defining swaps in the new version and keeps the old for rollback. */
static sw_val_t *_builtin_tool_define(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM) ||
        !a[0]->v.str || !a[1] || a[1]->type != SW_VAL_STRING || !a[1]->v.str)
        return _tool_err("tool_define(name, src): name + source string required");
    /* Reject over-length names LOUDLY rather than silently truncating into the
     * name[64] buffer (a truncated name never matches on tool_call). */
    if (strlen(a[0]->v.str) > sizeof(g_tools[0].name) - 1)
        return _tool_err("tool name too long (max 63 chars)");

    void *ast = _tool_parse_big_stack(a[1]->v.str);
    if (!ast) return _tool_err("parse error in tool source");
    if (!sw_lang_has_fun(ast, "run"))
        return _tool_err("tool source must define `fun run(...)` (the entry point)");
    /* Admission lint: reject hallucinated/undefined-fn calls and dangerous
     * builtins not granted via the optional caps list, LOUDLY at define time
     * (not a silent nil three calls later). caps = a[2] (a list of atoms);
     * absent => pure-logic only. */
    {
        sw_val_t *caps = (n >= 3) ? a[2] : NULL;
        char lint_err[256];
        if (sw_lang_lint_tool(ast, caps, lint_err, sizeof(lint_err)))
            return _tool_err(lint_err);
    }

    pthread_rwlock_wrlock(&g_tool_lock);
    sw_tool_entry_t *e = _tool_find(a[0]->v.str);
    if (!e) {
        if (g_tool_count >= SW_TOOL_MAX) {
            pthread_rwlock_unlock(&g_tool_lock);
            return _tool_err("tool registry full");
        }
        e = &g_tools[g_tool_count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, a[0]->v.str, sizeof(e->name) - 1);
    }
    /* retain old AST for rollback — never freed (a thread may be mid-call) */
    e->prev_ast = e->ast;
    e->prev_src = e->src;
    e->ast = ast;
    e->src = strdup(a[1]->v.str);
    e->version++;
    /* audit log: record this version into the ring (last SW_TOOL_HIST kept). */
    {
        int slot = e->hist_count % SW_TOOL_HIST;
        if (e->hist_src[slot]) free(e->hist_src[slot]);   /* overwrite oldest */
        e->hist_src[slot] = strdup(a[1]->v.str);
        e->hist_ver[slot] = e->version;
        e->hist_count++;
    }
    pthread_rwlock_unlock(&g_tool_lock);
    return sw_val_atom("ok");
}

/* tool_history(name) -> list of {version, src} tuples, oldest→newest (the last
 * SW_TOOL_HIST versions). Every self-written version as replayable source. */
static sw_val_t *_builtin_tool_history(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM) || !a[0]->v.str)
        return sw_val_list(NULL, 0);
    pthread_rwlock_rdlock(&g_tool_lock);
    sw_tool_entry_t *e = _tool_find(a[0]->v.str);
    if (!e || e->hist_count == 0) { pthread_rwlock_unlock(&g_tool_lock); return sw_val_list(NULL, 0); }
    int kept = e->hist_count < SW_TOOL_HIST ? e->hist_count : SW_TOOL_HIST;
    int start = e->hist_count - kept;            /* index of oldest kept version */
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * kept);
    for (int i = 0; i < kept; i++) {
        int slot = (start + i) % SW_TOOL_HIST;
        sw_val_t *pair[2] = { sw_val_int((int64_t)e->hist_ver[slot]),
                              sw_val_string(e->hist_src[slot] ? e->hist_src[slot] : "") };
        items[i] = sw_val_tuple(pair, 2);
    }
    pthread_rwlock_unlock(&g_tool_lock);
    sw_val_t *r = sw_val_list(items, kept);
    free(items);
    return r;
}

/* tool_call(name, args...) -> result | nil
 * Invoke the tool's `run` function with the trailing args. */
static sw_val_t *_builtin_tool_call(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM) || !a[0]->v.str)
        return sw_val_nil();
    pthread_rwlock_rdlock(&g_tool_lock);
    sw_tool_entry_t *e = _tool_find(a[0]->v.str);
    void *ast = e ? e->ast : NULL;
    pthread_rwlock_unlock(&g_tool_lock);
    if (!ast) return sw_val_nil();
    /* eval outside the lock on the snapshot AST (immutable, retained); fresh
     * per-call interpreter on an 8MB thread (isolation + stack + degrade). */
    return _tool_eval_big_stack(ast, "run", &a[1], n - 1);
}

/* tool_list() -> list of {name, version} tuples for every registered tool. */
static sw_val_t *_builtin_tool_list(sw_val_t **a, int n) {
    (void)a; (void)n;
    pthread_rwlock_rdlock(&g_tool_lock);
    int cnt = g_tool_count;
    if (cnt == 0) { pthread_rwlock_unlock(&g_tool_lock); return sw_val_list(NULL, 0); }
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cnt);
    for (int i = 0; i < cnt; i++) {
        sw_val_t *pair[2] = { sw_val_string(g_tools[i].name),
                              sw_val_int((int64_t)g_tools[i].version) };
        items[i] = sw_val_tuple(pair, 2);
    }
    pthread_rwlock_unlock(&g_tool_lock);
    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    return r;
}

/* tool_rollback(name) -> 'ok' | {'error', reason}
 * Swap a tool back to its previous version (toggles, so calling again rolls
 * forward). The retained-never-freed ASTs make this a pure pointer swap. */
static sw_val_t *_builtin_tool_rollback(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || (a[0]->type != SW_VAL_STRING && a[0]->type != SW_VAL_ATOM) || !a[0]->v.str)
        return _tool_err("tool_rollback(name): name required");
    pthread_rwlock_wrlock(&g_tool_lock);
    sw_tool_entry_t *e = _tool_find(a[0]->v.str);
    if (!e) { pthread_rwlock_unlock(&g_tool_lock); return _tool_err("no such tool"); }
    if (!e->prev_ast) { pthread_rwlock_unlock(&g_tool_lock); return _tool_err("no previous version to roll back to"); }
    void *ta = e->ast; e->ast = e->prev_ast; e->prev_ast = ta;
    char *ts = e->src; e->src = e->prev_src; e->prev_src = ts;
    e->version++;
    pthread_rwlock_unlock(&g_tool_lock);
    return sw_val_atom("ok");
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

    sw_val_t *keys[12], *vals[12];
    int c = 0;
    keys[c] = sw_val_atom("pid");     vals[c] = sw_val_int((int64_t)proc->pid); c++;
    keys[c] = sw_val_atom("status");
    /* Load the _Atomic state into a plain enum before the switch — some clang
     * versions reject switching directly on an _Atomic-qualified type ("statement
     * requires expression of integer type"). Implicit atomic-to-plain load. */
    sw_proc_state_t pi_state = proc->state;
    switch (pi_state) {
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
    /* memory: bytes handed out by this process's VALUE ARENA
     * (varena->total_bytes — the same number SW_PROC_MEM_MAX meters; 0 when
     * the process has no arena, e.g. SW_GC_OFF). Read UNDER link_lock:
     * process_destroy detaches proc->varena under the same lock before
     * freeing it, so a reader here sees either a live arena header or NULL —
     * never freed-but-non-NULL (the reg_entry lesson below, same shape).
     * total_bytes itself is owner-written without a lock; this is a
     * best-effort stat snapshot like the counters above. */
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
    /* mailbox_len: PENDING (undrained) mailbox depth — the counter the
     * SW_MAILBOX_MAX admission cap checks. 'messages' above only counts the
     * drained private queue; this one covers the signal stack too. */
    keys[c] = sw_val_atom("mailbox_len");
    vals[c] = sw_val_int((int64_t)atomic_load_explicit(&proc->mb_len, memory_order_relaxed)); c++;
    keys[c] = sw_val_atom("messages_sent"); vals[c] = sw_val_int((int64_t)proc->messages_sent); c++;
    keys[c] = sw_val_atom("messages_recv"); vals[c] = sw_val_int((int64_t)proc->messages_recv); c++;
    /* parent pid (numeric, matching the int 'pid' field above) so Swarm.tree()
     * can reconstruct the supervision hierarchy from the child->parent link.
     * Best-effort read of the arena slab, like process_list — the slot memory
     * never unmaps, so a stale parent at most mis-groups a child as a root. */
    /* Snapshot the pointer ONCE: `if (proc->parent) ... proc->parent->pid`
     * compiles to two loads, and a concurrent exit can null the field between
     * them (UBSan: member access within null pointer — caught by the
     * process_info_uaf repro under the stack-guard's perturbed timing). The
     * slot behind a stale non-NULL snapshot never unmaps, per the note above. */
    {
        sw_process_t *par = proc->parent;
        if (par) { keys[c] = sw_val_atom("parent"); vals[c] = sw_val_int((int64_t)par->pid); c++; }
    }
    /* reg_entry is malloc'd at registration and free()d ONLY under
     * registry.lock (registry_remove_proc). A process exiting / crash-restarting
     * on another scheduler frees it concurrently, so reading ->name unlocked is a
     * use-after-free: ASan reports heap-use-after-free in strlen, and in
     * production the freed+reallocated entry corrupts the heap -> SIGBUS in the
     * sw_val_map_new() below. Read the name under the rdlock (as _builtin_registered
     * does); sw_val_string copies it out, so it stays valid after we unlock. */
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

/* swarm_stats() → node-level runtime metrics map. Every field is a REAL
 * counter the runtime already maintains — nothing is fabricated:
 *   processes        live process count (best-effort slab scan, as process_list)
 *   schedulers       scheduler thread count
 *   spawns           processes spawned since boot        (_Atomic total_spawns)
 *   crashes          abnormal process exits since boot   (sw_proc_crashes)
 *   restarts         supervisor child restarts, static + dynamic (sw_restarts_total)
 *   mailbox_dropped  messages dropped by the SW_MAILBOX_MAX depth cap
 *   msgsize_dropped  messages dropped by the SW_MSG_MAX_BYTES size cap
 *   overflow_queue   global overflow run-queue length (work no scheduler
 *                    has claimed; read under its own cold mutex)
 *   scheduler_stats  per-scheduler list of %{id, procs_run, loop_iters,
 *                    idle_waits, steals} — best-effort lock-free reads of the
 *                    scheduler debug counters (the watchdog contract; a stale
 *                    value is fine, a lock on the hot path is not)
 * NOT exposed because the runtime does not track them: per-scheduler
 * run-queue depth (the Vyukov MPSC has no length counter), scheduler
 * utilization %, node-total reductions, and node-total sends
 * (g_swarm->total_reductions / total_sends are DEAD counters in this
 * runtime — only the unlinked legacy swarmrt_proc.c bumps them; wiring
 * total_sends would put a shared-cacheline atomic on the send hot path.
 * Per-process reductions / messages_sent in process_info cover both). */
static sw_val_t *_builtin_swarm_stats(sw_val_t **a, int n) {
    (void)a; (void)n;
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
    /* draining: 1 once graceful shutdown has begun — a readiness probe
     * (/readyz) fails on this so a load balancer stops routing here. */
    keys[c] = sw_val_atom("draining");        vals[c] = sw_val_atom(sw_is_draining() ? "true" : "false"); c++;
    keys[c] = sw_val_atom("scheduler_stats"); vals[c] = sched_list; c++;
    return sw_val_map_new(keys, vals, c);
}

/* map_new() → empty map */
static sw_val_t *_builtin_map_new(sw_val_t **a, int n) {
    (void)a; (void)n;
    return sw_val_map_new(NULL, NULL, 0);
}

/* map_get(map, key) → value or nil */
static sw_val_t *_builtin_map_get(sw_val_t **a, int n) {
    if (n < 2) return sw_val_nil();
    sw_val_t *v = sw_val_map_get(a[0], a[1]);
    /* 3-arg overload: map_get(m, k, default) → default when the key is
     * absent (i.e. lookup yields nil). 2-arg form keeps returning nil. */
    if (n >= 3 && (!v || v->type == SW_VAL_NIL)) return a[2];
    return v;
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
/* _sw_error is a PER-PROCESS slot (sw_self_error_slot, swarmrt_native.h), NOT a
 * thread-local: a scheduler-thread-local leaked across the context switch a
 * blocking op performs — a try/catch resuming after sleep()/receive() would
 * catch an UNRELATED process's error, whose value (in that process's arena) is
 * freed on its exit -> use-after-free. The macro redirects every generated
 * `_sw_error` (here + the codegen try/catch) to the current process's slot. */
#define _sw_error (*sw_self_error_slot())

/* Compiled try/catch frame. The generated N_TRY allocates one as a C local —
 * it lives on the process's FIBER stack, so the setjmp state survives a
 * mid-try yield + resume on another scheduler thread — links it through the
 * per-process chain head (sw_self_try_chain), and setjmps. error() unwinds by
 * longjmp to the innermost frame: full dynamic extent, so an error() raised
 * in a CALLEE lands in the caller's catch, matching the interpreter and every
 * exception system users come from. With no live frame, error() keeps the
 * documented no-try behavior: silent, execution continues with nil. Panics
 * never use this chain — they stay uncatchable (process death). */
#include <setjmp.h>
typedef struct _sw_try_frame {
    jmp_buf jb;
    struct _sw_try_frame *prev;
} _sw_try_frame_t;
#define _sw_try_chain (*(_sw_try_frame_t **)sw_self_try_chain())

static sw_val_t *_builtin_error(sw_val_t **a, int n) {
    _sw_error = (n >= 1) ? a[0] : sw_val_string("error");
    if (_sw_try_chain) longjmp(_sw_try_chain->jb, 1);
    return sw_val_nil();
}

/* Generated execution state (line/file/call-trace) is PER PROCESS, reached via
 * `_sw_gen` (swarmrt_native.h) — swapped on every context switch. These macros
 * redirect the generated `_sw_current_line = N; _sw_current_file = "..."` writes
 * and the trace push/pop + panic readers below to the running process's block,
 * so a panic after a blocking op reports THIS process's location/call chain, not
 * a fiber that happened to share the scheduler thread. (`_sw_frame_t` and the
 * sw_gen_exec_t fields are declared in swarmrt_native.h, included above.) */
#define _sw_current_line     (_sw_gen->current_line)
#define _sw_current_file     (_sw_gen->current_file)
#define _sw_trace            (_sw_gen->trace)
#define _sw_trace_top        (_sw_gen->trace_top)
#define _sw_trace_overflowed (_sw_gen->trace_overflowed)

/* Print the current call stack to stderr, top of stack first. Reads the
 * running process's call-stack ring buffer (via the _sw_trace* macros above),
 * which the codegen maintains through _sw_trace_push / _sw_trace_pop. */
static void _sw_print_trace(void) {
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
    /* Falsy = nil OR the 'false' atom — interpreter parity (its expect
     * always treated 'false' as a failure; this one only caught nil, so
     * expect(x == y, msg) compiled to a no-op pass-through on mismatch). */
    int is_falsy = (!a[0] || a[0]->type == SW_VAL_NIL ||
                    (a[0]->type == SW_VAL_ATOM && a[0]->v.str &&
                     (strcmp(a[0]->v.str, "nil") == 0 ||
                      strcmp(a[0]->v.str, "false") == 0)));
    if (!is_falsy) return a[0];
    sw_val_t *msg = (n >= 2) ? a[1] : sw_val_string("expected non-nil value, got nil/false");
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
                for (int j = 0; j < sz; j++)
                    snprintf(hex + j * 2, 3, "%02x", bytes[j]);
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

    /* 3-arg overload: db_exec(h, sql, [args]) — prepare + bind + step, no
     * rows returned. Lets writes (INSERT/UPDATE/DELETE) use placeholders
     * instead of misusing db_query for a side-effecting statement. */
    if (n >= 3 && a[2] && a[2]->type == SW_VAL_LIST) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(_sw_sqlite_db[slot], a[1]->v.str, -1, &stmt, NULL) != SQLITE_OK)
            return sw_val_string(sqlite3_errmsg(_sw_sqlite_db[slot]));
        for (int i = 0; i < a[2]->v.tuple.count; i++)
            _sw_db_bind(stmt, i + 1, a[2]->v.tuple.items[i]);
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { /* drain any rows */ }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_OK)
            return sw_val_string(sqlite3_errmsg(_sw_sqlite_db[slot]));
        return sw_val_atom("ok");
    }

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

    char outf[256];
    snprintf(outf, sizeof(outf), "%s/sw_http_out_%d_%u.json",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());

    /* curl ARGV — no shell, so the URL and header values (caller-supplied)
     * can't inject. Output goes to -o outf, which we then read back. */
    int nhdr = (n >= 2 && a[1] && a[1]->type == SW_VAL_LIST) ? a[1]->v.tuple.count : 0;
    /* 6 fixed + 2 per header + (url, -o, outf) + NULL */
    char **argv = (char **)malloc(sizeof(char *) * (6 + 2 * nhdr + 3 + 1));
    char **hdr_strs = (char **)malloc(sizeof(char *) * (nhdr > 0 ? nhdr : 1));
    int argc = 0, nhdr_alloc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-sS";
    argv[argc++] = "--connect-timeout";
    argv[argc++] = "30";
    argv[argc++] = "--max-time";
    /* note: --max-time value 120 below */
    argv[argc++] = "120";
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_LIST) {
        for (int i = 0; i < a[1]->v.tuple.count; i++) {
            sw_val_t *h = a[1]->v.tuple.items[i];
            if (h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->v.str && h->v.tuple.items[1]->v.str) {
                const char *hk = h->v.tuple.items[0]->v.str;
                const char *hv = h->v.tuple.items[1]->v.str;
                size_t hl = strlen(hk) + strlen(hv) + 3;
                char *hs = (char *)malloc(hl);
                snprintf(hs, hl, "%s: %s", hk, hv);
                hdr_strs[nhdr_alloc++] = hs;
                argv[argc++] = "-H";
                argv[argc++] = hs;
            }
        }
    }
    argv[argc++] = (char *)url;
    argv[argc++] = "-o";
    argv[argc++] = outf;
    argv[argc] = NULL;

    char *resp = NULL;
    size_t resp_len = 0;
    int delays[] = {0, 3, 10};
    for (int attempt = 0; attempt < 3; attempt++) {
        if (delays[attempt] > 0) sw_sleep(delays[attempt]);
        /* spawn curl (no shell) and wait for it; output lands in outf */
        _sw_popen_pid_t ch = _sw_popen_argv(argv, NULL);
        if (ch.fp) _sw_popen_pid_close(ch);
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
    for (int i = 0; i < nhdr_alloc; i++) free(hdr_strs[i]);
    free(hdr_strs);
    free(argv);
    if (!resp) return sw_val_string("");
    sw_val_t *r = sw_val_string(resp);
    free(resp);
    return r;
}

/* ============================================================
 * http_request(url, opts) → %{status, body, headers} | {'error', reason}
 * ============================================================
 *
 * The status-aware sibling of http_get/http_post. Those return body-or-nil
 * and HIDE the HTTP status, so a caller can't tell a 200 from a 4xx/5xx that
 * carries a body (the voice-agent Telnyx REST port had to guess from the JSON
 * envelope shape — see voice-agent/docs/RUNBOOK.md). http_request wires the
 * status code AND the response headers through, leaving http_post/http_get
 * untouched (they're widely used; changing their shape would break callers).
 *
 *   url   : string (required)
 *   opts  : map (optional) with keys —
 *             method  : string, e.g. "GET"/"POST"/"PUT"/"DELETE" (default GET)
 *             headers : a MAP {name=>value} OR a list of {name, value} tuples
 *             body    : string request body (sent via --data-binary)
 *
 * Returns a 3-key MAP on a completed transport:
 *   %{ status:  <int  http status code>,
 *      body:    <string response body>,
 *      headers: <map of response headers, keys lowercased> }
 * or {'error', <reason-string>} when the request never completed (curl could
 * not be spawned, the host was unreachable, the transfer timed out, …).
 *
 * Implementation: one curl invocation via _sw_popen_argv (execvp, no shell —
 * url/header/method values are literal argv elements, never shell-interpreted).
 *   -o BODYFILE      response body
 *   -D HEADERFILE    response header block (status line + headers)
 *   -w %{http_code}  the numeric status, printed to curl's stdout (we read it)
 * curl exits 0 on any HTTP response (incl. 4xx/5xx); a non-zero exit / empty
 * %{http_code} means no response arrived → {'error', ...}.
 */

/* Lowercase a header NAME in place (matches the request-side convention in
 * swarmrt_http.c http_parse_headers, so handler and client see the same keys). */
static void _sw_hr_lower(char *s) {
    /* ASCII-only, no <ctype.h> — the codegen prelude doesn't include it. */
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

/* Parse a curl -D dump (CRLF- or LF-terminated) into a sw MAP. The first line
 * is the HTTP status line ("HTTP/1.1 418 ...") and is skipped; each remaining
 * "Name: Value" line becomes a lowercased-name entry. Trailing CR and leading
 * value spaces are trimmed. A bare blank line ends the block (curl appends one;
 * with redirects there can be several blocks — last-wins, which is correct). */
static sw_val_t *_sw_hr_parse_headers(const char *raw) {
    sw_val_t *m = sw_val_map_new(NULL, NULL, 0);
    if (!raw) return m;
    const char *p = raw;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        /* Strip a trailing CR. */
        size_t L = linelen;
        if (L > 0 && p[L - 1] == '\r') L--;
        if (L == 0) { /* blank line: end of a header block */
            if (!eol) break;
            p = eol + 1;
            continue;
        }
        /* Status line ("HTTP/...") — reset the map so the LAST response block
         * (after any redirects) is what the caller sees, then skip it. */
        if (L >= 5 && strncmp(p, "HTTP/", 5) == 0) {
            m = sw_val_map_new(NULL, NULL, 0);
        } else {
            const char *colon = (const char *)memchr(p, ':', L);
            if (colon) {
                size_t klen = (size_t)(colon - p);
                const char *vp = colon + 1;
                size_t vlen = L - klen - 1;
                while (vlen > 0 && (*vp == ' ' || *vp == '\t')) { vp++; vlen--; }
                char *k = (char *)malloc(klen + 1);
                memcpy(k, p, klen); k[klen] = 0;
                _sw_hr_lower(k);
                char *v = (char *)malloc(vlen + 1);
                memcpy(v, vp, vlen); v[vlen] = 0;
                sw_val_t *kk = sw_val_string(k);
                sw_val_t *vv = sw_val_string(v);
                m = sw_val_map_put(m, kk, vv);
                free(k); free(v);
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return m;
}

/* Read an entire file into a NUL-terminated heap buffer (caller frees).
 * *out_len gets the byte length (NUL excluded). Returns NULL on open failure. */
static char *_sw_hr_slurp(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { if (out_len) *out_len = 0; return NULL; }
    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    char chunk[8192];
    size_t rd;
    while ((rd = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (len + rd + 1 > cap) { while (len + rd + 1 > cap) cap *= 2; buf = (char *)realloc(buf, cap); }
        memcpy(buf + len, chunk, rd);
        len += rd;
    }
    fclose(fp);
    buf[len] = 0;
    if (out_len) *out_len = len;
    return buf;
}

static sw_val_t *_sw_hr_error(const char *reason) {
    sw_val_t *items[2];
    items[0] = sw_val_atom("error");
    items[1] = sw_val_string(reason);
    return sw_val_tuple(items, 2);
}

static sw_val_t *_builtin_http_request(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return _sw_hr_error("http_request: url must be a string");
    const char *url = a[0]->v.str;

    /* opts (optional map): method / headers / body */
    sw_val_t *opts = (n >= 2) ? a[1] : NULL;
    const char *method = "GET";
    const char *body = NULL;
    sw_val_t *headers = NULL;   /* MAP or LIST of {k,v} */
    if (opts && opts->type == SW_VAL_MAP) {
        sw_val_t *km = sw_val_string("method");
        sw_val_t *mv = sw_val_map_get(opts, km);
        if (mv && mv->type == SW_VAL_STRING && mv->v.str[0]) method = mv->v.str;
        sw_val_t *kb = sw_val_string("body");
        sw_val_t *bv = sw_val_map_get(opts, kb);
        if (bv && bv->type == SW_VAL_STRING) body = bv->v.str;
        sw_val_t *kh = sw_val_string("headers");
        sw_val_t *hv = sw_val_map_get(opts, kh);
        if (hv && (hv->type == SW_VAL_MAP || hv->type == SW_VAL_LIST)) headers = hv;
    }

    /* Temp files: request body, response body, response header dump. */
    char body_file[256], out_file[256], hdr_file[256];
    snprintf(body_file, sizeof(body_file), "%s/sw_hr_b_%d_%u",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    snprintf(out_file, sizeof(out_file), "%s/sw_hr_o_%d_%u",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());
    snprintf(hdr_file, sizeof(hdr_file), "%s/sw_hr_h_%d_%u",
             sw_tmpdir(), sw_getpid_os(), sw_random_u32());

    int have_body = (body != NULL);
    if (have_body) {
        FILE *bf = fopen(body_file, "wb");
        if (!bf) return _sw_hr_error("http_request: cannot write request body");
        fwrite(body, 1, strlen(body), bf);
        fclose(bf);
    }
    char body_arg[300];
    snprintf(body_arg, sizeof(body_arg), "@%s", body_file);

    /* Count headers for argv sizing. headers is a MAP or a LIST of {k,v}. */
    int nhdr = 0;
    if (headers && headers->type == SW_VAL_MAP)  nhdr = headers->v.map.count;
    if (headers && headers->type == SW_VAL_LIST) nhdr = headers->v.tuple.count;

    /* fixed: curl -sS -X METHOD --connect-timeout 30 --max-time 300
     *        -o out -D hdr -w %{http_code} url  → 14 + 2/hdr + (--data-binary,arg) + NULL */
    char **argv = (char **)malloc(sizeof(char *) * (14 + 2 * nhdr + 2 + 2));
    char **hdr_strs = (char **)malloc(sizeof(char *) * (nhdr > 0 ? nhdr : 1));
    int argc = 0, nhdr_alloc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-sS";
    argv[argc++] = "-X";
    argv[argc++] = (char *)method;
    argv[argc++] = "--connect-timeout";
    argv[argc++] = "30";
    argv[argc++] = "--max-time";
    argv[argc++] = "300";
    argv[argc++] = "-o";
    argv[argc++] = out_file;
    argv[argc++] = "-D";
    argv[argc++] = hdr_file;
    argv[argc++] = "-w";
    argv[argc++] = "%{http_code}";
    if (headers && headers->type == SW_VAL_MAP) {
        for (int i = 0; i < headers->v.map.count; i++) {
            sw_val_t *hk = headers->v.map.keys[i];
            sw_val_t *hvv = headers->v.map.vals[i];
            if (hk && hk->type == SW_VAL_STRING && hvv && hvv->type == SW_VAL_STRING) {
                size_t hl = strlen(hk->v.str) + strlen(hvv->v.str) + 3;
                char *hs = (char *)malloc(hl);
                snprintf(hs, hl, "%s: %s", hk->v.str, hvv->v.str);
                hdr_strs[nhdr_alloc++] = hs;
                argv[argc++] = "-H";
                argv[argc++] = hs;
            }
        }
    } else if (headers && headers->type == SW_VAL_LIST) {
        for (int i = 0; i < headers->v.tuple.count; i++) {
            sw_val_t *h = headers->v.tuple.items[i];
            if (h && h->type == SW_VAL_TUPLE && h->v.tuple.count >= 2 &&
                h->v.tuple.items[0]->type == SW_VAL_STRING &&
                h->v.tuple.items[1]->type == SW_VAL_STRING) {
                const char *hk = h->v.tuple.items[0]->v.str;
                const char *hvv = h->v.tuple.items[1]->v.str;
                size_t hl = strlen(hk) + strlen(hvv) + 3;
                char *hs = (char *)malloc(hl);
                snprintf(hs, hl, "%s: %s", hk, hvv);
                hdr_strs[nhdr_alloc++] = hs;
                argv[argc++] = "-H";
                argv[argc++] = hs;
            }
        }
    }
    if (have_body) {
        argv[argc++] = "--data-binary";
        argv[argc++] = body_arg;
    }
    argv[argc++] = (char *)url;
    argv[argc] = NULL;

    /* Spawn curl and read its stdout — the -w %{http_code} string.
     * Flush our own stdout FIRST: _sw_popen_argv fork()s, and a child that
     * inherits an unflushed block-buffered stdout buffer can drop/duplicate a
     * byte of the parent's next write (seen as a missing leading char when
     * http_request is the program's very first output). Flushing before the
     * fork makes the parent's buffer state unambiguous across it. */
    fflush(stdout);
    char code_buf[64];
    size_t code_len = 0;
    code_buf[0] = 0;
    _sw_popen_pid_t ch = _sw_popen_argv(argv, NULL);
    if (ch.fp) {
        char rb[64];
        size_t rd;
        while ((rd = fread(rb, 1, sizeof(rb), ch.fp)) > 0) {
            for (size_t i = 0; i < rd && code_len + 1 < sizeof(code_buf); i++)
                code_buf[code_len++] = rb[i];
        }
        code_buf[code_len] = 0;
        _sw_popen_pid_close(ch);
    }

    /* http_code is a decimal int curl prints last (000 == no response). */
    long status = strtol(code_buf, NULL, 10);

    /* Read body + header dump regardless (body may exist even on 4xx/5xx). */
    size_t blen = 0;
    char *resp_body = _sw_hr_slurp(out_file, &blen);
    char *resp_hdrs = _sw_hr_slurp(hdr_file, NULL);

    /* Cleanup temp files + argv scratch. */
    if (have_body) swbs_unlink(body_file);
    swbs_unlink(out_file);
    swbs_unlink(hdr_file);
    for (int i = 0; i < nhdr_alloc; i++) free(hdr_strs[i]);
    free(hdr_strs);
    free(argv);

    /* No spawn / no response code → transport failure. */
    if (!ch.fp || status <= 0) {
        free(resp_body);
        free(resp_hdrs);
        return _sw_hr_error(code_buf[0] ? "http_request: transport failure"
                                        : "http_request: curl failed to spawn");
    }

    /* Build %{status, body, headers}. */
    sw_val_t *hmap = _sw_hr_parse_headers(resp_hdrs);
    sw_val_t *keys[3], *vals[3];
    keys[0] = sw_val_string("status");  vals[0] = sw_val_int((int64_t)status);
    keys[1] = sw_val_string("body");    vals[1] = sw_val_string(resp_body ? resp_body : "");
    keys[2] = sw_val_string("headers"); vals[2] = hmap;
    sw_val_t *r = sw_val_map_new(keys, vals, 3);

    free(resp_body);
    free(resp_hdrs);
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

    /* Poll for the exit file with an ADAPTIVE backoff (2ms -> 250ms).
     *
     * This used to sleep a FLAT 1 second before the first check, which put a
     * 1s floor under EVERY shell() call. That floor froze swarm-code's whole
     * UI (2026-07-09): the heartbeat handler ran a shell()-based prune on the
     * main fiber every 2s tick, ticks queued up faster than main could drain
     * them during long turns, and typed input sat behind an ever-growing
     * backlog. Fast commands now return in single-digit milliseconds; the
     * live-tail progress display only engages once the backoff has slowed to
     * >=50ms (a genuinely long-running command). */
    int is_tty = isatty(fileno(stdout));
    int done = 0;
    int displayed = 0;             /* progress tail actually drawn? */
    useconds_t delay_us = 2000;    /* 2ms, doubling to a 250ms cap */
    long long waited_us = 0;
    const long long max_wait_us = 120LL * 1000000;  /* hard cap (was 600s):
        bound the worst case if a backgrounded child is killed before writing
        its exit file (the orphan-wedge). swarm-code's own tools no longer rely
        on shell()'s poll for long commands — they use shell_managed (own
        pgroup + C timeout + killpg). 120s stays well above any legit
        fast/local shell() use. */
    off_t last_size = 0;

    while (!done && waited_us < max_wait_us) {
        /* Check if command finished — BEFORE sleeping, so an already-done
         * command costs one fopen, not a poll interval. */
        FILE *ef = fopen(exitf, "r");
        if (ef) { fclose(ef); done = 1; break; }

        usleep(delay_us);
        waited_us += delay_us;
        if (delay_us < 250000) {
            delay_us *= 2;
            if (delay_us > 250000) delay_us = 250000;
        }

        /* Show live tail if terminal — only once the command has proven
         * slow (backoff at/above 50ms), so quick shells never touch the
         * input row at all. */
        if (is_tty && delay_us >= 50000) {
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

                    /* Input-line-aware: when read_line is engaged we
                     * lock the terminal, wipe, draw the tail snippet,
                     * then redraw the input below — so the user's
                     * typing isn't eaten by progress updates. */
                    pthread_mutex_lock(&_sw_term_lock);
                    displayed = 1;
                    int _act = _sw_rl.active;
                    if (_act) {
                        _sw_rl_wipe_unlocked();   /* erase ALL input rows */
                        fprintf(stdout, "    \033[38;5;240m⎿ %s\033[0m\n",
                                line_buf);
                        if (_sw_rl.cur_buf && *_sw_rl.cur_buf) {
                            _sw_rl_redraw_unlocked(_sw_rl.cur_prompt,
                                *_sw_rl.cur_buf,
                                _sw_rl.cur_len ? *_sw_rl.cur_len : 0,
                                _sw_rl.cur_cursor ? *_sw_rl.cur_cursor : 0);
                        } else { fflush(stdout); }
                    } else {
                        fprintf(stdout, "\r\033[K    \033[38;5;240m⎿ %s\033[0m",
                                line_buf);
                        fflush(stdout);
                    }
                    pthread_mutex_unlock(&_sw_term_lock);
                }
            }
        }
    }
    /* Clear the progress line — and redraw the input box below if the
     * line editor is mid-read, otherwise the wipe eats the user's typing.
     * ONLY when a progress tail was actually drawn: an unconditional
     * \r\e[K here meant every fast shell() erased whatever row the cursor
     * was on (the pinned input prompt, once per heartbeat prune — the
     * "input field not properly rendered" churn). */
    if (is_tty && displayed) {
        pthread_mutex_lock(&_sw_term_lock);
        if (_sw_rl.active && _sw_rl.cur_buf && *_sw_rl.cur_buf) {
            _sw_rl_wipe_unlocked();   /* erase ALL input rows */
            _sw_rl_redraw_unlocked(_sw_rl.cur_prompt,
                *_sw_rl.cur_buf,
                _sw_rl.cur_len ? *_sw_rl.cur_len : 0,
                _sw_rl.cur_cursor ? *_sw_rl.cur_cursor : 0);
        } else { fputs("\r\033[K", stdout); fflush(stdout); }
        pthread_mutex_unlock(&_sw_term_lock);
    }

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

/* shell_managed(cmd, timeout_ms) → {exit_code, output, interrupted}
 *
 * Like shell() but built for arbitrary, possibly-runaway MODEL-supplied
 * commands. Three guarantees shell() can't make:
 *   1. The command runs in its OWN process group (via _sw_popen_pid's
 *      setpgid), so on timeout/interrupt we killpg the whole subtree —
 *      no leaked node/python/server grandchildren.
 *   2. While it runs we select() on stdin (only when the line editor has
 *      already put the tty in raw mode, i.e. _sw_rl.saved_ok). An ESC
 *      (0x1b) or Ctrl-C (0x03) keystroke kills the process group and
 *      returns immediately — this is what makes a hung tool interruptible
 *      from the REPL, mirroring the LLM-stream interrupt path above.
 *   3. The wall-clock timeout is enforced in C; when it fires we killpg.
 *
 * Returns a 3-tuple read by sw via elem(r,0/1/2):
 *   elem 0: exit_code (int) — natural code, or 124 (timeout) / 130 (key).
 *   elem 1: output (string) — sanitized stdout (caller folds stderr via 2>&1).
 *   elem 2: interrupted (atom 'true'|'false') — true on timeout OR keystroke.
 * timeout_ms <= 0 means "no wall-clock limit" ONLY when an interactive raw
 * tty is present (an ESC keystroke can then stop it). With no such tty
 * (headless, piped stdin, a worker whose stdin isn't the terminal) we impose
 * a hard ceiling instead, so a child that never EOFs (tail -f, a daemon)
 * can never wedge us unkillably.
 *
 * Every exit path — including errors — returns the SAME {code, output,
 * interrupted} 3-tuple shape so sw callers can always elem(r,0/1/2) safely.
 *
 * We deliberately NEVER call _sw_rl_setup()/_sw_rl_restore() here: the tool
 * worker is a different process from the reader that owns termios, so
 * flipping the terminal mode would race the reader and could leave it cooked
 * or steal the user's in-progress line. We only READ when it is already raw. */
static sw_val_t *_sw_managed_tuple(int code, const char *msg) {
    sw_val_t *it[3];
    it[0] = sw_val_int(code);
    it[1] = sw_val_string(msg);
    it[2] = sw_val_atom("false");
    return sw_val_tuple(it, 3);
}

static sw_val_t *_builtin_shell_managed(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return _sw_managed_tuple(-1, "error: shell_managed needs a string command");
#ifdef _WIN32
    return _sw_managed_tuple(-1, "error: shell_managed unsupported on this platform");
#else
    const char *cmd = a[0]->v.str;
    int64_t timeout_ms =
        (n >= 2 && a[1] && a[1]->type == SW_VAL_INT) ? a[1]->v.i : 0;

    int stdin_fd = -1;
    if (isatty(STDIN_FILENO) && _sw_rl.saved_ok) stdin_fd = STDIN_FILENO;

    /* Never allow an uninterruptible, unbounded run: with no raw tty to catch
     * an ESC, a non-positive timeout would otherwise loop forever on a child
     * that holds stdout open. Impose a 10-minute ceiling so the C deadline
     * still fires. (Both in-tree callers floor the timeout, so this only
     * guards direct/future callers — but it keeps the "can't hang" invariant.) */
    if (timeout_ms <= 0 && stdin_fd < 0) timeout_ms = 600000;

    _sw_popen_pid_t ch = _sw_popen_pid(cmd);
    if (!ch.fp) return _sw_managed_tuple(-1, "error: failed to launch command");

    int pipe_fd = fileno(ch.fp);
    int fl = fcntl(pipe_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(pipe_fd, F_SETFL, fl | O_NONBLOCK);

    size_t cap = 65536, raw_len = 0;
    char *raw = (char *)malloc(cap);
    if (!raw) { _sw_pkill_close(ch); return _sw_managed_tuple(-1, "error: out of memory"); }
    raw[0] = 0;

    uint64_t t_start = _sw_now_ms();
    int interrupted = 0;   /* set on a keystroke OR a timeout */
    int timed_out = 0;
    int done = 0;
    int oom = 0;

    while (!done) {
        /* Remaining wall-clock budget; tick at least once per second so a
         * huge timeout still re-checks, and so a quiet child can't wedge us. */
        int64_t remaining_ms = 1000;
        if (timeout_ms > 0) {
            int64_t elapsed = (int64_t)(_sw_now_ms() - t_start);
            if (elapsed >= timeout_ms) { timed_out = 1; interrupted = 1; break; }
            remaining_ms = timeout_ms - elapsed;
            if (remaining_ms > 1000) remaining_ms = 1000;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipe_fd, &rfds);
        if (stdin_fd >= 0) FD_SET(stdin_fd, &rfds);
        int max_fd = pipe_fd;
        if (stdin_fd > max_fd) max_fd = stdin_fd;

        struct timeval tv;
        tv.tv_sec  = remaining_ms / 1000;
        tv.tv_usec = (remaining_ms % 1000) * 1000;

        int ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;  /* tick — re-check the deadline at loop top */

        /* Check stdin BEFORE the pipe so an interrupt keystroke wins a tie. */
        if (stdin_fd >= 0 && FD_ISSET(stdin_fd, &rfds)) {
            unsigned char ib;
            ssize_t nb = read(stdin_fd, &ib, 1);
            if (nb == 1 && _sw_stdin_is_interrupt(stdin_fd, ib)) {
                interrupted = 1;
                _sw_stdin_flush_pending_interrupts(stdin_fd);
                break;
            }
            /* other keystrokes (arrow/F-keys drained by the helper): ignore */
        }

        if (FD_ISSET(pipe_fd, &rfds)) {
            char chunk[8192];
            ssize_t rn = read(pipe_fd, chunk, sizeof(chunk));
            if (rn == 0) { done = 1; }              /* EOF — child closed stdout */
            else if (rn < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                done = 1;
            } else {
                if (raw_len + (size_t)rn + 1 > cap) {
                    while (raw_len + (size_t)rn + 1 > cap) cap *= 2;
                    char *tmp = (char *)realloc(raw, cap);
                    if (!tmp) { oom = 1; break; }   /* keep old `raw` to free below */
                    raw = tmp;
                }
                memcpy(raw + raw_len, chunk, rn);
                raw_len += (size_t)rn;
            }
        }
    }

    /* Terminate + reap. On interrupt/timeout/oom, killpg the whole group so no
     * grandchild survives; on natural EOF just reap. Never touch ch twice. */
    int status;
    if (interrupted || oom) {
        _sw_pkill_close(ch);
        status = timed_out ? 124 : 130;
    } else {
        int wstatus = _sw_popen_pid_close(ch);
        status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    }
    if (oom) { free(raw); return _sw_managed_tuple(-1, "error: out of memory capturing output"); }
    raw[raw_len] = 0;

    /* Sanitize: keep printable ASCII + \t \n \r (binary poisons the model). */
    size_t len = 0;
    char *buf = (char *)malloc(raw_len + 64);
    if (!buf) { free(raw); return _sw_managed_tuple(-1, "error: out of memory"); }
    for (size_t i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 0x20 && c <= 0x7E))
            buf[len++] = (char)c;
    }
    buf[len] = 0;
    free(raw);
    if (len == 0 && raw_len > 0) {
        snprintf(buf, 63, "[binary output — %zu bytes, not text]", raw_len);
        len = strlen(buf);
    }

    sw_val_t *items[3];
    items[0] = sw_val_int(status);
    items[1] = sw_val_string(buf);
    items[2] = sw_val_atom(interrupted ? "true" : "false");
    sw_val_t *r = sw_val_tuple(items, 3);
    free(buf);
    return r;
#endif
}

/* read_key(timeout_ms) → int (the byte value) | nil
 *
 * A single, timeout-bounded raw key read for interrupt-watching while a
 * (non-shell) tool runs in a worker. Returns the byte (e.g. 27=ESC, 3=Ctrl-C)
 * if a key arrives within timeout_ms, else nil (timeout / no key / EOF).
 * Reads ONLY when the tty is already in raw mode (_sw_rl.saved_ok) — exactly
 * like shell_managed's stdin watch — so it never blocks on a cooked terminal
 * and is a harmless no-op (nil) when headless or piped. It NEVER changes
 * termios itself; the reader process owns that. Used by the reader's
 * interrupt-watch loop, which is active only during non-shell tool execution
 * (shell tools self-watch inside shell_managed), so stdin has a single
 * reader at any moment. */
static sw_val_t *_builtin_read_key(sw_val_t **a, int n) {
#ifdef _WIN32
    (void)a; (void)n;
    return sw_val_nil();
#else
    int64_t timeout_ms = (n >= 1 && a[0] && a[0]->type == SW_VAL_INT) ? a[0]->v.i : 0;
    if (!(isatty(STDIN_FILENO) && _sw_rl.saved_ok)) return sw_val_nil();
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0) return sw_val_nil();   /* timeout or error → no key */
    unsigned char b;
    ssize_t nb = read(STDIN_FILENO, &b, 1);
    if (nb != 1) return sw_val_nil();
    /* Bare Esc → 27 (a real interrupt). Arrow/F-key/paste escape sequences also
     * start with 0x1b but are drained by the shared helper and reported here as
     * "no key" (nil), so the interrupt-watcher never mistakes them for ESC. */
    if (b == 0x1b && !_sw_stdin_is_interrupt(STDIN_FILENO, b)) return sw_val_nil();
    return sw_val_int((int64_t)b);
#endif
}

/* exec_argv(cmd, args_list) → {exit_code, stdout}
 * Like shell() but forks+execs directly — no /bin/sh, no metacharacter risk.
 * cmd  : string — the executable (resolved via PATH by execvp)
 * args : sw list of strings — additional arguments (may be empty list)
 * Returns {exit_code, stdout_string} on success, nil on fork/pipe failure. */
static sw_val_t *_builtin_exec_argv(sw_val_t **a, int n) {
#ifdef _WIN32
    (void)a; (void)n;
    return sw_val_nil();
#else
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *cmd = a[0]->v.str;

    /* Collect extra args from the sw list (may be absent or empty). */
    int nlist = 0;
    sw_val_t *list = (n >= 2 && a[1] && a[1]->type == SW_VAL_LIST) ? a[1] : NULL;
    if (list) nlist = list->v.tuple.count;

    /* Build NULL-terminated argv: cmd + list_elements + NULL */
    char **argv = (char **)malloc(sizeof(char *) * (1 + nlist + 1));
    if (!argv) return sw_val_nil();
    argv[0] = (char *)cmd;
    for (int i = 0; i < nlist; i++) {
        sw_val_t *elem = list->v.tuple.items[i];
        /* Skip non-string elements silently (treat as empty string). */
        argv[1 + i] = (elem && elem->type == SW_VAL_STRING) ? elem->v.str : (char *)"";
    }
    argv[1 + nlist] = NULL;

    _sw_popen_pid_t pp = _sw_popen_argv(argv, NULL);
    free(argv); /* child has already exec'd; parent copy safe to free */
    if (!pp.fp) return sw_val_nil();

    /* Read all stdout into a growable buffer. */
    size_t cap = 65536, got = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { _sw_popen_pid_close(pp); return sw_val_nil(); }
    char chunk[8192];
    size_t rd;
    while ((rd = fread(chunk, 1, sizeof(chunk), pp.fp)) > 0) {
        if (got + rd + 1 > cap) {
            while (got + rd + 1 > cap) cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); _sw_popen_pid_close(pp); return sw_val_nil(); }
            buf = tmp;
        }
        memcpy(buf + got, chunk, rd);
        got += rd;
    }
    buf[got] = '\0';

    int wstatus = _sw_popen_pid_close(pp);
    int exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

    sw_val_t *items[2];
    items[0] = sw_val_int(exit_code);
    items[1] = sw_val_string(buf);
    free(buf);
    return sw_val_tuple(items, 2);
#endif
}

/* shell_detached(cmd, log_path, exit_path) → pid_int | nil
 *
 * Launch `cmd` FULLY detached from swarm-code: double-fork so the worker is
 * reparented to init/launchd (no zombies, survives our own exit), and
 * setsid() in the worker so it becomes a session + process-group leader
 * (pgid == worker pid). That pgid is what lets pid_kill_group(pid) tear down
 * the whole subtree via kill(-pid, ...). The worker's stdin is /dev/null (so a
 * command that reads stdin gets immediate EOF instead of hanging); stdout+stderr
 * are redirected to log_path (truncated); when the command finishes the wrapper
 * writes its exit code to exit_path.
 *
 * Returns the worker pid (int) so the caller can poll pid_alive / kill it, or
 * nil on any validation / pipe / fork failure. */
static sw_val_t *_builtin_shell_detached(sw_val_t **a, int n) {
#ifdef _WIN32
    (void)a; (void)n;
    return sw_val_nil();
#else
    if (n < 3 || !a[0] || a[0]->type != SW_VAL_STRING
              || !a[1] || a[1]->type != SW_VAL_STRING
              || !a[2] || a[2]->type != SW_VAL_STRING)
        return sw_val_nil();
    const char *cmd       = a[0]->v.str;
    const char *log_path  = a[1]->v.str;
    const char *exit_path = a[2]->v.str;

    int pfd[2];
    if (pipe(pfd) != 0) return sw_val_nil();

    pid_t inter = fork();
    if (inter < 0) { close(pfd[0]); close(pfd[1]); return sw_val_nil(); }

    if (inter == 0) {
        /* Intermediate child: close the read end, fork the worker, report the
         * worker pid up the pipe, then _exit(0). Its exit reparents the worker
         * to init/launchd so no zombie is left for swarm-code to reap. */
        close(pfd[0]);
        pid_t worker = fork();
        if (worker < 0) {                     /* worker fork failed */
            int neg = -1;
            (void)write(pfd[1], &neg, sizeof(neg));
            close(pfd[1]);
            _exit(0);
        }
        if (worker > 0) {                     /* still the intermediate */
            int wp = (int)worker;
            (void)write(pfd[1], &wp, sizeof(wp));
            close(pfd[1]);
            _exit(0);
        }

        /* Worker (grandchild). setsid() FIRST → new session, becomes its own
         * process-group leader (pgid == pid) so kill(-pid,...) hits the tree. */
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) { dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO); }
        close(pfd[1]);                         /* don't leak the pipe into cmd */
        if (devnull > STDERR_FILENO) close(devnull);
        if (logfd   > STDERR_FILENO) close(logfd);

        /* Wrapper: run cmd in a subshell, then record its exit code. */
        size_t wlen = strlen(cmd) + strlen(exit_path) + 32;
        char *wrapper = (char *)malloc(wlen);
        if (!wrapper) _exit(127);
        snprintf(wrapper, wlen, "( %s ); echo $? > %s", cmd, exit_path);
        execl("/bin/sh", "sh", "-c", wrapper, (char *)NULL);
        _exit(127);                            /* exec failed */
    }

    /* Parent: read the worker pid the intermediate wrote, reap the intermediate. */
    close(pfd[1]);
    int worker_pid = 0;
    ssize_t rd = read(pfd[0], &worker_pid, sizeof(worker_pid));
    close(pfd[0]);
    waitpid(inter, NULL, 0);                    /* reap the short-lived middle */
    if (rd != (ssize_t)sizeof(worker_pid) || worker_pid <= 0) return sw_val_nil();
    return sw_val_int(worker_pid);
#endif
}

/* pid_kill_group(pid) → 'true' | 'false'
 *
 * Tear down the entire process group led by `pid` (as established by
 * shell_detached's setsid): kill(-pid, SIGTERM) signals every process in the
 * group. REFUSES pid <= 1 (never kill(-1,...) — that would blast every process
 * we can signal — and never init). If the group send fails with ESRCH (no such
 * group / pid was never a leader), fall back to kill(pid, SIGTERM) for the lone
 * process. Returns 'true' if either signal was delivered, else 'false'. */
static sw_val_t *_builtin_pid_kill_group(sw_val_t **a, int n) {
#ifdef _WIN32
    (void)a; (void)n;
    return sw_val_atom("false");
#else
    if (n < 1 || !a[0]) return sw_val_atom("false");
    long pid = 0;
    if (a[0]->type == SW_VAL_INT) pid = (long)a[0]->v.i;
    else if (a[0]->type == SW_VAL_STRING) pid = atol(a[0]->v.str);
    else return sw_val_atom("false");
    if (pid <= 1) return sw_val_atom("false");   /* never kill(-1,...) or init */

    if (kill((pid_t)(-pid), SIGTERM) == 0) return sw_val_atom("true");
    if (errno == ESRCH && kill((pid_t)pid, SIGTERM) == 0) return sw_val_atom("true");
    return sw_val_atom("false");
#endif
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
            if (!**pp) break;  /* fuzz_json found: truncated escape at NUL -> OOB read */
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

/* Depth guard: nested arrays/objects recurse one C frame each on a 128KB fiber
 * stack — ~800 levels of adversarial JSON (an LLM response, an HTTP body) used
 * to SIGILL uncatchably. Count nesting at array/object entry; past the cap, set
 * a thread-local abort that the element loops check, so the parse unwinds to a
 * partial/nil value instead of crashing. */
#define SW_JSON_MAX_DEPTH 256
static __thread int g_json_depth = 0;
static __thread int g_json_abort = 0;

static sw_val_t *_json_parse_array(const char **pp) {
    if (g_json_depth >= SW_JSON_MAX_DEPTH) { g_json_abort = 1; return sw_val_nil(); }
    g_json_depth++;
    (*pp)++; /* skip [ */
    _json_skip_ws(pp);
    int cap = 64, cnt = 0;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    while (**pp && **pp != ']' && !g_json_abort) {
        items[cnt++] = _json_parse(pp);
        if (cnt >= cap) { cap *= 2; items = (sw_val_t **)realloc(items, sizeof(sw_val_t *) * cap); }
        _json_skip_ws(pp);
        if (**pp == ',') (*pp)++;
        _json_skip_ws(pp);
    }
    if (**pp == ']') (*pp)++;
    sw_val_t *r = sw_val_list(items, cnt);
    free(items);
    g_json_depth--;
    return r;
}

static sw_val_t *_json_parse_object(const char **pp) {
    if (g_json_depth >= SW_JSON_MAX_DEPTH) { g_json_abort = 1; return sw_val_nil(); }
    g_json_depth++;
    (*pp)++; /* skip { */
    _json_skip_ws(pp);
    int cap = 32, cnt = 0;
    sw_val_t **keys = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    sw_val_t **vals = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    while (**pp && **pp != '}' && !g_json_abort) {
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
    g_json_depth--;
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
    if (*pp == start) { if (**pp) (*pp)++; return sw_val_nil(); } /* fuzz_json: guard NUL before advance on unrecognized token */
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
    g_json_depth = 0; g_json_abort = 0;   /* reset the per-decode depth guard */
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

/* file_rename(src, dst)  'ok' | 'error' */
static sw_val_t *_builtin_file_rename(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("error");
    return sw_val_atom(rename(a[0]->v.str, a[1]->v.str) == 0 ? "ok" : "error");
}

/* file_stat(path)  %{size, mtime, mode, is_dir, exists} | nil */
static sw_val_t *_builtin_file_stat(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    struct stat st;
    if (stat(a[0]->v.str, &st) != 0) return sw_val_nil();
    sw_val_t *keys[5];
    sw_val_t *vals[5];
    keys[0] = sw_val_atom("size");    vals[0] = sw_val_int((int64_t)st.st_size);
#if defined(__APPLE__)
    keys[1] = sw_val_atom("mtime");   vals[1] = sw_val_int((int64_t)st.st_mtimespec.tv_sec);
#else
    keys[1] = sw_val_atom("mtime");   vals[1] = sw_val_int((int64_t)st.st_mtim.tv_sec);
#endif
    keys[2] = sw_val_atom("mode");    vals[2] = sw_val_int((int64_t)(st.st_mode & S_IFMT));
    keys[3] = sw_val_atom("is_dir");  vals[3] = sw_val_atom(S_ISDIR(st.st_mode) ? "true" : "false");
    keys[4] = sw_val_atom("exists");  vals[4] = sw_val_atom("true");
    return sw_val_map_new(keys, vals, 5);
}

/* file_atomic_write(path, content)  'ok' | 'error' */
static sw_val_t *_builtin_file_atomic_write(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("error");
    const char *path = a[0]->v.str;
    const char *content = a[1]->v.str;
    size_t plen = strlen(path);
    char *tmp = (char *)malloc(plen + 32);
    snprintf(tmp, plen + 32, "%s.tmp.%d", path, (int)getpid());
    FILE *fp = fopen(tmp, "w");
    if (!fp) { free(tmp); return sw_val_atom("error"); }
    size_t wlen = strlen(content);
    if (fwrite(content, 1, wlen, fp) != wlen) { fclose(fp); swbs_unlink(tmp); free(tmp); return sw_val_atom("error"); }
    if (fclose(fp) != 0) { swbs_unlink(tmp); free(tmp); return sw_val_atom("error"); }
    int rc = rename(tmp, path);
    free(tmp);
    return sw_val_atom(rc == 0 ? "ok" : "error");
}

/* file_temp(prefix)  unique tmp file path string */
static sw_val_t *_builtin_file_temp(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    const char *prefix = a[0]->v.str;
    size_t plen = strlen(prefix);
    char *tpl = (char *)malloc(plen + 16);
    snprintf(tpl, plen + 16, "%sXXXXXX", prefix);
    int fd = mkstemp(tpl);
    if (fd < 0) { free(tpl); return sw_val_nil(); }
    close(fd);
    sw_val_t *r = sw_val_string(tpl);
    free(tpl);
    return r;
}

/* === Timers: after & every === */

typedef struct { sw_val_t *fn; uint64_t ms; } _timer_closure_t;

/* Free a timer closure (a deep_copy_global tree). Armed as the timer process's
 * on_destroy hook so process_destroy reclaims it on EVERY exit path — crucially
 * the kill-while-parked path: a timer parked in sw_receive_any that is killed is
 * torn down by the scheduler WITHOUT resuming the fiber (kill_flag is checked
 * before swap-in, swarmrt_native.c), so a fiber-tail free would never run. */
static void _free_timer_closure(void *raw) {
    _timer_closure_t *c = (_timer_closure_t *)raw;
    if (c) { _sw_free_global_val(c->fn); free(c); }
}

static void _after_entry(void *raw) {
    _timer_closure_t *c = (_timer_closure_t *)raw;
    /* The closure is freed in process_destroy via on_destroy = _free_timer_closure,
     * armed by sw_spawn_dtor BEFORE this fiber was runnable — so it's reclaimed on
     * EVERY exit: natural fire, cancel-while-parked (the scheduler tears a killed
     * parked fiber down without resuming it), kill-while-running, and even a
     * pre-trampoline kill (before this fn ever runs). Wait via the yielding,
     * kill-aware sw_receive_any (NOT raw usleep — blocks the thread). */
    uint64_t tag;
    void *m = sw_receive_any(c->ms, &tag);
    if (m) free(m);                       /* discard any spurious message */
    sw_process_t *self = sw_self();
    if (!(self && self->kill_flag))       /* fire only if not cancelled mid-wait */
        sw_val_apply(c->fn, NULL, 0);
}

/* delay(ms, fn) → pid — run fn once after ms milliseconds */
static sw_val_t *_builtin_delay(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT || !a[1]) return sw_val_nil();
    _timer_closure_t *c = (_timer_closure_t *)malloc(sizeof(_timer_closure_t));
    /* GC v1: the timer process fires fn after the caller may have exited —
     * deep-copy the closure to the global heap so it survives. */
    c->fn = sw_val_deep_copy_global(a[1]);
    c->ms = (uint64_t)a[0]->v.i;
    /* on_destroy armed pre-runnable -> reclaimed even on a pre-trampoline kill. */
    sw_process_t *p = sw_spawn_dtor(_after_entry, c, _free_timer_closure);
    if (!p) { _free_timer_closure(c); return sw_val_nil(); }  /* spawn failed: reclaim */
    return sw_val_pid(p);
}

static void _every_entry(void *raw) {
    _timer_closure_t *c = (_timer_closure_t *)raw;
    /* The closure is freed in process_destroy via on_destroy = _free_timer_closure,
     * armed by sw_spawn_dtor BEFORE this fiber was runnable (so a cancelled
     * interval — killed while parked, which the scheduler tears down WITHOUT
     * resuming this fiber — still reclaims it, as does a pre-trampoline kill).
     * Yielding, kill-aware loop — NOT raw usleep (blocks the scheduler thread). */
    for (;;) {
        uint64_t tag;
        void *m = sw_receive_any(c->ms, &tag);
        if (m) free(m);                        /* discard any spurious message */
        sw_process_t *self = sw_self();
        if (self && self->kill_flag) break;    /* cancelled during the wait */
        sw_val_apply(c->fn, NULL, 0);
        self = sw_self();
        if (self && self->kill_flag) break;    /* cancelled during/after the tick */
    }
}

/* interval(ms, fn) → pid — run fn repeatedly every ms milliseconds */
static sw_val_t *_builtin_interval(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT || !a[1]) return sw_val_nil();
    _timer_closure_t *c = (_timer_closure_t *)malloc(sizeof(_timer_closure_t));
    /* GC v1: the interval process re-applies fn forever, past the caller's
     * lifetime — deep-copy the closure to the global heap. */
    c->fn = sw_val_deep_copy_global(a[1]);
    c->ms = (uint64_t)a[0]->v.i;
    /* on_destroy armed pre-runnable -> reclaimed even on a pre-trampoline kill. */
    sw_process_t *p = sw_spawn_dtor(_every_entry, c, _free_timer_closure);
    if (!p) { _free_timer_closure(c); return sw_val_nil(); }  /* spawn failed: reclaim */
    return sw_val_pid(p);
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

/* is_list(x) → 'true' | 'false' — runtime type predicate. */
static sw_val_t *_builtin_is_list(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_atom("false");
    return sw_val_atom(a[0]->type == SW_VAL_LIST ? "true" : "false");
}
/* is_map(x) → 'true' | 'false' — runtime type predicate. */
static sw_val_t *_builtin_is_map(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_atom("false");
    return sw_val_atom(a[0]->type == SW_VAL_MAP ? "true" : "false");
}

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
        if (name_len == 0 || name_len > 256) { p = brace; continue; }
        char name_buf[257];
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

/* ws_request_headers(conn) → %{} — the request-header MAP (lowercased keys)
 * from the WS UPGRADE request. Lets a WS handler read the Origin /
 * Authorization / webhook-signature headers the socket was opened with.
 * Always a map (empty if none); never nil, so map_get is safe. */
static sw_val_t *_builtin_ws_request_headers(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT)
        return sw_val_map_new(NULL, NULL, 0);
    return sw_ws_request_headers((int)a[0]->v.i);
}

/* ws_request_path(conn) → string — the path+query from the WS UPGRADE
 * request (""=unknown). The same value arrives in {'ws_connect', conn, path};
 * this lets a re-homed per-connection process re-read it without plumbing. */
static sw_val_t *_builtin_ws_request_path(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT)
        return sw_val_string("");
    return sw_val_string(sw_ws_request_path((int)a[0]->v.i));
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
        sw_send_value(ctx->caller, SW_TAG_NONE, sw_val_tuple(items, 2));   /* GC v1: copy off worker arena */
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
            sw_send_value(ctx->caller, SW_TAG_NONE, sw_val_tuple(items, 2));   /* GC v1: copy off worker arena */
        }
    }

    sw_pclose(fp);

    /* Send completion message */
    {
        sw_val_t *items[2];
        items[0] = sw_val_atom("llm_done");
        items[1] = sw_val_string(full_text);
        sw_send_value(ctx->caller, SW_TAG_NONE, sw_val_tuple(items, 2));   /* GC v1: copy off worker arena */
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

/* ets_list(table_id) → list of {key, value} tuples in that table.
 * Matches docs/SW_LANGUAGE.md ("all {key, val} as a list") and the
 * interpreter path. Previously returned keys only, which silently
 * disagreed with both the docs and the interpreter. */
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
            /* Copy BOTH key and value OUT into the caller's arena (BEAM ETS
             * semantics), exactly as ets_get does — the table owns + frees its
             * stored copies, so a later writer (put-replace/delete/take) must
             * not be able to dangle a leaf of the list we return. Aliasing the
             * stored e->key/e->value here was a heap-use-after-free (the reader
             * holds a list whose leaves point into the table; a subsequent
             * ets_delete/replace frees them). Done under the rdlock — a writer
             * needs the wrlock, so the graph can't be freed mid-copy. */
            sw_val_t *pair[2] = {
                sw_val_deep_copy_local(e->key),
                sw_val_deep_copy_local(e->value)
            };
            items[cnt++] = sw_val_tuple(pair, 2);
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
    /* Unbuffer stdio stdin for the raw-mode session. The editor's
     * ESC-disambiguation below and every stream-interrupt watcher select(2)
     * on the FD — which cannot see bytes fgetc() already slurped into the
     * stdio buffer. Buffered, a paste whose ESC[200~ open marker arrived in
     * the same read burst as its body was misread as bare-Esc + literal
     * "[200~" text (only pastes big enough to overflow the stdio buffer got
     * recognised). Unbuffered, fgetc() reads one byte per call, so the
     * kernel queue and select() always agree. Only the TTY path is
     * affected — piped/test stdin never reaches this setup. */
    setvbuf(stdin, NULL, _IONBF, 0);
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

/* Return the length of `s` (raw bytes) with any INCOMPLETE trailing UTF-8
 * multibyte sequence trimmed off, so callers never insert a broken codepoint.
 * A complete codepoint (or plain ASCII) is kept; a lead byte still missing one
 * or more of its continuation bytes is dropped. */
static size_t _sw_utf8_trim_incomplete(const unsigned char *s, size_t n) {
    if (n == 0) return 0;
    size_t cont = 0;
    size_t i = n;
    while (i > 0 && (s[i - 1] & 0xC0) == 0x80) { i--; cont++; }  /* skip continuations */
    if (i == 0) return 0;   /* nothing but continuation bytes — a codepoint whose
                             * lead byte was already lost (ring drop-oldest split
                             * it). Unusable garbage: drop it all rather than
                             * seeding mojibake into the caller's buffer. */
    unsigned char lead = s[i - 1];
    size_t need;
    if (lead < 0x80)               need = 1;   /* ASCII */
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else                            need = 1;   /* invalid lead — treat as single */
    size_t have = 1 + cont;                     /* lead + continuations present */
    if (have >= need) return n;                 /* complete → keep everything */
    return i - 1;                               /* incomplete tail → drop from the lead */
}

/* Extract the parent directory of `path` into `out` (NUL-terminated). Returns 1
 * if a parent component exists (out set), 0 if `path` has no '/' (cwd-relative). */
static int _sw_parent_dir(const char *path, char *out, size_t outsz) {
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return 0;
    size_t n = (size_t)(slash - path);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, path, n);
    out[n] = '\0';
    return 1;
}

/* rl_history_load(path) → count | nil. Read the history file, keep the newest
 * 1000 lines, and push them (oldest-first) into the in-session history array so
 * up-arrow recall survives a restart. Returns the number of lines loaded (post
 * 1000-line cap), or nil if the file cannot be opened. */
static sw_val_t *_builtin_rl_history_load(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    FILE *fp = fopen(a[0]->v.str, "r");
    if (!fp) return sw_val_nil();
    /* Ring of the newest 1000 line pointers. */
    #define _SW_HIST_LOAD_CAP 1000
    char *lines[_SW_HIST_LOAD_CAP];
    int head = 0, cnt = 0;
    char *lb = NULL;
    size_t lcap = 0;
    ssize_t rd;
    while ((rd = getline(&lb, &lcap, fp)) != -1) {
        while (rd > 0 && (lb[rd - 1] == '\n' || lb[rd - 1] == '\r')) lb[--rd] = '\0';
        char *dup = strdup(lb);
        if (!dup) continue;
        if (cnt < _SW_HIST_LOAD_CAP) {
            lines[(head + cnt) % _SW_HIST_LOAD_CAP] = dup;
            cnt++;
        } else {
            free(lines[head]);              /* drop oldest, keep newest 1000 */
            lines[head] = dup;
            head = (head + 1) % _SW_HIST_LOAD_CAP;
        }
    }
    free(lb);
    fclose(fp);
    for (int i = 0; i < cnt; i++) {
        char *ln = lines[(head + i) % _SW_HIST_LOAD_CAP];
        _sw_rl_history_push(ln);
        free(ln);
    }
    #undef _SW_HIST_LOAD_CAP
    return sw_val_int(cnt);
}

/* rl_history_append(path, line) → 'true' | 'false'. Append one line (creating
 * the parent directory if missing). Blank lines and embedded newlines are
 * rejected. */
static sw_val_t *_builtin_rl_history_append(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_STRING
              || !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("false");
    const char *path = a[0]->v.str;
    const char *line = a[1]->v.str;
    if (!*line || strchr(line, '\n')) return sw_val_atom("false");
    char parent[1024];
    if (_sw_parent_dir(path, parent, sizeof(parent))) _mkdirp(parent);
    FILE *fp = fopen(path, "a");
    if (!fp) return sw_val_atom("false");
    fputs(line, fp);
    fputc('\n', fp);
    fclose(fp);
    return sw_val_atom("true");
}

/* Advance a physical (row,col) cursor by the display width of one byte
 * stream, honouring ANSI escape sequences (zero width — CSI, OSC, and
 * two-byte ESC forms), hard newlines, UTF-8 continuation bytes (zero
 * width — only lead bytes advance a column), tabs (next multiple of 8),
 * and soft-wrap at `term_w`. `*row`/`*col` are updated in place. Used by
 * the multi-line redraw to compute geometry identically on the wipe pass
 * and the reprint pass.
 *
 * The escape skip is a real state machine. The naive version ("after ESC,
 * skip until a byte in 0x40..0x7e") is WRONG for CSI: the '[' introducer
 * itself is 0x5b — inside that range — so it terminated the skip
 * immediately and every SGR parameter byte after it ("38;2;175;0;0m")
 * was counted as a visible column. That parked the caret 15 columns right
 * of the true spot for swarm-code's colored "  ❯ " prompt and made the
 * wrap math fire 15 columns early (the caret-offset / broken-input-box
 * bug). States: 0 = normal, 1 = just saw ESC, 2 = CSI body (ESC [ params
 * / intermediates 0x20..0x3f, final byte 0x40..0x7e), 3 = OSC body
 * (ESC ] ... BEL or ESC \), 4 = OSC saw ESC (ST pending). Any other
 * byte after a bare ESC is a two-byte sequence: skip it and resume. */
static void _sw_rl_advance(const char *s, size_t n, int term_w,
                           int *row, int *col) {
    int esc = 0;
    if (term_w < 1) term_w = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (esc == 1) {                              /* byte after ESC */
            if (c == '[')      esc = 2;              /* CSI opener */
            else if (c == ']') esc = 3;              /* OSC opener */
            else               esc = 0;              /* two-byte ESC seq */
            continue;
        }
        if (esc == 2) {                              /* CSI body */
            if (c >= 0x40 && c <= 0x7e) esc = 0;     /* final byte ends it */
            continue;
        }
        if (esc == 3) {                              /* OSC body */
            if (c == 0x07)      esc = 0;             /* BEL terminator */
            else if (c == 0x1b) esc = 4;             /* maybe ST (ESC \) */
            continue;
        }
        if (esc == 4) {                              /* OSC: ESC seen */
            esc = (c == '\\') ? 0 : 3;               /* ESC \ = ST */
            continue;
        }
        if (c == 0x1b) { esc = 1; continue; }        /* ESC — start of escape */
        if (c == '\n') { (*row)++; *col = 0; continue; }
        if (c == '\r') { *col = 0; continue; }
        if (c == '\t') {
            /* Real terminals clamp TAB at the right margin — they never
             * soft-wrap on it — so mirror that or the row math drifts. */
            int next = ((*col / 8) + 1) * 8;
            if (next > term_w - 1) next = term_w - 1;
            if (next > *col) *col = next;
            continue;
        }
        if (c >= 0x80 && c < 0xc0) continue;         /* UTF-8 continuation */
        /* A visible column. Soft-wrap when it would overflow the line. */
        if (*col >= term_w) { (*row)++; *col = 0; }
        (*col)++;
    }
}

/* Render prompt + buffer AND/OR measure their physical geometry in ONE walk
 * (F10b). `emit` nonzero → bytes are written to stdout exactly as measured;
 * emit zero → pure measurement. Because printing and row/col math are the
 * same loop, a wipe pass can never disagree with what a previous paint
 * actually drew. (That disagreement was the root cause of the paste-then-type
 * garbage: the old bracketed-paste echo printed continuation lines with its
 * own hardcoded 2-space indent and never updated the recorded geometry, so
 * the first keystroke after a multi-line paste wiped from the wrong origin
 * row and stacked the reprint over stale rows.)
 *
 * Geometry rules (via _sw_rl_advance): rows/cols are 0-based; origin is
 * col 0 of the prompt's first physical row; soft-wrap at term_w with
 * deferred (xenl-style) wrap; ANSI CSI sequences and UTF-8 continuation
 * bytes are zero-width. Hard line breaks in the buffer ('\n', lone '\r',
 * or a "\r\n" pair = ONE break) start a continuation line indented with
 * spaces to the prompt's end column — and that SAME indent is included in
 * the math. Non-printing C0/DEL bytes in the buffer (possible via paste)
 * are neither emitted nor counted. Outputs: caret (row,col) for byte
 * offset `cursor`, end (row,col) after the full buffer. */
static void _sw_rl_paint(const char *prompt, const char *buf, size_t len,
                         size_t cursor, int term_w, int emit,
                         int *caret_row, int *caret_col,
                         int *end_row, int *end_col) {
    if (term_w < 1) term_w = 1;
    int row = 0, col = 0;
    if (prompt) {
        if (emit) fputs(prompt, stdout);
        _sw_rl_advance(prompt, strlen(prompt), term_w, &row, &col);
    }
    /* Continuation lines align under the column where input starts (the
     * prompt's end column). Degenerate prompts wider than the terminal
     * fall back to col 0. */
    int indent = (col < term_w) ? col : 0;
    if (cursor > len) cursor = len;
    int crow = row, ccol = col;
    for (size_t i = 0; i < len; i++) {
        if (i == cursor) { crow = row; ccol = col; }
        char cb = buf[i];
        if (cb == '\r' && i + 1 < len && buf[i + 1] == '\n') continue; /* CRLF: one break, at the '\n' */
        if (cb == '\n' || cb == '\r') {
            if (emit) {
                fputc('\n', stdout);                   /* ONLCR is on → CR+LF */
                for (int k = 0; k < indent; k++) fputc(' ', stdout);
            }
            row++;
            col = indent;
            continue;
        }
        if (((unsigned char)cb < 0x20 && cb != '\t') || cb == 0x7f)
            continue;                                  /* invisible controls */
        if (emit) fputc(cb, stdout);
        _sw_rl_advance(&cb, 1, term_w, &row, &col);
    }
    if (cursor == len) { crow = row; ccol = col; }
    *caret_row = crow; *caret_col = ccol;
    *end_row = row;    *end_col = col;
}

/* Paint prompt + buffer starting at the CURRENT physical caret position
 * (which must be the render origin: col 0 of the render's top row), then
 * park the caret at its logical spot and record the geometry for the next
 * wipe. Shared tail of the FIRST paint at read_line entry (no wipe — the
 * caller's scrollback above the prompt must survive) and of every redraw.
 * Caller must hold _sw_term_lock. */
static void _sw_rl_paint_park_unlocked(const char *prompt, const char *buf,
                                       size_t len, size_t cursor) {
    int term_w = _sw_term_cols();   /* re-query TIOCGWINSZ every paint */
    int crow = 0, ccol = 0, erow = 0, ecol = 0;
    _sw_rl_paint(prompt, buf, len, cursor, term_w, 1, &crow, &ccol, &erow, &ecol);
    /* Move the physical caret from end-of-text back to the caret position. */
    if (erow > crow) printf("\x1b[%dA", erow - crow);
    fputc('\r', stdout);
    if (ccol > 0) printf("\x1b[%dC", ccol);
    /* Record geometry for the next writer's wipe pass. */
    _sw_rl.last_rows = erow + 1;
    _sw_rl.caret_row = crow;
    fflush(stdout);
}

/* Redraw the current buffer multi-line-correctly (F10).
 *
 * The old single-line `\r\x1b[K` only wiped ONE physical row, so when the
 * prompt+buffer wrapped (long input) or contained hard newlines (multi-line
 * paste) and any of the five concurrent writers fired a redraw, the stale
 * lower rows survived and the reprint stacked another copy underneath —
 * producing the ~14x input-doubling. We instead remember the physical-line
 * geometry of the previous render in `_sw_rl` and, on every redraw:
 *   1. move the caret to the top-left of the previous render
 *      (\r + cursor-up by the stored caret_row), then
 *   2. \x1b[J to erase from there to the end of the screen (ALL prior
 *      rows — so a SHRINKING buffer leaves no stale rows), then
 *   3. reprint prompt + buffer THROUGH THE SAME WALK that measures them
 *      (_sw_rl_paint) and reposition the caret to `cursor`, recording the
 *      new geometry for the NEXT writer.
 * The whole wipe+reprint runs under _sw_term_lock (held by the caller),
 * so concurrent writers serialise on one atomic redraw and never interleave.
 * `cursor` is the byte offset within buf where the caret should end up.
 * The _unlocked variant assumes the caller already holds _sw_term_lock. */
static void _sw_rl_redraw_unlocked(const char *prompt, const char *buf, size_t len, size_t cursor) {
    /* (1)+(2) Wipe the entire previous render. Return the caret to col 0
     * of the top physical row of the last render, then clear downward. */
    fputs("\r", stdout);
    if (_sw_rl.caret_row > 0) printf("\x1b[%dA", _sw_rl.caret_row);
    fputs("\x1b[J", stdout);
    /* (3) Reprint + re-park through the shared render walk. */
    _sw_rl_paint_park_unlocked(prompt, buf, len, cursor);
}

static void _sw_rl_redraw(const char *prompt, const char *buf, size_t len, size_t cursor) {
    pthread_mutex_lock(&_sw_term_lock);
    _sw_rl_redraw_unlocked(prompt, buf, len, cursor);
    pthread_mutex_unlock(&_sw_term_lock);
}

/* Erase the entire active input render (all physical rows of the previous
 * redraw) and leave the caret at col 0 of the top row, then reset the
 * geometry so the subsequent reprint starts from a clean slate. This is the
 * one entry point every EXTERNAL writer (print_above, shell progress, the
 * http-stream stall path) must use BEFORE printing its own "above" text and
 * then re-calling _sw_rl_redraw_unlocked — it replaces the old single-line
 * `\r\x1b[K`, which wiped only one row and let wrapped/multi-line input
 * stack duplicate copies (the ~14x doubling). Caller must hold _sw_term_lock. */
static void _sw_rl_wipe_unlocked(void) {
    fputs("\r", stdout);
    if (_sw_rl.caret_row > 0) printf("\x1b[%dA", _sw_rl.caret_row);
    fputs("\x1b[J", stdout);
    _sw_rl.last_rows = 0;
    _sw_rl.caret_row = 0;
}

/* Clear the published editor state — under the lock so a concurrent
 * print_above() can't dereference a buffer read_line is about to free. */
static void _sw_rl_done(void) {
    pthread_mutex_lock(&_sw_term_lock);
    _sw_rl.active = 0;
    _sw_rl.cur_buf = NULL;
    _sw_rl.cur_len = NULL;
    _sw_rl.cur_cursor = NULL;
    _sw_rl.last_rows = 0;
    _sw_rl.caret_row = 0;
    pthread_mutex_unlock(&_sw_term_lock);
}

static sw_val_t *_builtin_read_line(sw_val_t **a, int n) {
    const char *prompt = (n >= 1 && a[0] && a[0]->type == SW_VAL_STRING) ? a[0]->v.str : NULL;

    /* Fall back to canonical line read if not a TTY (piped input, tests). */
    if (!_sw_rl_setup()) {
        if (prompt) {
            fputs(prompt, stdout);
            fflush(stdout);
        }
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

    /* Publish editor state so print_above() can redraw around output. */
    pthread_mutex_lock(&_sw_term_lock);
    _sw_rl.cur_prompt = prompt;
    _sw_rl.cur_buf = &buf;
    _sw_rl.cur_len = &len;
    _sw_rl.cur_cursor = &cursor;
    _sw_rl.last_rows = 0;
    _sw_rl.caret_row = 0;
    _sw_rl.active = 1;
    /* First paint: emit the prompt through the SAME render walk every
     * subsequent redraw uses, so the recorded geometry (last_rows /
     * caret_row) is exact from the first byte — even for prompts that wrap
     * or contain newlines. No wipe here: the caller's own output above the
     * prompt must survive. Same critical section as the publish so a
     * concurrent print_above can never slip in between `active` flipping
     * on and the first geometry record (it would double-paint the prompt). */
    _sw_rl_paint_park_unlocked(prompt, buf, len, cursor);
    pthread_mutex_unlock(&_sw_term_lock);

    /* Seed the edit buffer with any pending type-ahead the user entered while
     * the previous stream/tool was running (captured into the pending-input
     * ring). Printable bytes — including multi-byte UTF-8 — are inserted at the
     * caret and echoed; control bytes (Esc, arrow/paste framing residue, CR/LF)
     * are dropped so we never seed a multi-line buffer. The ring holds RAW
     * bytes, so an incomplete trailing UTF-8 sequence is trimmed first to avoid
     * inserting a broken codepoint. */
    {
        size_t plen = 0;
        char *pend = _sw_pending_take(&plen);
        if (pend) {
            size_t vlen = _sw_utf8_trim_incomplete((const unsigned char *)pend, plen);
            int seeded = 0;
            for (size_t i = 0; i < vlen; i++) {
                unsigned char ch = (unsigned char)pend[i];
                if (ch < 0x20 || ch == 0x7f) continue;   /* drop control bytes */
                if (len + 2 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
                buf[len++] = (char)ch;
                seeded = 1;
            }
            buf[len] = '\0';
            cursor = len;
            free(pend);
            if (seeded) _sw_rl_redraw(prompt, buf, len, cursor);
        }
    }

    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            if (len == 0) { _sw_rl_done(); free(buf); return sw_val_nil(); }
            break;
        }
        /* Ctrl+D (EOT) on empty line = EOF; otherwise forward-delete. */
        if (c == 4) {
            if (len == 0) { _sw_rl_done(); free(buf); fputc('\n', stdout); fflush(stdout); return sw_val_nil(); }
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
            /* F10: the caret may be parked on a NON-bottom physical row of a
             * wrapped multi-row input (after a cursor-up / Left / Ctrl-A). A bare
             * '\n' from there moves down only one row, leaving the lower input
             * rows on screen for the reply to overwrite. Step DOWN to the bottom
             * row first, then emit a clean CR+LF so output starts at col 0 on a
             * fresh line below every input row. Held under the term lock so a
             * concurrent print_above can't interleave with the submit. */
            pthread_mutex_lock(&_sw_term_lock);
            int _rl_down = _sw_rl.last_rows - 1 - _sw_rl.caret_row;
            if (_rl_down > 0) printf("\x1b[%dB", _rl_down);
            fputc('\r', stdout);
            fputc('\n', stdout);
            fflush(stdout);
            pthread_mutex_unlock(&_sw_term_lock);
            break;
        }
        /* Backspace (DEL or BS). */
        if (c == 127 || c == 8) {
            if (len > 0 && cursor > 0) {
                if (has_newline || cursor == len) {
                    /* Append-mode backspace. A bare "\b \b" can't undo a line
                     * wrap and won't update the multi-line geometry (F10), so
                     * redraw through the geometry-tracking path instead. */
                    len--;
                    cursor = len;
                    buf[len] = '\0';
                    _sw_rl_redraw(prompt, buf, len, cursor);
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
            /* Right arrow. Redraw rather than emit a bare \x1b[C so the caret
             * crosses a wrap boundary correctly and caret_row stays exact for
             * a concurrent writer's wipe (F10). */
            if (c2 == 'C') {
                if (!has_newline && cursor < len) {
                    cursor++;
                    _sw_rl_redraw(prompt, buf, len, cursor);
                }
                continue;
            }
            /* Left arrow. Redraw rather than emit a bare \x1b[D (which can't
             * move up across a wrap) so caret_row stays exact (F10). */
            if (c2 == 'D') {
                if (!has_newline && cursor > 0) {
                    cursor--;
                    _sw_rl_redraw(prompt, buf, len, cursor);
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
                    /* Collect paste content verbatim until ESC [ 2 0 1 ~.
                     * Collection is SILENT — no per-byte echo. The old echo
                     * printed continuation lines with a hardcoded 2-space
                     * indent the redraw geometry never knew about, so the
                     * first keystroke after a multi-line paste wiped from
                     * the wrong origin row and interleaved the reprint with
                     * stale rows. One geometry-tracked redraw below paints
                     * the whole paste once the body is in (a paste arrives
                     * as one burst, so nothing visible is delayed). */
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
                        if (pc == '\n' || pc == '\r') has_newline = 1;
                        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                        buf[len++] = (char)pc;
                        buf[len] = '\0';
                    }
                    paste_done:
                    cursor = len;
                    _sw_rl_redraw(prompt, buf, len, cursor);
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
                /* Append at end. We could echo the single byte directly, but
                 * a bare fputc doesn't update the multi-line geometry (F10),
                 * so once prompt+buf wraps the stored caret_row goes stale and
                 * a concurrent writer's wipe would clear the wrong rows. Go
                 * through the geometry-tracking redraw so last_rows/caret_row
                 * stay exact even as input wraps onto new physical lines. */
                buf[len++] = (char)c;
                cursor = len;
                buf[len] = '\0';
                _sw_rl_redraw(prompt, buf, len, cursor);
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
    _sw_rl_done();
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

/* print_above(args...) → 'ok'
 * Print like print(), but cooperate with an active raw-mode read_line:
 * wipe the input line, print the text above it, then redraw the input
 * line (prompt + buffer + caret) below. Lets background processes log
 * to the terminal without clobbering what the user is mid-typing.
 * Falls back to a plain newline-terminated print when no line editor
 * is active. */
static sw_val_t *_builtin_print_above(sw_val_t **a, int n) {
    pthread_mutex_lock(&_sw_term_lock);
    int act = _sw_rl.active;
    if (act) _sw_rl_wipe_unlocked();      /* wipe ALL rows of the input render */
    for (int i = 0; i < n; i++) {
        if (i) fputc(' ', stdout);
        sw_val_print(a[i]);
    }
    fputc('\n', stdout);
    if (act && _sw_rl.cur_buf && *_sw_rl.cur_buf) {
        _sw_rl_redraw_unlocked(_sw_rl.cur_prompt,
                               *_sw_rl.cur_buf,
                               _sw_rl.cur_len ? *_sw_rl.cur_len : 0,
                               _sw_rl.cur_cursor ? *_sw_rl.cur_cursor : 0);
    } else {
        fflush(stdout);
    }
    pthread_mutex_unlock(&_sw_term_lock);
    return sw_val_atom("ok");
}

/* pid_alive(pid) → 'true' | 'false'
 * Check whether an OS process is still running. Uses kill(pid, 0) —
 * no signal sent; just returns 0 if the process exists, non-zero if
 * it doesn't (or we lack permission). Used by schedulers / supervisors
 * to back-pressure: don't spawn child N+1 if child N hasn't exited. */
static sw_val_t *_builtin_pid_alive(sw_val_t **a, int n) {
    if (n < 1 || !a[0]) return sw_val_atom("false");
    long pid = 0;
    if (a[0]->type == SW_VAL_INT) pid = (long)a[0]->v.i;
    else if (a[0]->type == SW_VAL_STRING) pid = atol(a[0]->v.str);
    else return sw_val_atom("false");
    if (pid <= 0) return sw_val_atom("false");
#ifndef _WIN32
    int r = kill((pid_t)pid, 0);
    if (r == 0) return sw_val_atom("true");
    /* EPERM = process exists but we can't signal it. Still alive. */
    if (errno == EPERM) return sw_val_atom("true");
    return sw_val_atom("false");
#else
    (void)pid;
    return sw_val_atom("false");
#endif
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
    int is_tls;          /* 1 = traffic goes through SSL */
#ifdef SWARMRT_TLS
    SSL     *ssl;
    SSL_CTX *ctx;
#endif
    /* --- async push delivery (wsc_set_handler) --------------------------
     * When a handler is registered, a dedicated reader process blocking-
     * recvs frames off this slot and sw_send_tagged's them to `handler`
     * as {'wsc_message', handle, data} / {'wsc_close', handle}, mirroring
     * the WS server's tagged-message model (src/swarmrt_http.c).
     *
     * Concurrency invariant: with a handler set, exactly ONE thread reads
     * (the reader, via SSL_read in _builtin_wsc_recv) and at most one writes
     * (wsc_send, via SSL_write). That one-reader/one-writer split is the
     * full-duplex pattern OpenSSL supports without locking. Callers must NOT
     * also call blocking wsc_recv on a handle that has a handler (that would
     * be a second reader) — wsc_set_handler owns the read path from then on. */
    sw_process_t   *handler;       /* owner pid to deliver frames to */
    int             reader_running;/* 1 once a reader process is spawned */
    sw_process_t   *reader_proc;   /* the dedicated reader process — the ONLY
                                    * caller allowed to drive _builtin_wsc_recv
                                    * once a handler is set. Lets the recv guard
                                    * below distinguish the reader's own loop
                                    * from a stray user wsc_recv (a 2nd reader,
                                    * which would race the read path). */
} _sw_wsc_slot_t;
static _sw_wsc_slot_t _sw_wsc[_SW_WSC_MAX] = {0};

static int _sw_wsc_alloc_slot(int fd) {
    for (int i = 0; i < _SW_WSC_MAX; i++) {
        if (!_sw_wsc[i].used) {
            _sw_wsc[i].fd = fd;
            _sw_wsc[i].used = 1;
            _sw_wsc[i].is_tls = 0;
#ifdef SWARMRT_TLS
            _sw_wsc[i].ssl = NULL;
            _sw_wsc[i].ctx = NULL;
#endif
            _sw_wsc[i].handler = NULL;
            _sw_wsc[i].reader_running = 0;
            _sw_wsc[i].reader_proc = NULL;
            return i;
        }
    }
    return -1;
}

static void _sw_wsc_free_slot(int handle) {
    if (handle >= 0 && handle < _SW_WSC_MAX) {
#ifdef SWARMRT_TLS
        if (_sw_wsc[handle].ssl) { SSL_shutdown(_sw_wsc[handle].ssl); SSL_free(_sw_wsc[handle].ssl); }
        if (_sw_wsc[handle].ctx) SSL_CTX_free(_sw_wsc[handle].ctx);
        _sw_wsc[handle].ssl = NULL;
        _sw_wsc[handle].ctx = NULL;
#endif
        _sw_wsc[handle].used = 0;
        _sw_wsc[handle].fd = -1;
        _sw_wsc[handle].is_tls = 0;
        _sw_wsc[handle].handler = NULL;
        _sw_wsc[handle].reader_running = 0;
        _sw_wsc[handle].reader_proc = NULL;
    }
}

/* Per-slot transport send/recv. The existing _sw_wsc_send_all /
 * _sw_wsc_recv_all take a bare fd (used during the plaintext handshake);
 * these slot-aware wrappers route through SSL once a slot is TLS. */
static int _sw_wsc_slot_send_all(int handle, const void *buf, size_t len);
static int _sw_wsc_slot_recv_all(int handle, void *buf, size_t len);

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

/* === Scheduler-yielding connect + handshake (in-VM loopback fix) =========
 *
 * The plaintext path of wsc_connect / wsc_connect_tls used to do a *blocking*
 * connect() + a blocking handshake recv() directly on the scheduler thread.
 * That freezes the thread until the network responds. In production the WS
 * server is remote, so the response always arrives and the freeze is invisible.
 * But when the server you're *serving* (http_listen) and the server you're
 * *dialing* live in the SAME VM, the dial's connect/handshake can starve the
 * in-VM server's bridge process that must send the 101 — a deadlock that bit
 * the voice-agent in-VM self-test (its bridge dials an in-VM OpenAI mock).
 * Repro: tests/sw/test_voice_wsc_invm_loopback.sw under SW_SCHEDULERS=1.
 *
 * Fix: drive connect() and the handshake recv() *non-blocking*, calling
 * sw_yield() between readiness polls so the scheduler can run the in-VM server
 * (and every other process) on this same OS thread. sw_yield() is a pure
 * cooperative yield — unlike sw_receive_any() it does NOT touch the process
 * mailbox, so a bridge process mid-call keeps its queued {'ws_message'} frames.
 * Blocking mode is restored before returning so wsc_send/wsc_recv behave
 * exactly as before. The TLS (wss) SSL_connect leg is unchanged — it is only
 * taken against a remote server, which never deadlocks. */

/* Toggle O_NONBLOCK on a fd. Returns 0 on success, -1 on error. */
static int _sw_fd_set_nonblock(int fd, int on) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (on) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl);
}

/* Yield to the scheduler if we're running inside a sw process, so the in-VM
 * server can make progress. Outside a process (no current proc) there is no
 * scheduler to yield to, so fall back to a tiny select() nap to avoid a 100%
 * busy-spin. */
static void _sw_wsc_cooperative_pause(int fd, int for_write) {
    if (sw_self()) {
        sw_yield();
        return;
    }
    fd_set s; FD_ZERO(&s); FD_SET(fd, &s);
    struct timeval tv = {0, 2000};  /* 2 ms */
    if (for_write) select(fd + 1, NULL, &s, NULL, &tv);
    else           select(fd + 1, &s, NULL, NULL, &tv);
}

/* Non-blocking connect that yields the scheduler while the TCP handshake is in
 * flight. fd must already be a fresh SOCK_STREAM socket. Returns 0 connected,
 * -1 on error/timeout. Leaves the fd in BLOCKING mode on return. */
static int _sw_wsc_connect_yielding(int fd, const struct sockaddr *addr,
                                    socklen_t alen, int deadline_ms) {
    if (_sw_fd_set_nonblock(fd, 1) < 0) {
        /* Can't go non-blocking — fall back to a plain blocking connect. */
        return connect(fd, addr, alen) < 0 ? -1 : 0;
    }
    int rc = connect(fd, addr, alen);
    if (rc == 0) { _sw_fd_set_nonblock(fd, 0); return 0; }  /* connected at once */
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        _sw_fd_set_nonblock(fd, 0);
        return -1;
    }
    uint64_t start = _sw_now_ms();
    for (;;) {
        fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
        struct timeval tv = {0, 0};                    /* poll, don't block */
        int sel = select(fd + 1, NULL, &wf, NULL, &tv);
        if (sel > 0 && FD_ISSET(fd, &wf)) {
            int err = 0; socklen_t el = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) {
                _sw_fd_set_nonblock(fd, 0);
                return -1;
            }
            _sw_fd_set_nonblock(fd, 0);
            return 0;                                  /* connected */
        }
        if ((int)(_sw_now_ms() - start) >= deadline_ms) {
            _sw_fd_set_nonblock(fd, 0);
            return -1;                                 /* timed out */
        }
        _sw_wsc_cooperative_pause(fd, 1);
    }
}

/* Read the HTTP upgrade response headers (until "\r\n\r\n") into resp, yielding
 * the scheduler between non-blocking reads. fd must be connected. Returns the
 * total bytes read (>0) on success, -1 on error/timeout/close. Leaves the fd in
 * BLOCKING mode on return. */
static int _sw_wsc_handshake_recv_yielding(int fd, char *resp, size_t cap,
                                           int deadline_ms) {
    if (_sw_fd_set_nonblock(fd, 1) < 0) {
        /* Fall back to the old blocking header read. */
        size_t rlen = 0;
        while (rlen < cap - 1) {
            ssize_t got = recv(fd, resp + rlen, cap - 1 - rlen, 0);
            if (got <= 0) return -1;
            rlen += (size_t)got; resp[rlen] = 0;
            if (strstr(resp, "\r\n\r\n")) return (int)rlen;
        }
        return -1;
    }
    size_t rlen = 0;
    uint64_t start = _sw_now_ms();
    while (rlen < cap - 1) {
        ssize_t got = recv(fd, resp + rlen, cap - 1 - rlen, 0);
        if (got > 0) {
            rlen += (size_t)got; resp[rlen] = 0;
            if (strstr(resp, "\r\n\r\n")) { _sw_fd_set_nonblock(fd, 0); return (int)rlen; }
            continue;
        }
        if (got == 0) { _sw_fd_set_nonblock(fd, 0); return -1; }  /* peer closed */
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) { _sw_fd_set_nonblock(fd, 0); return -1; }
        if ((int)(_sw_now_ms() - start) >= deadline_ms) { _sw_fd_set_nonblock(fd, 0); return -1; }
        _sw_wsc_cooperative_pause(fd, 0);
    }
    _sw_fd_set_nonblock(fd, 0);
    return -1;  /* response did not fit / no terminator */
}

/* Wait for `fd` to become readable, yielding the scheduler between polls so an
 * in-VM WS server (or any sibling process) can run on this OS thread — the same
 * cooperative model used for connect/handshake. Returns 1 readable, 0 timed out.
 *   timeout_ms <  0 : wait forever (the async reader's "block until a frame"),
 *                     still yielding so it never monopolizes the scheduler.
 *   timeout_ms >= 0 : bounded wait.
 * Uses select() with a zero timeout purely as a non-blocking readiness probe;
 * the actual waiting is the sw_yield() in _sw_wsc_cooperative_pause. */
static int _sw_wsc_wait_readable_yielding(int fd, int timeout_ms) {
    uint64_t start = _sw_now_ms();
    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        struct timeval tv = {0, 0};               /* poll, don't block */
        int rv = select(fd + 1, &rf, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(fd, &rf)) return 1;
        if (rv < 0 && errno == EINTR) continue;
        if (timeout_ms >= 0 && (int)(_sw_now_ms() - start) >= timeout_ms) return 0;
        _sw_wsc_cooperative_pause(fd, 0);
    }
}

/* Slot-aware transport. Routes through SSL_write/SSL_read when the slot
 * is TLS, else falls back to the bare-fd loops above. */
static int _sw_wsc_slot_send_all(int handle, const void *buf, size_t len) {
    if (handle < 0 || handle >= _SW_WSC_MAX || !_sw_wsc[handle].used) return -1;
#ifdef SWARMRT_TLS
    if (_sw_wsc[handle].is_tls && _sw_wsc[handle].ssl) {
        const char *p = (const char *)buf;
        size_t off = 0;
        while (off < len) {
            int n = SSL_write(_sw_wsc[handle].ssl, p + off, (int)(len - off));
            if (n <= 0) {
                int e = SSL_get_error(_sw_wsc[handle].ssl, n);
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) continue;
                return -1;
            }
            off += (size_t)n;
        }
        return 0;
    }
#endif
    return _sw_wsc_send_all(_sw_wsc[handle].fd, buf, len);
}

static int _sw_wsc_slot_recv_all(int handle, void *buf, size_t len) {
    if (handle < 0 || handle >= _SW_WSC_MAX || !_sw_wsc[handle].used) return -1;
#ifdef SWARMRT_TLS
    if (_sw_wsc[handle].is_tls && _sw_wsc[handle].ssl) {
        char *p = (char *)buf;
        size_t off = 0;
        while (off < len) {
            int n = SSL_read(_sw_wsc[handle].ssl, p + off, (int)(len - off));
            if (n <= 0) {
                int e = SSL_get_error(_sw_wsc[handle].ssl, n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
                return -1;
            }
            off += (size_t)n;
        }
        return 0;
    }
#endif
    return _sw_wsc_recv_all(_sw_wsc[handle].fd, buf, len);
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
    /* Scheduler-yielding connect: lets an in-VM WS server make progress while
     * we wait for the TCP handshake (see the helper's comment). */
    if (_sw_wsc_connect_yielding(fd, res->ai_addr, res->ai_addrlen, 30000) < 0) {
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

    /* Read response headers until "\r\n\r\n", yielding the scheduler between
     * non-blocking reads so an in-VM server can answer. 4KB cap is plenty. */
    char resp[4096] = {0};
    if (_sw_wsc_handshake_recv_yielding(fd, resp, sizeof(resp), 30000) < 0) {
        close(fd); return sw_val_nil();
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
    if (_sw_wsc_slot_send_all(handle, hdr, (size_t)hlen) < 0) return sw_val_atom("error");

    /* Mask payload in 4KB chunks. */
    char chunk[4096];
    size_t off = 0;
    while (off < len) {
        size_t cn = len - off;
        if (cn > sizeof(chunk)) cn = sizeof(chunk);
        for (size_t i = 0; i < cn; i++) chunk[i] = text[off + i] ^ mask[(off + i) & 3];
        if (_sw_wsc_slot_send_all(handle, chunk, cn) < 0) return sw_val_atom("error");
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

    /* One-reader guard: if this handle has an async handler, its dedicated
     * reader process owns the read path. A blocking wsc_recv from any OTHER
     * process would be a second concurrent SSL_read on the same connection
     * (the documented UB). Refuse it: return nil + a stderr note, leaving the
     * reader's read path intact. The reader itself (reader_proc) is allowed
     * through — that's the loop in _sw_wsc_reader_entry. */
    if (_sw_wsc[handle].reader_running &&
        sw_self() != _sw_wsc[handle].reader_proc) {
        fprintf(stderr,
            "[swarmrt] wsc_recv(handle=%d): this handle has an async handler "
            "(wsc_set_handler) — its reader owns the read path. Don't also "
            "call wsc_recv on it (that races the reader). Receive the "
            "{'wsc_message', %d, data} messages in your mailbox instead.\n",
            handle, handle);
        fflush(stderr);
        return sw_val_nil();
    }
    int fd = _sw_wsc[handle].fd;

    int timeout_ms = -1;  /* default: block forever (the async reader's mode) */
    if (n >= 2 && a[1] && a[1]->type == SW_VAL_INT) timeout_ms = (int)a[1]->v.i;

    /* Wait for fd to be readable, YIELDING the scheduler between polls so an
     * in-VM WS server can produce the very frame we're waiting for (the same
     * in-VM-loopback fix connect/handshake got). A blocking select() here froze
     * the scheduler thread, starving the in-VM peer under a single scheduler.
     *
     * With TLS, SSL_read may have buffered record data the kernel select()
     * can't see; if so, skip the wait. The forever case (timeout_ms < 0, used
     * by the wsc_set_handler reader) also yields, so it never monopolizes the
     * scheduler. */
    {
        int have_buffered = 0;
#ifdef SWARMRT_TLS
        if (_sw_wsc[handle].is_tls && _sw_wsc[handle].ssl &&
            SSL_pending(_sw_wsc[handle].ssl) > 0) have_buffered = 1;
#endif
        if (!have_buffered) {
            if (_sw_wsc_wait_readable_yielding(fd, timeout_ms) == 0) return sw_val_nil();
        }
    }

    /* Read frame header: opcode/flags + payload-len byte. */
    uint8_t hdr[2];
    if (_sw_wsc_slot_recv_all(handle, hdr, 2) < 0) return sw_val_nil();
    int opcode = hdr[0] & 0x0f;
    int masked = hdr[1] & 0x80;
    uint64_t plen = hdr[1] & 0x7f;
    if (plen == 126) {
        uint8_t ext[2];
        if (_sw_wsc_slot_recv_all(handle, ext, 2) < 0) return sw_val_nil();
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (_sw_wsc_slot_recv_all(handle, ext, 8) < 0) return sw_val_nil();
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }
    uint8_t mask[4] = {0};
    if (masked) {
        if (_sw_wsc_slot_recv_all(handle, mask, 4) < 0) return sw_val_nil();
    }
    char *payload = (char *)malloc((size_t)plen + 1);
    if (!payload) return sw_val_nil();
    if (_sw_wsc_slot_recv_all(handle, payload, (size_t)plen) < 0) { free(payload); return sw_val_nil(); }
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
        _sw_wsc_slot_send_all(handle, ph, 2);
        uint8_t pmask[4] = {0,0,0,0};
        _sw_wsc_slot_send_all(handle, pmask, 4);
        if (plen > 0 && plen < 126) _sw_wsc_slot_send_all(handle, payload, (size_t)plen);
        free(payload);
        /* Recurse to get the next real frame. */
        return _builtin_wsc_recv(a, n);
    }
    if (opcode == 0x2) {
        /* Binary frame → return its raw bytes as a base64 string so sw
         * can carry NUL-bearing audio without a binary value type. The
         * tag mirrors the WS-server {ws_binary,...} convention. */
        char *b64 = _sw_audio_b64_encode((const uint8_t *)payload, (size_t)plen);
        free(payload);
        if (!b64) return sw_val_nil();
        sw_val_t *r = sw_val_string(b64);
        free(b64);
        return r;
    }
    if (opcode != 0x1 && opcode != 0x0) {
        /* Unknown control frame — return nil. */
        free(payload);
        return sw_val_nil();
    }

    sw_val_t *r = sw_val_string(payload);
    free(payload);
    return r;
}

/* === Async push delivery: wsc_set_handler ============================
 *
 * The blocking wsc_recv above is great for request/response (CDP), but a
 * voice bridge needs to `receive` on BOTH its Telnyx WS-server mailbox AND
 * the OpenAI wsc connection in ONE selective-receive loop — without a poll
 * tick. wsc_set_handler(handle, pid) registers `pid` as the owner and
 * spawns a dedicated reader process that blocking-recvs frames off the
 * connection and sw_send_tagged's each one to `pid`:
 *
 *     {'wsc_message', handle, data}   (text, or base64 for binary frames)
 *     {'wsc_close',   handle}         (once, when the peer/socket closes)
 *
 * This mirrors the WS server's tagged-message convention exactly, so a
 * call process can write a single `receive { {'ws_message',..}; {'wsc_message',..} }`
 * loop. The reader is one scheduler-managed sw process per wsc handle
 * (the documented scaling cost — fine for "one OpenAI socket per call").
 *
 * Lifetime / teardown: once a handler is set, the reader is the sole owner
 * of the read path AND of slot teardown. wsc_close() with a reader running
 * does NOT free the slot directly (that would race the reader's in-flight
 * recv); it shutdown(2)s the socket, which unblocks the reader's recv ->
 * recv returns nil -> reader emits {'wsc_close', handle}, frees the slot,
 * and exits. One reader + one wsc_send writer is the supported OpenSSL
 * full-duplex pattern (read on one thread, write on another). */

typedef struct { int handle; } _sw_wsc_reader_arg_t;

static void _sw_wsc_reader_entry(void *raw) {
    _sw_wsc_reader_arg_t *ra = (_sw_wsc_reader_arg_t *)raw;
    int handle = ra->handle;
    free(ra);

    for (;;) {
        if (handle < 0 || handle >= _SW_WSC_MAX || !_sw_wsc[handle].used) break;
        sw_process_t *owner = _sw_wsc[handle].handler;
        if (!owner) break;

        /* Blocking single-frame read (no timeout). Returns a string on a
         * text/binary frame, or nil on close/error. Reuses the tested
         * frame parser + ping/pong handling in _builtin_wsc_recv. */
        sw_val_t *harg[1]; harg[0] = sw_val_int(handle);
        sw_val_t *frame = _builtin_wsc_recv(harg, 1);

        if (frame && frame->type == SW_VAL_STRING) {
            sw_val_t *items[3];
            items[0] = sw_val_atom("wsc_message");
            items[1] = sw_val_int(handle);
            items[2] = frame;   /* the recv'd string value (already owned) */
            sw_send_value(owner, SW_TAG_NONE, sw_val_tuple(items, 3));   /* GC v1: copy off worker arena */
        } else {
            /* Close / error → notify once and tear the slot down ourselves. */
            sw_val_t *items[2];
            items[0] = sw_val_atom("wsc_close");
            items[1] = sw_val_int(handle);
            sw_send_value(owner, SW_TAG_NONE, sw_val_tuple(items, 2));   /* GC v1: copy off worker arena */
            if (handle >= 0 && handle < _SW_WSC_MAX && _sw_wsc[handle].used) {
                int fd = _sw_wsc[handle].fd;
                _sw_wsc_free_slot(handle);   /* frees SSL/CTX if TLS */
                if (fd >= 0) close(fd);
            }
            break;
        }
    }
}

/* wsc_set_handler(handle, pid) — deliver inbound frames to pid's mailbox.
 * Returns 'ok' on success, 'error' on a bad handle / missing reader. */
static sw_val_t *_builtin_wsc_set_handler(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    if (!a[1] || a[1]->type != SW_VAL_PID || !a[1]->v.pid) return sw_val_atom("error");
    int handle = (int)a[0]->v.i;
    if (handle < 0 || handle >= _SW_WSC_MAX || !_sw_wsc[handle].used) return sw_val_atom("error");

    /* One-reader guard: a handle gets EXACTLY one async reader for its
     * lifetime. A second wsc_set_handler would either spawn a second reader
     * (two threads racing SSL_read on one connection — the documented UB) or
     * silently re-point `handler` while the first reader keeps delivering to
     * the old owner. Refuse instead: return a clean sw-level 'error' + a
     * stderr note so the author sees the misuse rather than a heisenbug. */
    if (_sw_wsc[handle].reader_running) {
        fprintf(stderr,
            "[swarmrt] wsc_set_handler(handle=%d): a handler is already set "
            "on this handle; ignoring the second call. One wsc handle owns "
            "exactly one async reader — open a separate wsc_connect if you "
            "need another reader.\n", handle);
        fflush(stderr);
        return sw_val_atom("error");
    }

    _sw_wsc_reader_arg_t *ra = (_sw_wsc_reader_arg_t *)malloc(sizeof(*ra));
    if (!ra) return sw_val_atom("error");
    ra->handle = handle;
    sw_process_t *rp = sw_spawn(_sw_wsc_reader_entry, ra);
    if (!rp) { free(ra); return sw_val_atom("error"); }
    /* Record handler + reader BEFORE returning so the wsc_recv guard can
     * tell the reader's own loop from a stray user wsc_recv. */
    _sw_wsc[handle].handler = a[1]->v.pid;
    _sw_wsc[handle].reader_proc = rp;
    _sw_wsc[handle].reader_running = 1;
    return sw_val_atom("ok");
}

/* wsc_close(handle) — send close frame, close socket, free slot. */
static sw_val_t *_builtin_wsc_close(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_atom("error");
    int handle = (int)a[0]->v.i;
    if (handle < 0 || handle >= _SW_WSC_MAX || !_sw_wsc[handle].used) return sw_val_atom("error");
    int fd = _sw_wsc[handle].fd;
    /* Best-effort close frame. */
    uint8_t close_frame[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
    _sw_wsc_slot_send_all(handle, close_frame, sizeof(close_frame));

    /* If an async reader owns this slot, don't free it out from under the
     * reader's in-flight recv. shutdown(2) unblocks that recv -> the reader
     * emits {'wsc_close',handle}, frees the slot, and exits. We return now;
     * the reader does the teardown. */
    if (_sw_wsc[handle].reader_running) {
        shutdown(fd, SHUT_RDWR);
        return sw_val_atom("ok");
    }

    _sw_wsc_free_slot(handle);   /* frees SSL/CTX if TLS */
    close(fd);
    return sw_val_atom("ok");
}

/* wsc_connect_tls(url, headers_list) — like wsc_connect but:
 *   (a) accepts wss:// (TLS handshake; default port 443) as well as ws://,
 *   (b) injects each "Key: Value" string from headers_list into the
 *       WebSocket upgrade request (e.g. Authorization: Bearer ...).
 *
 * Returns an integer handle on success, nil on failure. On a build
 * without TLS (e.g. macOS without -DSWARMRT_TLS), a wss:// URL returns
 * nil with a one-line stderr note; ws:// still works fully. */
static sw_val_t *_builtin_wsc_connect_tls(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    const char *url = a[0]->v.str;

    int want_tls = 0;
    const char *p = NULL;
    int default_port = 80;
    if (strncmp(url, "wss://", 6) == 0) { want_tls = 1; p = url + 6; default_port = 443; }
    else if (strncmp(url, "ws://", 5) == 0) { want_tls = 0; p = url + 5; default_port = 80; }
    else return sw_val_nil();

#ifndef SWARMRT_TLS
    if (want_tls) {
        fprintf(stderr, "wsc_connect_tls: wss:// requires a TLS build "
                        "(rebuild with -DSWARMRT_TLS and link openssl); ws:// works.\n");
        return sw_val_nil();
    }
#endif

    char host[256] = {0};
    int port = default_port;
    char path[2048] = "/";

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
    hints.ai_family = AF_UNSPEC;       /* allow IPv6 too */
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return sw_val_nil();
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return sw_val_nil(); }
    /* Scheduler-yielding TCP connect (same in-VM-loopback fix as wsc_connect).
     * The TLS handshake below is unchanged; it's only reached for wss:// against
     * a remote server, which never deadlocks an in-VM listener. */
    if (_sw_wsc_connect_yielding(fd, res->ai_addr, res->ai_addrlen, 30000) < 0) {
        close(fd); freeaddrinfo(res); return sw_val_nil();
    }
    freeaddrinfo(res);

    /* Allocate a slot up front so the TLS objects have a home. */
    int handle = _sw_wsc_alloc_slot(fd);
    if (handle < 0) { close(fd); return sw_val_nil(); }

#ifdef SWARMRT_TLS
    if (want_tls) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { _sw_wsc_free_slot(handle); close(fd); return sw_val_nil(); }
        SSL_CTX_set_default_verify_paths(ctx); /* best-effort; we don't hard-fail verify for MVP */
        SSL *ssl = SSL_new(ctx);
        if (!ssl) { SSL_CTX_free(ctx); _sw_wsc_free_slot(handle); close(fd); return sw_val_nil(); }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host);   /* SNI — required at OpenAI's edge */
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl); SSL_CTX_free(ctx);
            _sw_wsc_free_slot(handle); close(fd); return sw_val_nil();
        }
        _sw_wsc[handle].ssl = ssl;
        _sw_wsc[handle].ctx = ctx;
        _sw_wsc[handle].is_tls = 1;
    }
#endif

    /* Sec-WebSocket-Key. */
    uint8_t key_raw[16];
    for (int i = 0; i < 16; i++) key_raw[i] = (uint8_t)(rand() & 0xff);
    char key_b64[32] = {0};
    _sw_b64_encode(key_raw, 16, key_b64);

    /* Build the handshake, appending caller headers before the final CRLF. */
    char req[8192];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n",
        path, host, port, key_b64);
    if (rl < 0 || rl >= (int)sizeof(req)) { _sw_wsc_free_slot(handle); close(fd); return sw_val_nil(); }

    if (n >= 2 && a[1] && a[1]->type == SW_VAL_LIST) {
        sw_val_t *lst = a[1];
        for (int i = 0; i < lst->v.tuple.count; i++) {
            sw_val_t *h = lst->v.tuple.items[i];
            if (!h || h->type != SW_VAL_STRING) continue;
            int w = snprintf(req + rl, sizeof(req) - rl, "%s\r\n", h->v.str);
            if (w < 0 || w >= (int)sizeof(req) - rl) { _sw_wsc_free_slot(handle); close(fd); return sw_val_nil(); }
            rl += w;
        }
    }
    /* Final blank line. */
    int w = snprintf(req + rl, sizeof(req) - rl, "\r\n");
    if (w < 0 || w >= (int)sizeof(req) - rl) { _sw_wsc_free_slot(handle); close(fd); return sw_val_nil(); }
    rl += w;

    if (_sw_wsc_slot_send_all(handle, req, (size_t)rl) < 0) {
        _sw_wsc_free_slot(handle); close(fd); return sw_val_nil();
    }

    /* Read response headers until "\r\n\r\n".
     *
     * Non-TLS slot (plaintext ws://, including the in-VM loopback mock): read
     * the headers through the scheduler-yielding bare-fd helper so an in-VM
     * server can answer — the same deadlock fix wsc_connect got.
     *
     * TLS slot (wss://): the bytes are inside the encrypted stream, so we must
     * go through SSL_read via the slot-aware byte reader. That path is only ever
     * taken against a remote server, which never deadlocks an in-VM listener, so
     * the existing blocking read is fine. */
    char resp[4096] = {0};
    int is_tls_slot = 0;
#ifdef SWARMRT_TLS
    is_tls_slot = _sw_wsc[handle].is_tls;
#endif
    if (!is_tls_slot) {
        if (_sw_wsc_handshake_recv_yielding(fd, resp, sizeof(resp), 30000) < 0) {
            _sw_wsc_free_slot(handle); close(fd); return sw_val_nil();
        }
    } else {
        size_t rlen = 0;
        while (rlen < sizeof(resp) - 1) {
            char one;
            if (_sw_wsc_slot_recv_all(handle, &one, 1) < 0) { _sw_wsc_free_slot(handle); close(fd); return sw_val_nil(); }
            resp[rlen++] = one;
            resp[rlen] = 0;
            if (rlen >= 4 && memcmp(resp + rlen - 4, "\r\n\r\n", 4) == 0) break;
        }
    }
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        _sw_wsc_free_slot(handle); close(fd); return sw_val_nil();
    }

    return sw_val_int(handle);
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

/* ================================================================
 * Audio codecs for native voice agents — G.711 mu-law / PCM16 /
 * resample. All are base64-in / base64-out so raw (NUL-bearing) PCM
 * never surfaces to sw (Option B from the voice plan: zero core change).
 * ================================================================ */

/* audio_ulaw_to_pcm16(b64_ulaw) → b64 little-endian PCM16, or nil. */
static sw_val_t *_builtin_audio_ulaw_to_pcm16(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    size_t inlen = 0;
    uint8_t *ulaw = _sw_audio_b64_decode(a[0]->v.str, &inlen);
    if (!ulaw) return sw_val_nil();
    size_t pcmlen = 0;
    uint8_t *pcm = _sw_ulaw_to_pcm16(ulaw, inlen, &pcmlen);
    free(ulaw);
    if (!pcm) return sw_val_nil();
    char *b64 = _sw_audio_b64_encode(pcm, pcmlen);
    free(pcm);
    if (!b64) return sw_val_nil();
    sw_val_t *r = sw_val_string(b64);
    free(b64);
    return r;
}

/* audio_pcm16_to_ulaw(b64_pcm16) → b64 mu-law, or nil. */
static sw_val_t *_builtin_audio_pcm16_to_ulaw(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    size_t inlen = 0;
    uint8_t *pcm = _sw_audio_b64_decode(a[0]->v.str, &inlen);
    if (!pcm) return sw_val_nil();
    size_t ulen = 0;
    uint8_t *ulaw = _sw_pcm16_to_ulaw(pcm, inlen, &ulen);
    free(pcm);
    if (!ulaw) return sw_val_nil();
    char *b64 = _sw_audio_b64_encode(ulaw, ulen);
    free(ulaw);
    if (!b64) return sw_val_nil();
    sw_val_t *r = sw_val_string(b64);
    free(b64);
    return r;
}

/* audio_resample(b64_pcm16, from_hz, to_hz) → b64 PCM16 at to_hz, or nil. */
static sw_val_t *_builtin_audio_resample(sw_val_t **a, int n) {
    if (n < 3 || !a[0] || a[0]->type != SW_VAL_STRING ||
        !a[1] || a[1]->type != SW_VAL_INT ||
        !a[2] || a[2]->type != SW_VAL_INT) return sw_val_nil();
    size_t inlen = 0;
    uint8_t *pcm = _sw_audio_b64_decode(a[0]->v.str, &inlen);
    if (!pcm) return sw_val_nil();
    size_t outlen = 0;
    uint8_t *out = _sw_pcm16_resample(pcm, inlen, (int)a[1]->v.i, (int)a[2]->v.i, &outlen);
    free(pcm);
    if (!out) return sw_val_nil();
    char *b64 = _sw_audio_b64_encode(out, outlen);
    free(out);
    if (!b64) return sw_val_nil();
    sw_val_t *r = sw_val_string(b64);
    free(b64);
    return r;
}

/* === ed25519_verify (TLS-gated, openssl EVP) =============================
 *
 * ed25519_verify(public_key, signature, message) -> 'true' | 'false' | nil
 *
 * Verifies an Ed25519 signature with OpenSSL's EVP one-shot verify
 * (EVP_PKEY ED25519 + EVP_DigestVerify, no separate digest — Ed25519 hashes
 * internally over SHA-512). Added for Telnyx webhook signature verification,
 * which the voice-agent port had to fail-closed-stub
 * (/Users/sky/voice-agent/src/Telnyx.sw): Telnyx signs each webhook Ed25519
 * over the bytes "{timestamp}|{raw_body}", delivering a base64 signature in
 * `telnyx-signature-ed25519` and a base64 raw-32-byte portal public key.
 *
 * Input contract (matches that real shape; both raw and base64 accepted):
 *   public_key : a SW_VAL_BYTES of exactly 32 raw bytes, OR a base64 STRING
 *                that decodes to 32 bytes (Telnyx's portal key — the path the
 *                webhook code takes).
 *   signature  : a SW_VAL_BYTES of exactly 64 raw bytes, OR a base64 STRING
 *                that decodes to 64 bytes (Telnyx's signature header).
 *   message    : a SW_VAL_BYTES (raw signed bytes), OR a STRING taken as its
 *                raw bytes verbatim — NOT base64-decoded. For Telnyx this is
 *                the literal "{timestamp}|{raw_body}" concatenation.
 *
 * Returns the atom 'true' on a valid signature, 'false' on an invalid one or
 * any malformed/wrong-length input (never crashes, fail-closed), and nil on a
 * build without TLS (openssl) — exactly mirroring wsc_connect_tls's wss:// case
 * (a one-line stderr note, additive, never breaks a non-TLS build).
 *
 * Escape hatch the port can still use without this builtin (documented in the
 * runbook): shell out to `openssl pkeyutl -verify -pubin -inkey key.pem -rawin
 * -in msg.bin -sigfile sig.bin`. */
#ifdef SWARMRT_TLS
/* Coerce a sw value to a malloc'd raw byte buffer: SW_VAL_BYTES -> copy;
 * SW_VAL_STRING + want_b64 -> base64-decode; SW_VAL_STRING + !want_b64 ->
 * the string's bytes verbatim (NUL-terminated content, len = strlen). Returns
 * NULL on type mismatch / decode failure. Caller frees. */
static uint8_t *_sw_ed_coerce_bytes(sw_val_t *v, int want_b64, size_t *out_len) {
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
            uint8_t *raw = _sw_audio_b64_decode(v->v.str, &n);  /* malloc'd */
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

static sw_val_t *_builtin_ed25519_verify(sw_val_t **a, int n) {
#ifndef SWARMRT_TLS
    (void)a; (void)n;
    fprintf(stderr, "ed25519_verify: requires a TLS build "
                    "(rebuild with -DSWARMRT_TLS and link openssl); "
                    "or shell out to `openssl pkeyutl -verify -rawin`.\n");
    return sw_val_nil();
#else
    if (n < 3) return sw_val_atom("false");
    size_t pk_len = 0, sig_len = 0, msg_len = 0;
    uint8_t *pk  = _sw_ed_coerce_bytes(a[0], 1, &pk_len);   /* key: base64 or raw 32 */
    uint8_t *sig = _sw_ed_coerce_bytes(a[1], 1, &sig_len);  /* sig: base64 or raw 64 */
    uint8_t *msg = _sw_ed_coerce_bytes(a[2], 0, &msg_len);  /* msg: verbatim bytes  */

    sw_val_t *result = sw_val_atom("false");
    /* Ed25519: public key is always 32 bytes, signature always 64. Anything
     * else is malformed input — fail closed, never hand it to OpenSSL. */
    if (pk && sig && msg && pk_len == 32 && sig_len == 64) {
        EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pk, pk_len);
        if (pkey) {
            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
            if (ctx) {
                /* Ed25519 uses a one-shot verify with a NULL digest. */
                if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1) {
                    int rc = EVP_DigestVerify(ctx, sig, sig_len, msg, msg_len);
                    if (rc == 1) result = sw_val_atom("true");
                    /* rc == 0 -> bad signature (false); rc < 0 -> error (false) */
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

/* === SW_VAL_BYTES builtins (length-carrying, NUL-safe byte vectors) ======
 *
 * These give sw a real binary value type. Unlike base64_decode (which returns
 * a NUL-terminated sw string and truncates binary data at the first 0x00),
 * bytes carry an explicit length and survive embedded NULs. */

/* bytes_from_base64(ascii_b64) → bytes | nil. The NUL-safe twin of
 * base64_decode: raw bytes are kept in a length-carrying value, not a
 * NUL-terminated string. */
static sw_val_t *_builtin_bytes_from_base64(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    size_t dlen = 0;
    uint8_t *raw = _sw_audio_b64_decode(a[0]->v.str, &dlen);
    if (!raw) return sw_val_nil();
    sw_val_t *r = sw_val_bytes(raw, dlen);
    free(raw);
    return r;
}

/* bytes_to_base64(bytes) → ascii_b64 string (always a valid sw string). */
static sw_val_t *_builtin_bytes_to_base64(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_BYTES) return sw_val_nil();
    char *b64 = _sw_audio_b64_encode(a[0]->v.bytes.data, a[0]->v.bytes.len);
    if (!b64) return sw_val_string("");
    sw_val_t *r = sw_val_string(b64);
    free(b64);
    return r;
}

/* byte_size(bytes) → int. Length in bytes (NOT string_length, which would
 * stop at the first NUL). Lenient: 0 for non-bytes. */
static sw_val_t *_builtin_byte_size(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_BYTES) return sw_val_int(0);
    return sw_val_int((int64_t)a[0]->v.bytes.len);
}

/* byte_at(bytes, i) → int 0..255. Panics out-of-range, like elem/hd. */
static sw_val_t *_builtin_byte_at(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_BYTES ||
        !a[1] || a[1]->type != SW_VAL_INT)
        _sw_runtime_panic("byte_at: expected (bytes, int)");
    int64_t i = a[1]->v.i;
    if (i < 0 || (size_t)i >= a[0]->v.bytes.len)
        _sw_runtime_panic("byte_at: index out of range (got %lld, size %zu)",
                          (long long)i, a[0]->v.bytes.len);
    return sw_val_int((int64_t)a[0]->v.bytes.data[i]);
}

/* byte_slice(bytes, start, len) → bytes. Clamps len to the end; returns
 * empty bytes if start is past the end or negative inputs are given. */
static sw_val_t *_builtin_byte_slice(sw_val_t **a, int n) {
    if (n < 3 || !a[0] || a[0]->type != SW_VAL_BYTES ||
        !a[1] || a[1]->type != SW_VAL_INT ||
        !a[2] || a[2]->type != SW_VAL_INT) return sw_val_nil();
    size_t total = a[0]->v.bytes.len;
    int64_t start = a[1]->v.i;
    int64_t want  = a[2]->v.i;
    if (start < 0 || want < 0 || (size_t)start >= total)
        return sw_val_bytes(NULL, 0);
    size_t avail = total - (size_t)start;
    size_t take  = ((size_t)want < avail) ? (size_t)want : avail;
    return sw_val_bytes(a[0]->v.bytes.data + start, take);
}

/* bytes_concat(a, b) → new bytes a ++ b. */
static sw_val_t *_builtin_bytes_concat(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_BYTES ||
        !a[1] || a[1]->type != SW_VAL_BYTES) return sw_val_nil();
    size_t la = a[0]->v.bytes.len, lb = a[1]->v.bytes.len;
    size_t tot = la + lb;
    uint8_t *tmp = (uint8_t *)malloc(tot ? tot : 1);
    if (la) memcpy(tmp, a[0]->v.bytes.data, la);
    if (lb) memcpy(tmp + la, a[1]->v.bytes.data, lb);
    sw_val_t *r = sw_val_bytes(tmp, tot);
    free(tmp);
    return r;
}

/* string_to_bytes(s) → bytes. The chars of s as raw bytes (stops at the sw
 * string's NUL — fine, the source is itself a NUL-terminated string). */
static sw_val_t *_builtin_string_to_bytes(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_nil();
    return sw_val_bytes((const uint8_t *)a[0]->v.str, strlen(a[0]->v.str));
}

/* UTF-8 sequence length from the lead byte (1–4); 1 for invalid leads so a
 * malformed byte degrades to one char instead of desyncing the walk. */
static int _sw_utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* string_chars(s) → list of single-CODEPOINT strings. The codepoint-aware
 * complement to byte-oriented string_length/string_sub: slicing UTF-8 text
 * by bytes corrupts multi-byte runes (Round-7 audit O7), and agent code
 * handles non-ASCII text constantly. length(string_chars(s)) is the
 * codepoint length; reassemble slices with Std.join(chars, ""). Walks raw
 * UTF-8 sequence lengths (no grapheme clustering — combining marks stay
 * separate codepoints, documented). */
static sw_val_t *_builtin_string_chars(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_STRING) return sw_val_list(NULL, 0);
    const char *s = a[0]->v.str ? a[0]->v.str : "";
    size_t len = strlen(s);
    int cap = (int)len + 1;
    sw_val_t **items = (sw_val_t **)malloc(sizeof(sw_val_t *) * cap);
    int cnt = 0;
    size_t i = 0;
    while (i < len) {
        int sl = _sw_utf8_seq_len((unsigned char)s[i]);
        if (i + (size_t)sl > len) sl = 1;   /* truncated tail sequence */
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

/* bytes_to_string(b) → string. Renders bytes as a string; truncates at the
 * first embedded NUL BY DESIGN (explicit, opt-in — call only when the data
 * is known to be text). NUL-free input round-trips losslessly. */
static sw_val_t *_builtin_bytes_to_string(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_BYTES) return sw_val_nil();
    size_t len = a[0]->v.bytes.len;
    char *tmp = (char *)malloc(len + 1);
    if (len) memcpy(tmp, a[0]->v.bytes.data, len);
    tmp[len] = 0;
    sw_val_t *r = sw_val_string(tmp);   /* sw_val_string strdups → stops at NUL */
    free(tmp);
    return r;
}

/* bytes_from_ints([n0, n1, ...]) → bytes. Direct byte construction from a
 * list of ints; each is masked to 0..255. The clean replacement for the
 * base64-identity hack. nil/empty list → empty bytes. */
static sw_val_t *_builtin_bytes_from_ints(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_LIST) return sw_val_bytes(NULL, 0);
    int cnt = a[0]->v.tuple.count;
    uint8_t *buf = (uint8_t *)malloc(cnt ? (size_t)cnt : 1);
    for (int i = 0; i < cnt; i++) {
        sw_val_t *e = a[0]->v.tuple.items[i];
        buf[i] = (e && e->type == SW_VAL_INT) ? (uint8_t)(e->v.i & 0xFF) : 0;
    }
    sw_val_t *r = sw_val_bytes(buf, (size_t)cnt);
    free(buf);
    return r;
}

/* byte(n) → bytes of length 1 holding n & 0xFF. Convenience for building
 * one-byte protocol frames without bytes_from_ints([n]). */
static sw_val_t *_builtin_byte(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_INT) return sw_val_bytes(NULL, 0);
    uint8_t b = (uint8_t)(a[0]->v.i & 0xFF);
    return sw_val_bytes(&b, 1);
}

/* === Bytes-native audio codec twins (no base64 round-trip) ===============
 * Same _sw_* helpers as the string codecs, but bytes-in / bytes-out so a sw
 * program can slice / index / hash a PCM frame directly. The string audio_*
 * builtins stay the zero-copy fast lane for g711 passthrough. */

/* audio_ulaw_to_pcm16_b(ulaw_bytes) → pcm16_bytes | nil. */
static sw_val_t *_builtin_audio_ulaw_to_pcm16_b(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_BYTES) return sw_val_nil();
    size_t pcmlen = 0;
    uint8_t *pcm = _sw_ulaw_to_pcm16(a[0]->v.bytes.data, a[0]->v.bytes.len, &pcmlen);
    if (!pcm) return sw_val_nil();
    sw_val_t *r = sw_val_bytes(pcm, pcmlen);
    free(pcm);
    return r;
}

/* audio_pcm16_to_ulaw_b(pcm16_bytes) → ulaw_bytes | nil. */
static sw_val_t *_builtin_audio_pcm16_to_ulaw_b(sw_val_t **a, int n) {
    if (n < 1 || !a[0] || a[0]->type != SW_VAL_BYTES) return sw_val_nil();
    size_t ulen = 0;
    uint8_t *ulaw = _sw_pcm16_to_ulaw(a[0]->v.bytes.data, a[0]->v.bytes.len, &ulen);
    if (!ulaw) return sw_val_nil();
    sw_val_t *r = sw_val_bytes(ulaw, ulen);
    free(ulaw);
    return r;
}

/* audio_resample_b(pcm16_bytes, from_hz, to_hz) → pcm16_bytes | nil. */
static sw_val_t *_builtin_audio_resample_b(sw_val_t **a, int n) {
    if (n < 3 || !a[0] || a[0]->type != SW_VAL_BYTES ||
        !a[1] || a[1]->type != SW_VAL_INT ||
        !a[2] || a[2]->type != SW_VAL_INT) return sw_val_nil();
    size_t outlen = 0;
    uint8_t *out = _sw_pcm16_resample(a[0]->v.bytes.data, a[0]->v.bytes.len,
                                      (int)a[1]->v.i, (int)a[2]->v.i, &outlen);
    if (!out) return sw_val_nil();
    sw_val_t *r = sw_val_bytes(out, outlen);
    free(out);
    return r;
}

/* ws_send_binary(conn, b64) → 'ok' | 'error' — decode base64 and send as
 * a WebSocket BINARY frame (server→client). For providers/protocols that
 * use opcode 0x2 rather than text. */
static sw_val_t *_builtin_ws_send_binary(sw_val_t **a, int n) {
    if (n < 2 || !a[0] || a[0]->type != SW_VAL_INT ||
        !a[1] || a[1]->type != SW_VAL_STRING) return sw_val_atom("error");
    int conn_id = (int)a[0]->v.i;
    size_t blen = 0;
    uint8_t *bytes = _sw_audio_b64_decode(a[1]->v.str, &blen);
    if (!bytes) return sw_val_atom("error");
    int rc = sw_ws_send_binary(conn_id, (const char *)bytes, (uint32_t)blen);
    free(bytes);
    return sw_val_atom(rc == 0 ? "ok" : "error");
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
    unsigned int buf = 0; int bits = 0;   /* buf unsigned: base64 sextet accumulator must not signed-shift (UB) */
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
