/*
 * SwarmRT Search v2 - BM25 + HNSW + Fuzzy + Vector + Hybrid
 *
 * - Trigram-based fuzzy text search with Jaccard similarity
 * - BM25 inverted index for full-text search (Tantivy equivalent)
 * - HNSW graph for O(log n) approximate nearest neighbor (Qdrant equivalent)
 * - Hybrid search via reciprocal rank fusion (BM25 + vector)
 * - SIMD-accelerated cosine similarity (NEON / AVX / SSE / scalar)
 * - Bump arena for posting list nodes (no per-node malloc)
 * - Chained hash tables (same pattern as swarmrt_ets.c)
 * - Per-index pthread_rwlock_t for concurrent reads
 * - Binary persistence (SWS1 + SWS2 format)
 * - Adaptive capacity scaling (phone / laptop / server)
 *
 * otonomy.ai
 */

#ifndef SWARMRT_SEARCH_H
#define SWARMRT_SEARCH_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* === Configuration === */

#define SWS_DOC_BUCKETS      1024
#define SWS_TRIGRAM_BUCKETS   4096
#define SWS_TOKEN_BUCKETS     8192
#define SWS_MAX_TRIGRAMS      512   /* Max trigrams cached per doc */
#define SWS_ARENA_BLOCK_SIZE  (64 * 1024)  /* 64KB arena blocks */
#define SWS_MAGIC             0x53575331   /* "SWS1" */
#define SWS_MAGIC_V2          0x53575332   /* "SWS2" */

/* BM25 tuning (Lucene/Tantivy defaults) */
#define SWS_BM25_K1           1.2f
#define SWS_BM25_B            0.75f

/* === Adaptive Config === */

typedef struct {
    uint32_t dims;              /* embedding dimensions (0 = text-only) */
    /* HNSW params — scale with device */
    uint32_t hnsw_m;            /* max connections/node (default 16, phone: 8, GPU: 32) */
    uint32_t hnsw_ef_construct; /* build beam width (default 200, phone: 100, GPU: 400) */
    uint32_t hnsw_ef_search;    /* search beam width (default 50, phone: 20, GPU: 100) */
    uint32_t max_docs;          /* capacity hint for pre-allocation (default 0 = grow) */
} sws_config_t;

/* === Bump Arena (for posting list nodes) === */

typedef struct sws_arena_block {
    uint8_t *data;
    uint32_t used;
    uint32_t capacity;
    struct sws_arena_block *next;
} sws_arena_block_t;

typedef struct {
    sws_arena_block_t *head;
    size_t total_allocated;
} sws_arena_t;

/* === Document === */

typedef struct sws_doc {
    uint64_t id;
    char *text;                          /* owned, malloc'd */
    uint32_t text_len;
    float *embedding;                    /* owned, malloc'd, NULL if no vector */
    uint32_t num_trigrams;
    uint32_t trigram_hashes[SWS_MAX_TRIGRAMS]; /* cached for Jaccard + removal */
    uint32_t token_count;                /* number of tokens in text (for BM25) */
    uint32_t hnsw_node_idx;             /* index into hnsw.nodes, UINT32_MAX if not in graph */
    struct sws_doc *next;                /* hash chain */
} sws_doc_t;

/* === Trigram Posting List (arena-allocated) === */

typedef struct sws_posting {
    sws_doc_t *doc;
    struct sws_posting *next;
} sws_posting_t;

/* === Trigram Bucket Entry === */

typedef struct sws_trigram_entry {
    uint32_t trigram_hash;
    uint32_t count;
    sws_posting_t *postings;
    struct sws_trigram_entry *next;       /* hash chain */
} sws_trigram_entry_t;

/* === BM25 Token Posting (arena-allocated) === */

typedef struct sws_token_posting {
    sws_doc_t *doc;
    uint32_t term_freq;                  /* how many times this token appears in this doc */
    struct sws_token_posting *next;
} sws_token_posting_t;

/* === BM25 Token Bucket Entry === */

typedef struct sws_token_entry {
    uint32_t token_hash;
    uint32_t doc_freq;                   /* how many docs contain this token */
    sws_token_posting_t *postings;
    struct sws_token_entry *next;        /* hash chain */
} sws_token_entry_t;

