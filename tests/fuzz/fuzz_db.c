/*
 * fuzz_db.c — fuzz the db builtins' SQLite argument path.
 *
 * Includes swarmrt_builtins_studio.h the same way generated code does, so
 * it drives the REAL _builtin_db_exec/_builtin_db_query/_sw_db_bind/
 * _sw_db_row_to_map implementations (they are static in the header). The
 * surface under test is OUR code around sqlite, not sqlite itself:
 *   - the 3-arg bind overload (arg-list walk, 1-based bind indexes),
 *   - _sw_db_bind's open_memstream fallback for non-scalar values,
 *   - _sw_db_row_to_map's column readback (TEXT copy, BLOB hex render
 *     with its sz*2+1 buffer, NULL/typeless columns),
 *   - the rows realloc-growth in db_query.
 *
 * Input layout: first NUL (or end) splits SQL from arg bytes; the arg
 * bytes are chopped into up to 4 bind values cycling through the sw_val
 * types (string / int / float / nil / tuple — tuple exercising the
 * memstream fallback). Every input runs against a fresh in-memory table
 * via db_exec (both forms) and db_query. Errors are fine; ASAN/UBSAN
 * crashes are the failure.
 *
 * The db handle is opened ONCE (:memory:, DEFENSIVE on) — per-input opens
 * would exhaust the slot table and fuzz slot bookkeeping instead of the
 * arg path. db_open/db_close still get one smoke pass at init.
 */
#include "swarmrt_native.h"
#include "swarmrt_lang.h"
#include "swarmrt_varena.h"
#include "swarmrt_ets.h"
#include "swarmrt_builtins_studio.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "fuzz_standalone.h"

static int g_db = -1;

static void db_fuzz_init(void) {
    if (g_db >= 0) return;
    /* Smoke the open/close arg checks once (bad args must not crash). */
    sw_val_t *bad = sw_val_int(12345);
    (void)_builtin_db_open(&bad, 1);
    (void)_builtin_db_close(&bad, 1);

    sw_val_t *mem = sw_val_string(":memory:");
    sw_val_t *h = _builtin_db_open(&mem, 1);
    g_db = (int)h->v.i;
    if (g_db < 0) abort();   /* can't fuzz without a handle */
    /* DEFENSIVE: mutated SQL can't ATTACH/write files or corrupt schema. */
    sqlite3_db_config(_sw_sqlite_db[g_db], SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    sw_val_t *args2[2] = {
        h, sw_val_string("CREATE TABLE t (id INTEGER, name TEXT, x REAL, b BLOB)")
    };
    (void)_builtin_db_exec(args2, 2);
    sw_val_t *seed[2] = {
        h, sw_val_string("INSERT INTO t VALUES (1, 'a', 1.5, x'00ff10')")
    };
    (void)_builtin_db_exec(seed, 2);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 8192) return 0;   /* SQL parse cost, not our surface */
    db_fuzz_init();

    /* Split input at the first NUL: SQL text | bind-arg bytes. */
    size_t sql_len = size;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == '\0') { sql_len = i; break; }
    }
    char *sql = (char *)malloc(sql_len + 1);
    if (!sql) return 0;
    memcpy(sql, data, sql_len);
    sql[sql_len] = '\0';

    const uint8_t *ab = data + (sql_len < size ? sql_len + 1 : size);
    size_t alen = (size_t)(data + size - ab);

    /* Build up to 4 bind args from the remaining bytes, cycling types so
     * every _sw_db_bind branch (incl. the memstream fallback) gets hit. */
    sw_val_t *args[4];
    int nargs = 0;
    while (nargs < 4 && alen > 0) {
        size_t chunk = alen < 5 ? alen : 5;
        switch (nargs % 4) {
        case 0: {   /* string of raw bytes (NUL-safe by construction) */
            char *s = (char *)malloc(chunk + 1);
            memcpy(s, ab, chunk);
            s[chunk] = '\0';
            args[nargs] = sw_val_string(s);
            free(s);
            break;
        }
        case 1: {   /* int from the bytes */
            int64_t v = 0;
            memcpy(&v, ab, chunk < 8 ? chunk : 8);
            args[nargs] = sw_val_int(v);
            break;
        }
        case 2:     /* nil → bind_null */
            args[nargs] = sw_val_nil();
            break;
        default: {  /* tuple → the open_memstream format fallback */
            sw_val_t *items[2] = { sw_val_int((int64_t)ab[0]),
                                   sw_val_atom("fuzz") };
            args[nargs] = sw_val_tuple(items, 2);
            break;
        }
        }
        nargs++;
        ab += chunk;
        alen -= chunk;
    }

    sw_val_t *h = sw_val_int(g_db);
    sw_val_t *sqlv = sw_val_string(sql);
    sw_val_t *arglist = sw_val_list(args, nargs);

    /* db_exec 2-arg (sqlite3_exec path). */
    sw_val_t *e2[2] = { h, sqlv };
    (void)_builtin_db_exec(e2, 2);

    /* db_exec 3-arg (prepare + bind + step). */
    sw_val_t *e3[3] = { h, sqlv, arglist };
    (void)_builtin_db_exec(e3, 3);

    /* db_query with binds (prepare + bind + row readback). */
    (void)_builtin_db_query(e3, 3);

    /* A query guaranteed to return rows, with fuzzed binds — drives
     * _sw_db_row_to_map over every column type incl. the BLOB hex path. */
    sw_val_t *sel = sw_val_string("SELECT id, name, x, b FROM t WHERE id != ?");
    sw_val_t *q2[3] = { h, sel, arglist };
    (void)_builtin_db_query(q2, 3);

    free(sql);
    /* sw_vals go to the global heap here (no fiber) — reclaimed at process
     * exit; the gate runs ASAN with detect_leaks=0. */
    return 0;
}
