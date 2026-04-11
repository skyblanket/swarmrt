/*
 * SwarmRT Search v2 - Standalone Test Binary
 *
 * Tests: trigram fuzzy search, BM25 full-text, HNSW vector search,
 *        hybrid search, SIMD cosine similarity, document add/remove,
 *        save/load roundtrip (SWS1 + SWS2), adaptive config.
 *
 * Usage: make test-search
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "swarmrt_search.h"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        passes++; \
    } \
} while (0)

static int passes = 0;
static int failures = 0;

/* === Test: Basic lifecycle === */
static void test_lifecycle(void) {
    printf("\n[test_lifecycle]\n");

    sws_index_t *idx = sws_new(384);
    ASSERT(idx != NULL, "sws_new returns non-NULL");
    ASSERT(sws_count(idx) == 0, "empty index has count 0");

    sws_free(idx);
    ASSERT(1, "sws_free does not crash");
}

/* === Test: Config lifecycle === */
static void test_config(void) {
    printf("\n[test_config]\n");

    sws_config_t cfg = {
        .dims = 128,
        .hnsw_m = 8,
        .hnsw_ef_construct = 100,
        .hnsw_ef_search = 20,
        .max_docs = 500,
    };
    sws_index_t *idx = sws_new_with_config(cfg);
    ASSERT(idx != NULL, "sws_new_with_config returns non-NULL");
    ASSERT(sws_count(idx) == 0, "config index has count 0");

    sws_info_t info;
    sws_info(idx, &info);
    ASSERT(info.dims == 128, "info reports correct dims");
    ASSERT(info.doc_count == 0, "info reports 0 docs");

    sws_free(idx);
    ASSERT(1, "config index freed without crash");
}

/* === Test: Add and count === */
static void test_add_count(void) {
    printf("\n[test_add_count]\n");

    sws_index_t *idx = sws_new(0);

    int rc = sws_add(idx, 1, "hello world", NULL, 0);
    ASSERT(rc == 0, "add doc 1 succeeds");
    ASSERT(sws_count(idx) == 1, "count is 1");

    rc = sws_add(idx, 2, "goodbye moon", NULL, 0);
    ASSERT(rc == 0, "add doc 2 succeeds");
    ASSERT(sws_count(idx) == 2, "count is 2");

    /* Duplicate ID should fail */
    rc = sws_add(idx, 1, "duplicate", NULL, 0);
    ASSERT(rc == -1, "duplicate ID rejected");
    ASSERT(sws_count(idx) == 2, "count still 2 after dup");

    sws_free(idx);
}

/* === Test: Remove === */
static void test_remove(void) {
    printf("\n[test_remove]\n");

    sws_index_t *idx = sws_new(0);
    sws_add(idx, 10, "document ten", NULL, 0);
    sws_add(idx, 20, "document twenty", NULL, 0);
    sws_add(idx, 30, "document thirty", NULL, 0);
    ASSERT(sws_count(idx) == 3, "3 docs added");

    int rc = sws_remove(idx, 20);
    ASSERT(rc == 0, "remove doc 20 succeeds");
    ASSERT(sws_count(idx) == 2, "count is 2 after remove");

    rc = sws_remove(idx, 99);
    ASSERT(rc == -1, "remove nonexistent fails");

    sws_free(idx);
}

/* === Test: Fuzzy search === */
static void test_fuzzy_search(void) {
    printf("\n[test_fuzzy_search]\n");

    sws_index_t *idx = sws_new(0);

    sws_add(idx, 1, "fix login bug in auth controller", NULL, 0);
    sws_add(idx, 2, "add dark mode toggle to settings", NULL, 0);
    sws_add(idx, 3, "refactor database connection pool", NULL, 0);
    sws_add(idx, 4, "update login page styling", NULL, 0);
    sws_add(idx, 5, "implement session logout endpoint", NULL, 0);

    sws_result_t results[5];
    int n = sws_fuzzy_search(idx, "logn bug", results, 5);

    ASSERT(n > 0, "fuzzy search returns results");
    ASSERT(results[0].id == 1 || results[0].id == 4, "top result is login-related");
    ASSERT(results[0].score > 0.0f, "top result has positive score");
    ASSERT(results[0].text != NULL, "result has text pointer");

    printf("  -- fuzzy results for 'logn bug':\n");
    for (int i = 0; i < n; i++) {
        printf("     %d. id=%llu score=%.4f text=\"%s\"\n",
               i + 1, (unsigned long long)results[i].id,
               results[i].score, results[i].text);
    }

    /* Empty query */
    n = sws_fuzzy_search(idx, "ab", results, 5);  /* Too short for trigrams */
    ASSERT(n == 0, "query shorter than 3 chars returns 0");

    sws_free(idx);
}

