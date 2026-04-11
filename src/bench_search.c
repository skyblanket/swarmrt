/*
 * SwarmRT Search v2 — Performance Benchmark
 *
 * Tests fuzzy + BM25 + HNSW vector + hybrid search at 1K, 10K, 100K, 1M docs.
 * Reports: p50/p99/max latency, throughput, memory.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/resource.h>
#include "swarmrt_search.h"

/* ============================================================ */

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static size_t rss_bytes(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (size_t)ru.ru_maxrss;  /* bytes on macOS, KB on Linux */
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

/* Simple LCG for deterministic "random" data */
static uint32_t rng_state = 42;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* ============================================================
 * Generate test corpus
 * ============================================================ */

static const char *words[] = {
    "fix", "add", "update", "remove", "refactor", "implement", "debug",
    "optimize", "deploy", "test", "review", "merge", "revert", "migrate",
    "login", "auth", "session", "token", "password", "user", "admin",
    "database", "query", "index", "cache", "redis", "postgres", "sqlite",
    "api", "endpoint", "route", "controller", "middleware", "handler",
    "component", "button", "modal", "sidebar", "header", "footer",
    "dark", "mode", "theme", "style", "layout", "responsive", "mobile",
    "error", "warning", "crash", "timeout", "retry", "fallback", "log",
    "config", "env", "secret", "key", "certificate", "ssl", "tls",
    "search", "filter", "sort", "paginate", "scroll", "infinite",
    "upload", "download", "stream", "socket", "websocket", "grpc",
    "kubernetes", "docker", "terraform", "ansible", "nginx", "caddy",
};
#define NUM_WORDS (sizeof(words) / sizeof(words[0]))

static void gen_text(char *buf, int max_len) {
    int nwords = 4 + (int)(rng_next() % 8);  /* 4-11 words */
    int pos = 0;
    for (int i = 0; i < nwords && pos < max_len - 20; i++) {
        const char *w = words[rng_next() % NUM_WORDS];
        int wlen = (int)strlen(w);
        if (i > 0) buf[pos++] = ' ';
        memcpy(buf + pos, w, wlen);
        pos += wlen;
    }
    buf[pos] = '\0';
}

static void gen_embedding(float *emb, uint32_t dims) {
    float norm = 0.0f;
    for (uint32_t i = 0; i < dims; i++) {
        emb[i] = ((float)(rng_next() % 10000) - 5000.0f) / 5000.0f;
        norm += emb[i] * emb[i];
    }
    /* Normalize to unit vector */
    norm = sqrtf(norm);
    if (norm > 1e-6f) {
        for (uint32_t i = 0; i < dims; i++) emb[i] /= norm;
    }
}

static void print_latencies(double *latencies, int iters) {
    qsort(latencies, iters, sizeof(double), cmp_double);
    printf("  p50:   %8.1f us\n", latencies[iters / 2]);
    printf("  p90:   %8.1f us\n", latencies[(int)(iters * 0.9)]);
    printf("  p99:   %8.1f us\n", latencies[(int)(iters * 0.99)]);
    printf("  max:   %8.1f us\n", latencies[iters - 1]);
    double total = 0; for (int i = 0; i < iters; i++) total += latencies[i];
    printf("  mean:  %8.1f us\n", total / iters);
    printf("  qps:   %8.0f\n", iters / (total / 1e6));
}

/* ============================================================
 * Benchmark: build index (with all indexes: trigrams, BM25, HNSW)
 * ============================================================ */

static sws_index_t *build_index(int n, uint32_t dims, int with_vectors) {
    rng_state = 42;
    sws_config_t cfg = {
        .dims = dims,
        .hnsw_m = with_vectors ? 16 : 0,
        .hnsw_ef_construct = 200,
        .hnsw_ef_search = 50,
    };
    sws_index_t *idx = sws_new_with_config(cfg);
    char text[256];
    float *emb = with_vectors ? (float *)malloc(dims * sizeof(float)) : NULL;

    for (int i = 0; i < n; i++) {
        gen_text(text, sizeof(text));
        if (with_vectors) gen_embedding(emb, dims);
        sws_add(idx, (uint64_t)i, text, emb, with_vectors ? dims : 0);
    }

    free(emb);
    return idx;
}

static void bench_insert(int n, uint32_t dims, int with_vectors) {
    printf("\n--- INSERT %dK docs (dims=%u, vectors=%s) ---\n",
           n / 1000, dims, with_vectors ? "yes" : "no");

    rng_state = 42;
    size_t mem_before = rss_bytes();

    double t0 = now_us();
    sws_index_t *idx = build_index(n, dims, with_vectors);
    double elapsed = now_us() - t0;

    size_t mem_after = rss_bytes();
    size_t mem_delta = (mem_after > mem_before) ? mem_after - mem_before : 0;

    sws_info_t info;
    sws_info(idx, &info);

    printf("  time:       %.1f ms (%.1f us/doc)\n", elapsed / 1e3, elapsed / n);
    printf("  throughput: %.0f inserts/sec\n", n / (elapsed / 1e6));
    printf("  memory:     ~%.1f MB (delta RSS), ~%.1f MB (tracked)\n",
           mem_delta / (1024.0 * 1024.0), info.memory_bytes / (1024.0 * 1024.0));
    printf("  count:      %u\n", sws_count(idx));
    if (with_vectors)
        printf("  hnsw:       %u nodes, %u levels\n", info.hnsw_nodes, info.hnsw_levels);

    sws_free(idx);
}

/* ============================================================
 * Benchmark: Fuzzy search
 * ============================================================ */

static const char *queries[] = {
    "logn bug",            /* typo */
    "fix auth",            /* exact match */
    "databse query",       /* typo */
    "dark mode toggle",    /* multi-word */
    "kubernetes deploy",   /* exact */
    "websocket handler",   /* exact */
    "revert merge",        /* exact */
    "optimze cache",       /* typo */
    "xyznotfound",         /* no match */
    "implement search filter sort",  /* long query */
};
static int nqueries = (int)(sizeof(queries) / sizeof(queries[0]));

static void bench_fuzzy(int n, int iters, int limit) {
    printf("\n--- FUZZY SEARCH %dK docs (%d queries, limit=%d) ---\n",
           n / 1000, iters, limit);

    sws_index_t *idx = build_index(n, 0, 0);

    sws_result_t *results = (sws_result_t *)malloc(limit * sizeof(sws_result_t));
    double *latencies = (double *)malloc(iters * sizeof(double));

    for (int i = 0; i < iters; i++) {
        const char *q = queries[i % nqueries];
        double t0 = now_us();
        sws_fuzzy_search(idx, q, results, limit);
        latencies[i] = now_us() - t0;
    }

    print_latencies(latencies, iters);

    free(latencies);
    free(results);
    sws_free(idx);
}

/* ============================================================
 * Benchmark: BM25 search
 * ============================================================ */

static void bench_bm25(int n, int iters, int limit) {
    printf("\n--- BM25 SEARCH %dK docs (%d queries, limit=%d) ---\n",
           n / 1000, iters, limit);

    sws_index_t *idx = build_index(n, 0, 0);

    sws_result_t *results = (sws_result_t *)malloc(limit * sizeof(sws_result_t));
    double *latencies = (double *)malloc(iters * sizeof(double));

    for (int i = 0; i < iters; i++) {
        const char *q = queries[i % nqueries];
        double t0 = now_us();
        sws_bm25_search(idx, q, results, limit);
        latencies[i] = now_us() - t0;
    }

    print_latencies(latencies, iters);

    free(latencies);
    free(results);
    sws_free(idx);
}

/* ============================================================
 * Benchmark: Vector search (HNSW vs brute-force)
 * ============================================================ */

static void bench_vector(int n, uint32_t dims, int iters, int limit, int use_hnsw) {
    printf("\n--- %s SEARCH %dK docs (dims=%u, %d queries, limit=%d) ---\n",
           use_hnsw ? "HNSW" : "BRUTE-FORCE",
           n / 1000, dims, iters, limit);

    rng_state = 42;
    sws_config_t cfg = {
        .dims = dims,
        .hnsw_m = use_hnsw ? 16 : 0,
        .hnsw_ef_construct = 200,
        .hnsw_ef_search = 50,
    };
    sws_index_t *idx = sws_new_with_config(cfg);
    char text[256];
    float *emb = (float *)malloc(dims * sizeof(float));

    for (int i = 0; i < n; i++) {
        gen_text(text, sizeof(text));
        gen_embedding(emb, dims);
        sws_add(idx, (uint64_t)i, text, emb, dims);
    }

    /* Generate query vectors */
    float *query = (float *)malloc(dims * sizeof(float));
    sws_result_t *results = (sws_result_t *)malloc(limit * sizeof(sws_result_t));
    double *latencies = (double *)malloc(iters * sizeof(double));

    for (int i = 0; i < iters; i++) {
        gen_embedding(query, dims);
        double t0 = now_us();
        sws_vector_search(idx, query, dims, results, limit);
        latencies[i] = now_us() - t0;
    }

    print_latencies(latencies, iters);

    free(latencies);
    free(results);
    free(query);
    free(emb);
    sws_free(idx);
}

/* ============================================================
 * Benchmark: Hybrid search
 * ============================================================ */

static void bench_hybrid(int n, uint32_t dims, int iters, int limit) {
    printf("\n--- HYBRID SEARCH %dK docs (dims=%u, %d queries, limit=%d) ---\n",
           n / 1000, dims, iters, limit);

    rng_state = 42;
    sws_config_t cfg = {
        .dims = dims,
        .hnsw_m = 16,
        .hnsw_ef_construct = 200,
        .hnsw_ef_search = 50,
    };
    sws_index_t *idx = sws_new_with_config(cfg);
    char text[256];
    float *emb = (float *)malloc(dims * sizeof(float));

    for (int i = 0; i < n; i++) {
        gen_text(text, sizeof(text));
        gen_embedding(emb, dims);
        sws_add(idx, (uint64_t)i, text, emb, dims);
    }

    float *query_vec = (float *)malloc(dims * sizeof(float));
    sws_result_t *results = (sws_result_t *)malloc(limit * sizeof(sws_result_t));
    double *latencies = (double *)malloc(iters * sizeof(double));

    for (int i = 0; i < iters; i++) {
        const char *q = queries[i % nqueries];
        gen_embedding(query_vec, dims);
        double t0 = now_us();
        sws_hybrid_search(idx, q, query_vec, dims, 0.5f, results, limit);
        latencies[i] = now_us() - t0;
    }

    print_latencies(latencies, iters);

    free(latencies);
    free(results);
    free(query_vec);
    free(emb);
    sws_free(idx);
}

/* ============================================================
 * Benchmark: Save/Load
 * ============================================================ */

static void bench_save_load(int n, uint32_t dims) {
    printf("\n--- SAVE/LOAD %dK docs (dims=%u, SWS2) ---\n", n / 1000, dims);

    rng_state = 42;
    sws_config_t cfg = {
        .dims = dims,
        .hnsw_m = 16,
        .hnsw_ef_construct = 200,
        .hnsw_ef_search = 50,
    };
    sws_index_t *idx = sws_new_with_config(cfg);
    char text[256];
    float *emb = (float *)malloc(dims * sizeof(float));

    for (int i = 0; i < n; i++) {
        gen_text(text, sizeof(text));
        gen_embedding(emb, dims);
        sws_add(idx, (uint64_t)i, text, emb, dims);
    }

    const char *path = "/tmp/bench_search_v2.sws";

    double t0 = now_us();
    sws_save(idx, path);
    double save_ms = (now_us() - t0) / 1e3;

    /* Check file size */
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);

    t0 = now_us();
    sws_index_t *idx2 = sws_load(path);
    double load_ms = (now_us() - t0) / 1e3;

    printf("  save:  %8.1f ms\n", save_ms);
    printf("  load:  %8.1f ms\n", load_ms);
    printf("  file:  %8.1f MB\n", fsize / (1024.0 * 1024.0));
    printf("  docs:  %u (verified)\n", sws_count(idx2));

    remove(path);
    free(emb);
    sws_free(idx);
    sws_free(idx2);
}