/* === HNSW Graph === */

typedef struct sws_hnsw_node {
    sws_doc_t *doc;
    uint32_t level;                      /* max layer this node appears in */
    uint32_t *neighbors;                 /* flat array: [layer0_n, n0, n1, ..., layer1_n, n0, ...] */
    uint32_t neighbors_size;             /* used size */
    uint32_t neighbors_cap;              /* allocated size */
} sws_hnsw_node_t;

typedef struct {
    sws_hnsw_node_t *nodes;              /* array indexed by insertion order */
    uint32_t node_count;
    uint32_t node_cap;
    uint32_t entry_point;                /* index of entry node (highest level) */
    uint32_t max_level;                  /* current max level in graph */
    uint32_t m;                          /* max connections per node per layer */
    uint32_t m_max0;                     /* max connections at layer 0 (= 2*m) */
    uint32_t ef_construction;
    uint32_t ef_search;
    float ml;                            /* level generation factor = 1/ln(m) */
} sws_hnsw_t;

/* === Hash Map for Fuzzy Hit Counting === */

typedef struct {
    sws_doc_t **keys;
    uint32_t *values;
    uint32_t capacity;
    uint32_t count;
} sws_hitmap_t;

/* === Search Result === */

typedef struct {
    uint64_t id;
    float score;
    const char *text;                    /* borrowed pointer, valid while index alive */
} sws_result_t;

/* === Index Info === */

typedef struct {
    uint32_t doc_count;
    uint32_t token_count;
    uint32_t trigram_count;
    uint32_t hnsw_levels;
    uint32_t hnsw_nodes;
    uint32_t dims;
    size_t   memory_bytes;
} sws_info_t;

/* === Top-level Index === */

typedef struct {
    sws_doc_t *doc_buckets[SWS_DOC_BUCKETS];
    uint32_t doc_count;

    sws_trigram_entry_t *trigram_buckets[SWS_TRIGRAM_BUCKETS];
    uint32_t trigram_count;

    /* BM25 inverted index */
    sws_token_entry_t *token_buckets[SWS_TOKEN_BUCKETS];
    uint32_t token_count;               /* unique tokens */
    uint64_t total_doc_length;           /* sum of all doc token counts (for avgdl) */

    /* HNSW graph */
    sws_hnsw_t hnsw;

    /* Config */
    sws_config_t config;

    uint32_t dims;                       /* embedding dimensions, 0 = no vectors */

    sws_arena_t arena;
    pthread_rwlock_t rwlock;
} sws_index_t;

/* === Public API === */

/* Lifecycle */
sws_index_t *sws_new(uint32_t dims);
sws_index_t *sws_new_with_config(sws_config_t config);
void         sws_free(sws_index_t *idx);

/* Document operations (caller holds or acquires write lock) */
int  sws_add(sws_index_t *idx, uint64_t id, const char *text,
             const float *embedding, uint32_t embedding_len);
int  sws_remove(sws_index_t *idx, uint64_t id);

/* Search (caller holds or acquires read lock) */
int  sws_fuzzy_search(sws_index_t *idx, const char *query,
                      sws_result_t *results, int limit);
int  sws_vector_search(sws_index_t *idx, const float *query_vec,
                       uint32_t query_len, sws_result_t *results, int limit);
int  sws_bm25_search(sws_index_t *idx, const char *query,
                     sws_result_t *results, int limit);
int  sws_hybrid_search(sws_index_t *idx, const char *query,
                       const float *query_vec, uint32_t query_len,
                       float alpha, sws_result_t *results, int limit);

/* Persistence */
int  sws_save(sws_index_t *idx, const char *path);
sws_index_t *sws_load(const char *path);

/* Info */
uint32_t sws_count(sws_index_t *idx);
void     sws_info(sws_index_t *idx, sws_info_t *info);

/* === Internal helpers (exposed for NIF, not for general use) === */

uint32_t sws_fnv1a(const char *data, uint32_t len);
float    sws_cosine_similarity(const float *a, const float *b, uint32_t dims);

#endif /* SWARMRT_SEARCH_H */