/* === Test: BM25 search === */
static void test_bm25_search(void) {
    printf("\n[test_bm25_search]\n");

    sws_index_t *idx = sws_new(0);

    sws_add(idx, 1, "fix login bug in auth controller", NULL, 0);
    sws_add(idx, 2, "add dark mode toggle to settings", NULL, 0);
    sws_add(idx, 3, "refactor database connection pool", NULL, 0);
    sws_add(idx, 4, "update login page styling and fix layout", NULL, 0);
    sws_add(idx, 5, "implement session logout endpoint", NULL, 0);

    sws_result_t results[5];
    int n = sws_bm25_search(idx, "login bug", results, 5);

    ASSERT(n > 0, "BM25 search returns results");
    ASSERT(results[0].score > 0.0f, "top result has positive score");
    /* Doc 1 has both "login" and "bug", should be top */
    ASSERT(results[0].id == 1, "top BM25 result is doc 1 (has both terms)");

    printf("  -- BM25 results for 'login bug':\n");
    for (int i = 0; i < n; i++) {
        printf("     %d. id=%llu score=%.4f text=\"%s\"\n",
               i + 1, (unsigned long long)results[i].id,
               results[i].score, results[i].text);
    }

    /* Multi-term query */
    n = sws_bm25_search(idx, "dark mode toggle settings", results, 5);
    ASSERT(n > 0, "BM25 multi-term returns results");
    ASSERT(results[0].id == 2, "top result is doc 2 (dark mode)");

    /* No match */
    n = sws_bm25_search(idx, "xyznotfound", results, 5);
    ASSERT(n == 0, "BM25 no-match returns 0");

    /* Single char query (< 2 chars per token) */
    n = sws_bm25_search(idx, "x", results, 5);
    ASSERT(n == 0, "BM25 single char returns 0");

    sws_free(idx);
}

/* === Test: BM25 with remove === */
static void test_bm25_remove(void) {
    printf("\n[test_bm25_remove]\n");

    sws_index_t *idx = sws_new(0);
    sws_add(idx, 1, "login auth fix", NULL, 0);
    sws_add(idx, 2, "login page update", NULL, 0);

    sws_result_t results[5];
    int n = sws_bm25_search(idx, "login", results, 5);
    ASSERT(n == 2, "BM25 finds 2 docs with 'login'");

    sws_remove(idx, 1);
    n = sws_bm25_search(idx, "login", results, 5);
    ASSERT(n == 1, "BM25 finds 1 doc after remove");
    ASSERT(results[0].id == 2, "remaining doc is id 2");

    sws_free(idx);
}