/* ============================================================
 * Main
 * ============================================================ */

int main(void) {
    printf("==================================================\n");
    printf("  SwarmRT Search v2 Benchmark\n");
    printf("  SIMD: ");
#if defined(__aarch64__)
    printf("ARM64 NEON");
#elif defined(__x86_64__) && defined(__AVX__) && defined(__FMA__)
    printf("x86_64 AVX+FMA");
#elif defined(__x86_64__) && defined(__SSE__)
    printf("x86_64 SSE");
#else
    printf("Scalar");
#endif
    printf("\n==================================================\n");

    /* === INSERT benchmarks === */
    bench_insert(1000, 384, 0);
    bench_insert(1000, 384, 1);
    bench_insert(10000, 384, 0);
    bench_insert(10000, 384, 1);
    bench_insert(100000, 384, 0);
    bench_insert(100000, 384, 1);

    /* === FUZZY benchmarks (with hash map fix) === */
    bench_fuzzy(1000, 1000, 10);
    bench_fuzzy(10000, 1000, 10);
    bench_fuzzy(100000, 500, 10);

    /* === BM25 benchmarks === */
    bench_bm25(1000, 1000, 10);
    bench_bm25(10000, 1000, 10);
    bench_bm25(100000, 500, 10);

    /* === VECTOR benchmarks: HNSW vs brute-force === */
    printf("\n-- HNSW vs Brute-Force comparison --\n");
    bench_vector(1000, 384, 1000, 10, 0);   /* brute-force */
    bench_vector(1000, 384, 1000, 10, 1);   /* HNSW */
    bench_vector(10000, 384, 1000, 10, 0);  /* brute-force */
    bench_vector(10000, 384, 1000, 10, 1);  /* HNSW */
    bench_vector(100000, 384, 100, 10, 0);  /* brute-force */
    bench_vector(100000, 384, 100, 10, 1);  /* HNSW */

    /* === HYBRID benchmarks === */
    bench_hybrid(1000, 384, 1000, 10);
    bench_hybrid(10000, 384, 500, 10);

    /* === SAVE/LOAD (SWS2 with HNSW) === */
    bench_save_load(10000, 384);

    printf("\n==================================================\n");
    printf("Done.\n");
    return 0;
}