/* === Test: SIMD cosine similarity === */
static void test_cosine(void) {
    printf("\n[test_cosine]\n");

    /* Identical vectors -> 1.0 */
    float a[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    float sim = sws_cosine_similarity(a, a, 8);
    ASSERT(fabsf(sim - 1.0f) < 0.001f, "identical vectors -> ~1.0");

    /* Orthogonal vectors -> 0.0 */
    float b[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float c[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    sim = sws_cosine_similarity(b, c, 4);
    ASSERT(fabsf(sim) < 0.001f, "orthogonal vectors -> ~0.0");

    /* Opposite vectors -> -1.0 */
    float d[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float e[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
    sim = sws_cosine_similarity(d, e, 4);
    ASSERT(fabsf(sim + 1.0f) < 0.001f, "opposite vectors -> ~-1.0");

    /* 384-dim random (smoke test) */
    float v1[384], v2[384];
    for (int i = 0; i < 384; i++) {
        v1[i] = (float)(i % 17) * 0.1f;
        v2[i] = (float)((i + 7) % 13) * 0.15f;
    }
    sim = sws_cosine_similarity(v1, v2, 384);
    ASSERT(sim >= -1.0f && sim <= 1.0f, "384-dim cosine in [-1, 1]");
    printf("  -- 384-dim cosine: %.6f\n", sim);
}

/* === Test: Vector search (brute-force, dims=0 HNSW) === */
static void test_vector_search(void) {
    printf("\n[test_vector_search]\n");

    uint32_t dims = 16;
    /* Use config with m=0 to force brute-force */
    sws_config_t cfg = {.dims = dims, .hnsw_m = 0};
    sws_index_t *idx = sws_new_with_config(cfg);

    /* Create docs with embeddings */
    float emb1[16], emb2[16], emb3[16], query[16];
    for (uint32_t i = 0; i < dims; i++) {
        emb1[i] = (float)i * 0.1f;
        emb2[i] = (float)(dims - i) * 0.1f;
        emb3[i] = (float)i * 0.05f;  /* Similar to emb1 */
        query[i] = (float)i * 0.1f;  /* Identical to emb1 */
    }

    sws_add(idx, 1, "doc one", emb1, dims);
    sws_add(idx, 2, "doc two", emb2, dims);
    sws_add(idx, 3, "doc three", emb3, dims);

    sws_result_t results[3];
    int n = sws_vector_search(idx, query, dims, results, 3);

    ASSERT(n == 3, "vector search returns 3 results");
    ASSERT(results[0].id == 1, "top result is doc 1 (identical embedding)");
    ASSERT(fabsf(results[0].score - 1.0f) < 0.001f, "top score is ~1.0");

    printf("  -- vector search results:\n");
    for (int i = 0; i < n; i++) {
        printf("     %d. id=%llu score=%.6f\n",
               i + 1, (unsigned long long)results[i].id, results[i].score);
    }

    sws_free(idx);
}

/* === Test: HNSW vector search === */
static void test_hnsw_search(void) {
    printf("\n[test_hnsw_search]\n");

    uint32_t dims = 32;
    sws_config_t cfg = {
        .dims = dims,
        .hnsw_m = 8,
        .hnsw_ef_construct = 100,
        .hnsw_ef_search = 30,
    };
    sws_index_t *idx = sws_new_with_config(cfg);

    /* Add 100 docs with embeddings */
    float *emb = (float *)malloc(dims * sizeof(float));
    for (int i = 0; i < 100; i++) {
        for (uint32_t d = 0; d < dims; d++) {
            emb[d] = (float)((i * 31 + d * 7) % 100) * 0.01f;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "hnsw doc %d", i);
        sws_add(idx, (uint64_t)i, buf, emb, dims);
    }
    ASSERT(sws_count(idx) == 100, "100 HNSW docs added");

    /* Query for doc 42 */
    float *query = (float *)malloc(dims * sizeof(float));
    for (uint32_t d = 0; d < dims; d++) {
        query[d] = (float)((42 * 31 + d * 7) % 100) * 0.01f;
    }

    sws_result_t results[10];
    int n = sws_vector_search(idx, query, dims, results, 10);

    ASSERT(n > 0, "HNSW search returns results");
    ASSERT(results[0].id == 42, "HNSW top result is doc 42 (exact match)");
    ASSERT(fabsf(results[0].score - 1.0f) < 0.01f, "HNSW exact match score ~1.0");

    printf("  -- HNSW results for doc 42:\n");
    for (int i = 0; i < n && i < 5; i++) {
        printf("     %d. id=%llu score=%.6f\n",
               i + 1, (unsigned long long)results[i].id, results[i].score);
    }

    /* Check info */
    sws_info_t info;
    sws_info(idx, &info);
    ASSERT(info.hnsw_nodes == 100, "HNSW has 100 nodes");
    ASSERT(info.hnsw_levels >= 1, "HNSW has at least 1 level");
    printf("  -- HNSW levels: %u, nodes: %u, memory: %zu bytes\n",
           info.hnsw_levels, info.hnsw_nodes, info.memory_bytes);

    free(emb);
    free(query);
    sws_free(idx);
}

/* === Test: HNSW recall at scale === */
static void test_hnsw_recall(void) {
    printf("\n[test_hnsw_recall]\n");

    uint32_t dims = 32;
    int n_docs = 1000;

    sws_config_t cfg = {
        .dims = dims,
        .hnsw_m = 16,
        .hnsw_ef_construct = 200,
        .hnsw_ef_search = 50,
    };
    sws_index_t *idx = sws_new_with_config(cfg);

    /* Build brute-force index too for recall comparison */
    sws_config_t bf_cfg = {.dims = dims, .hnsw_m = 0};
    sws_index_t *bf_idx = sws_new_with_config(bf_cfg);

    float *emb = (float *)malloc(dims * sizeof(float));
    uint32_t rng = 7777;
    for (int i = 0; i < n_docs; i++) {
        /* Deterministic pseudo-random embeddings */
        float norm = 0;
        for (uint32_t d = 0; d < dims; d++) {
            rng = rng * 1664525u + 1013904223u;
            emb[d] = ((float)(rng & 0xFFFF) - 32768.0f) / 32768.0f;
            norm += emb[d] * emb[d];
        }
        norm = sqrtf(norm);
        for (uint32_t d = 0; d < dims; d++) emb[d] /= norm;

        char buf[64];
        snprintf(buf, sizeof(buf), "recall doc %d", i);
        sws_add(idx, (uint64_t)i, buf, emb, dims);
        sws_add(bf_idx, (uint64_t)i, buf, emb, dims);
    }

    /* Run 10 queries and measure recall@10 */
    int total_hits = 0;
    int total_possible = 0;
    int k = 10;

    for (int q = 0; q < 10; q++) {
        float *query = (float *)malloc(dims * sizeof(float));
        float norm = 0;
        for (uint32_t d = 0; d < dims; d++) {
            rng = rng * 1664525u + 1013904223u;
            query[d] = ((float)(rng & 0xFFFF) - 32768.0f) / 32768.0f;
            norm += query[d] * query[d];
        }
        norm = sqrtf(norm);
        for (uint32_t d = 0; d < dims; d++) query[d] /= norm;

        sws_result_t hnsw_results[10], bf_results[10];
        int n_hnsw = sws_vector_search(idx, query, dims, hnsw_results, k);
        int n_bf = sws_vector_search(bf_idx, query, dims, bf_results, k);

        /* Count how many of brute-force top-k appear in HNSW top-k */
        for (int i = 0; i < n_bf; i++) {
            for (int j = 0; j < n_hnsw; j++) {
                if (bf_results[i].id == hnsw_results[j].id) {
                    total_hits++;
                    break;
                }
            }
        }
        total_possible += n_bf;
        free(query);
    }

    float recall = (total_possible > 0) ? (float)total_hits / (float)total_possible : 0.0f;
    printf("  -- HNSW recall@%d on %d docs: %.1f%% (%d/%d)\n",
           k, n_docs, recall * 100.0f, total_hits, total_possible);
    ASSERT(recall > 0.80f, "HNSW recall@10 > 80% on 1K docs");

    free(emb);
    sws_free(idx);
    sws_free(bf_idx);
}

/* === Test: Hybrid search === */
static void test_hybrid_search(void) {
    printf("\n[test_hybrid_search]\n");

    uint32_t dims = 16;
    sws_config_t cfg = {
        .dims = dims,
        .hnsw_m = 8,
        .hnsw_ef_construct = 100,
        .hnsw_ef_search = 30,
    };
    sws_index_t *idx = sws_new_with_config(cfg);

    /* Doc 1: text-relevant for "login", embedding similar to query */
    float emb1[16], emb2[16], emb3[16], query_vec[16];
    for (uint32_t i = 0; i < dims; i++) {
        emb1[i] = (float)i * 0.1f;
        emb2[i] = (float)(dims - i) * 0.1f;
        emb3[i] = (float)i * 0.05f;
        query_vec[i] = (float)i * 0.1f;  /* matches emb1 */
    }

    sws_add(idx, 1, "fix login bug in auth controller", emb1, dims);
    sws_add(idx, 2, "add dark mode toggle to settings", emb2, dims);
    sws_add(idx, 3, "update login page styling", emb3, dims);

    sws_result_t results[5];
    int n = sws_hybrid_search(idx, "login bug", query_vec, dims, 0.5f, results, 5);

    ASSERT(n > 0, "hybrid search returns results");
    /* Doc 1 should rank high: BM25 has "login" + "bug", vector is exact match */
    ASSERT(results[0].id == 1, "hybrid top result is doc 1 (both modalities agree)");

    printf("  -- hybrid results for 'login bug' + vec:\n");
    for (int i = 0; i < n; i++) {
        printf("     %d. id=%llu score=%.6f text=\"%s\"\n",
               i + 1, (unsigned long long)results[i].id,
               results[i].score, results[i].text);
    }

    /* Text-only hybrid (no vector) */
    n = sws_hybrid_search(idx, "dark mode", NULL, 0, 0.5f, results, 5);
    ASSERT(n > 0, "hybrid text-only returns results");
    ASSERT(results[0].id == 2, "text-only hybrid top is doc 2");

    /* Vector-only hybrid (no text) */
    n = sws_hybrid_search(idx, NULL, query_vec, dims, 0.5f, results, 5);
    ASSERT(n > 0, "hybrid vector-only returns results");
    ASSERT(results[0].id == 1, "vector-only hybrid top is doc 1");

    sws_free(idx);
}

/* === Test: Save/Load roundtrip (SWS1 compat) === */
static void test_persistence_sws1(void) {
    printf("\n[test_persistence_sws1]\n");

    const char *path = "/tmp/test_swarmrt_search_v1.sws";
    uint32_t dims = 8;

    /* Build index with m=0 (no HNSW) -> saves as SWS1 */
    sws_config_t cfg = {.dims = dims, .hnsw_m = 0};
    sws_index_t *idx = sws_new_with_config(cfg);
    float emb[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    sws_add(idx, 100, "hello world", emb, dims);
    sws_add(idx, 200, "goodbye moon", NULL, 0);
    sws_add(idx, 300, "search engine test", emb, dims);

    int rc = sws_save(idx, path);
    ASSERT(rc == 0, "SWS1 save succeeds");
    sws_free(idx);

    sws_index_t *idx2 = sws_load(path);
    ASSERT(idx2 != NULL, "SWS1 load returns non-NULL");
    ASSERT(sws_count(idx2) == 3, "loaded index has 3 docs");

    /* Fuzzy search on loaded index */
    sws_result_t results[3];
    int n = sws_fuzzy_search(idx2, "hello", results, 3);
    ASSERT(n > 0, "fuzzy on loaded index returns results");
    ASSERT(results[0].id == 100, "top result is 'hello world'");

    /* BM25 on loaded index */
    n = sws_bm25_search(idx2, "search engine", results, 3);
    ASSERT(n > 0, "BM25 on loaded index works");

    sws_free(idx2);
    remove(path);
}

/* === Test: Save/Load roundtrip (SWS2 with HNSW) === */
static void test_persistence_sws2(void) {
    printf("\n[test_persistence_sws2]\n");

    const char *path = "/tmp/test_swarmrt_search_v2.sws";
    uint32_t dims = 16;

    sws_config_t cfg = {
        .dims = dims,
        .hnsw_m = 8,
        .hnsw_ef_construct = 100,
        .hnsw_ef_search = 30,
    };
    sws_index_t *idx = sws_new_with_config(cfg);

    float emb[16];
    for (int i = 0; i < 50; i++) {
        for (uint32_t d = 0; d < dims; d++)
            emb[d] = (float)((i * 31 + d * 7) % 100) * 0.01f;
        char buf[64];
        snprintf(buf, sizeof(buf), "sws2 doc %d", i);
        sws_add(idx, (uint64_t)i, buf, emb, dims);
    }

    sws_info_t info_before;
    sws_info(idx, &info_before);

    int rc = sws_save(idx, path);
    ASSERT(rc == 0, "SWS2 save succeeds");
    sws_free(idx);

    sws_index_t *idx2 = sws_load(path);
    ASSERT(idx2 != NULL, "SWS2 load returns non-NULL");
    ASSERT(sws_count(idx2) == 50, "SWS2 loaded index has 50 docs");

    sws_info_t info_after;
    sws_info(idx2, &info_after);
    ASSERT(info_after.hnsw_nodes == info_before.hnsw_nodes, "HNSW node count matches after load");

    /* Vector search on loaded HNSW */
    for (uint32_t d = 0; d < dims; d++)
        emb[d] = (float)((25 * 31 + d * 7) % 100) * 0.01f;
    sws_result_t results[5];
    int n = sws_vector_search(idx2, emb, dims, results, 5);
    ASSERT(n > 0, "vector search on loaded SWS2 works");
    ASSERT(results[0].id == 25, "loaded HNSW finds doc 25");

    /* BM25 on loaded SWS2 */
    n = sws_bm25_search(idx2, "sws2 doc", results, 5);
    ASSERT(n > 0, "BM25 on loaded SWS2 works");

    sws_free(idx2);
    remove(path);
}

/* === Test: Scale (1K docs) === */
static void test_scale(void) {
    printf("\n[test_scale]\n");

    sws_index_t *idx = sws_new(0);

    /* Add 1K docs */
    for (int i = 0; i < 1000; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "document number %d with some text content", i);
        sws_add(idx, (uint64_t)i, buf, NULL, 0);
    }
    ASSERT(sws_count(idx) == 1000, "1000 docs added");

    /* Time fuzzy search */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    sws_result_t results[10];
    int n = sws_fuzzy_search(idx, "document number 42", results, 10);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;

    ASSERT(n > 0, "fuzzy on 1K docs returns results");
    printf("  -- fuzzy search on 1K docs: %d results in %.1f us\n", n, elapsed_us);

    /* Time BM25 search */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    n = sws_bm25_search(idx, "document number 42", results, 10);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed_us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;

    ASSERT(n > 0, "BM25 on 1K docs returns results");
    printf("  -- BM25 search on 1K docs: %d results in %.1f us\n", n, elapsed_us);

    sws_free(idx);
}

/* === Test: Scale vector (1K docs, 384 dims) === */
static void test_scale_vector(void) {
    printf("\n[test_scale_vector]\n");

    uint32_t dims = 384;
    sws_index_t *idx = sws_new(dims);

    /* Add 1K docs with random-ish embeddings */
    float *emb = (float *)malloc(dims * sizeof(float));
    for (int i = 0; i < 1000; i++) {
        for (uint32_t d = 0; d < dims; d++) {
            emb[d] = (float)((i * 31 + d * 7) % 100) * 0.01f;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "vector doc %d", i);
        sws_add(idx, (uint64_t)i, buf, emb, dims);
    }
    ASSERT(sws_count(idx) == 1000, "1000 vector docs added");

    /* Query vector */
    float *query = (float *)malloc(dims * sizeof(float));
    for (uint32_t d = 0; d < dims; d++) {
        query[d] = (float)((42 * 31 + d * 7) % 100) * 0.01f;  /* Match doc 42 */
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    sws_result_t results[10];
    int n = sws_vector_search(idx, query, dims, results, 10);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;

    ASSERT(n > 0, "HNSW search on 1K docs returns results");
    ASSERT(results[0].id == 42, "top result is doc 42 (exact match)");
    ASSERT(fabsf(results[0].score - 1.0f) < 0.01f, "exact match score ~1.0");
    printf("  -- HNSW search on 1K docs (384 dims): %d results in %.1f us\n", n, elapsed_us);

    free(emb);
    free(query);
    sws_free(idx);
}

/* === Test: sws_info === */
static void test_info(void) {
    printf("\n[test_info]\n");

    sws_index_t *idx = sws_new(0);
    sws_add(idx, 1, "hello world test", NULL, 0);
    sws_add(idx, 2, "goodbye world test", NULL, 0);

    sws_info_t info;
    sws_info(idx, &info);

    ASSERT(info.doc_count == 2, "info doc_count is 2");
    ASSERT(info.token_count > 0, "info has tokens");
    ASSERT(info.trigram_count > 0, "info has trigrams");
    ASSERT(info.memory_bytes > 0, "info reports memory usage");
    printf("  -- info: docs=%u tokens=%u trigrams=%u mem=%zu\n",
           info.doc_count, info.token_count, info.trigram_count, info.memory_bytes);

    sws_free(idx);
}

/* === Main === */

int main(void) {
    printf("=== SwarmRT Search v2 Tests ===\n");

    test_lifecycle();
    test_config();
    test_add_count();
    test_remove();
    test_fuzzy_search();
    test_bm25_search();
    test_bm25_remove();
    test_cosine();
    test_vector_search();
    test_hnsw_search();
    test_hnsw_recall();
    test_hybrid_search();
    test_persistence_sws1();
    test_persistence_sws2();
    test_scale();
    test_scale_vector();
    test_info();

    printf("\n=== Results: %d passed, %d failed ===\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
