/*
 * SwarmRT Search v2 - BM25 + HNSW + Fuzzy + Vector + Hybrid
 *
 * Trigram index for fuzzy text search + BM25 inverted index for full-text +
 * HNSW graph for ANN + SIMD-accelerated cosine similarity.
 * Arena-allocated posting lists, chained hash tables, binary persistence.
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <pthread.h>
#include "swarmrt_search.h"

/* SIMD headers */
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#endif

/* ============================================================
 * Arena Allocator
 * ============================================================ */

static void arena_init(sws_arena_t *a) {
    a->head = NULL;
    a->total_allocated = 0;
}

static void *arena_alloc(sws_arena_t *a, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    /* Try current block */
    if (a->head && (a->head->used + size <= a->head->capacity)) {
        void *ptr = a->head->data + a->head->used;
        a->head->used += (uint32_t)size;
        return ptr;
    }

    /* Allocate new block */
    uint32_t block_cap = SWS_ARENA_BLOCK_SIZE;
    if (size > block_cap) block_cap = (uint32_t)size;

    sws_arena_block_t *block = (sws_arena_block_t *)malloc(sizeof(sws_arena_block_t));
    if (!block) return NULL;

    block->data = (uint8_t *)malloc(block_cap);
    if (!block->data) { free(block); return NULL; }

    block->capacity = block_cap;
    block->used = (uint32_t)size;
    block->next = a->head;
    a->head = block;
    a->total_allocated += block_cap;

    return block->data;
}

static void arena_destroy(sws_arena_t *a) {
    sws_arena_block_t *b = a->head;
    while (b) {
        sws_arena_block_t *next = b->next;
        free(b->data);
        free(b);
        b = next;
    }
    a->head = NULL;
    a->total_allocated = 0;
}

/* ============================================================
 * FNV-1a Hash (same as swarmrt_ets.c)
 * ============================================================ */

uint32_t sws_fnv1a(const char *data, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 0x100000001b3ULL;
    }
    /* Finalizer */
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

/* ============================================================
 * Trigram Extraction
 * ============================================================ */

static uint32_t extract_trigrams(const char *text, uint32_t text_len,
                                  uint32_t *out_hashes, uint32_t max_trigrams) {
    if (text_len < 3) return 0;

    /* Lowercase copy (stack for short strings, heap for long) */
    char stack_buf[1024];
    char *lower = (text_len <= sizeof(stack_buf)) ? stack_buf : (char *)malloc(text_len);
    if (!lower) return 0;

    for (uint32_t i = 0; i < text_len; i++) {
        lower[i] = (char)tolower((unsigned char)text[i]);
    }

    uint32_t count = 0;
    uint32_t limit = text_len - 2;

    /* Bitmap dedup — 1024-slot hash set (O(1) vs O(n) linear scan) */
    uint64_t seen[16] = {0};  /* 1024 bits = 16 * 64 */
    #define TRIG_SEEN_SLOT(h) ((h) & 1023)

    for (uint32_t i = 0; i < limit && count < max_trigrams; i++) {
        uint32_t h = sws_fnv1a(lower + i, 3);

        uint32_t slot = TRIG_SEEN_SLOT(h);
        uint32_t word = slot >> 6;
        uint64_t bit = (uint64_t)1 << (slot & 63);
        if (seen[word] & bit) continue;  /* likely duplicate */
        seen[word] |= bit;
        out_hashes[count++] = h;
    }
    #undef TRIG_SEEN_SLOT

    if (lower != stack_buf) free(lower);
    return count;
}

/* ============================================================
 * Tokenizer (for BM25)
 * Split on whitespace + punctuation, lowercase, skip < 2 chars
 * ============================================================ */

#define SWS_MAX_QUERY_TOKENS 64

typedef struct {
    uint32_t hash;
    uint32_t count;  /* for TF in query */
} sws_query_token_t;

/* Tokenize text, return token hashes via callback or into array.
 * Returns total token count (for doc length). */
static uint32_t tokenize_text(const char *text, uint32_t text_len,
                               uint32_t *out_hashes, uint32_t max_tokens) {
    uint32_t count = 0;
    uint32_t tok_start = 0;
    int in_token = 0;

    for (uint32_t i = 0; i <= text_len; i++) {
        int is_sep = (i == text_len) || !isalnum((unsigned char)text[i]);
        if (is_sep) {
            if (in_token && (i - tok_start) >= 2) {
                /* Lowercase and hash */
                char buf[128];
                uint32_t tlen = i - tok_start;
                if (tlen > sizeof(buf)) tlen = sizeof(buf);
                for (uint32_t j = 0; j < tlen; j++)
                    buf[j] = (char)tolower((unsigned char)text[tok_start + j]);
                uint32_t h = sws_fnv1a(buf, tlen);
                if (out_hashes && count < max_tokens)
                    out_hashes[count] = h;
                count++;
            }
            in_token = 0;
        } else {
            if (!in_token) tok_start = i;
            in_token = 1;
        }
    }
    return count;
}

/* Tokenize query with deduplication + TF counts */
static int tokenize_query(const char *query, sws_query_token_t *tokens, int max_tokens) {
    uint32_t hashes[256];
    uint32_t qlen = (uint32_t)strlen(query);
    uint32_t n = tokenize_text(query, qlen, hashes, 256);

    /* Dedup with index lookup table (O(1) per token vs O(n) linear scan) */
    int unique = 0;
    int8_t idx_map[128];  /* hash → token index, -1 = empty */
    memset(idx_map, -1, sizeof(idx_map));

    for (uint32_t i = 0; i < n && i < 256; i++) {
        uint32_t slot = hashes[i] & 127;
        /* Linear probe in the small table */
        int found = 0;
        for (int probe = 0; probe < 8; probe++) {
            uint32_t s = (slot + (uint32_t)probe) & 127;
            if (idx_map[s] < 0) {
                /* Empty slot — new token */
                if (unique < max_tokens) {
                    idx_map[s] = (int8_t)unique;
                    tokens[unique].hash = hashes[i];
                    tokens[unique].count = 1;
                    unique++;
                }
                found = 1;
                break;
            } else if (tokens[(int)idx_map[s]].hash == hashes[i]) {
                /* Found — increment TF */
                tokens[(int)idx_map[s]].count++;
                found = 1;
                break;
            }
        }
        if (!found && unique < max_tokens) {
            /* Probe chain full — fallback to direct insert */
            tokens[unique].hash = hashes[i];
            tokens[unique].count = 1;
            unique++;
        }
    }
    return unique;
}

/* ============================================================
 * SIMD Cosine Similarity
 * ============================================================ */

#if defined(__aarch64__)
/* ARM64 NEON — fused multiply-add, 4x unroll */
float sws_cosine_similarity(const float *a, const float *b, uint32_t dims) {
    float32x4_t dot0 = vdupq_n_f32(0.0f);
    float32x4_t dot1 = vdupq_n_f32(0.0f);
    float32x4_t dot2 = vdupq_n_f32(0.0f);
    float32x4_t dot3 = vdupq_n_f32(0.0f);
    float32x4_t na0  = vdupq_n_f32(0.0f);
    float32x4_t na1  = vdupq_n_f32(0.0f);
    float32x4_t na2  = vdupq_n_f32(0.0f);
    float32x4_t na3  = vdupq_n_f32(0.0f);
    float32x4_t nb0  = vdupq_n_f32(0.0f);
    float32x4_t nb1  = vdupq_n_f32(0.0f);
    float32x4_t nb2  = vdupq_n_f32(0.0f);
    float32x4_t nb3  = vdupq_n_f32(0.0f);

    uint32_t i = 0;
    uint32_t end16 = dims & ~15u;

    for (; i < end16; i += 16) {
        float32x4_t va0 = vld1q_f32(a + i);
        float32x4_t va1 = vld1q_f32(a + i + 4);
        float32x4_t va2 = vld1q_f32(a + i + 8);
        float32x4_t va3 = vld1q_f32(a + i + 12);
        float32x4_t vb0 = vld1q_f32(b + i);
        float32x4_t vb1 = vld1q_f32(b + i + 4);
        float32x4_t vb2 = vld1q_f32(b + i + 8);
        float32x4_t vb3 = vld1q_f32(b + i + 12);

        dot0 = vfmaq_f32(dot0, va0, vb0);
        dot1 = vfmaq_f32(dot1, va1, vb1);
        dot2 = vfmaq_f32(dot2, va2, vb2);
        dot3 = vfmaq_f32(dot3, va3, vb3);

        na0 = vfmaq_f32(na0, va0, va0);
        na1 = vfmaq_f32(na1, va1, va1);
        na2 = vfmaq_f32(na2, va2, va2);
        na3 = vfmaq_f32(na3, va3, va3);

        nb0 = vfmaq_f32(nb0, vb0, vb0);
        nb1 = vfmaq_f32(nb1, vb1, vb1);
        nb2 = vfmaq_f32(nb2, vb2, vb2);
        nb3 = vfmaq_f32(nb3, vb3, vb3);
    }

    /* Horizontal reduction */
    float dot = vaddvq_f32(vaddq_f32(vaddq_f32(dot0, dot1), vaddq_f32(dot2, dot3)));
    float norm_a = vaddvq_f32(vaddq_f32(vaddq_f32(na0, na1), vaddq_f32(na2, na3)));
    float norm_b = vaddvq_f32(vaddq_f32(vaddq_f32(nb0, nb1), vaddq_f32(nb2, nb3)));

    /* Handle remainder */
    for (; i < dims; i++) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    float denom = sqrtf(norm_a) * sqrtf(norm_b);
    return (denom > 1e-8f) ? (dot / denom) : 0.0f;
}

#elif defined(__x86_64__) && defined(__AVX__) && defined(__FMA__)
/* x86_64 AVX+FMA — 8 floats/iteration */
float sws_cosine_similarity(const float *a, const float *b, uint32_t dims) {
    __m256 dot0 = _mm256_setzero_ps();
    __m256 dot1 = _mm256_setzero_ps();
    __m256 na0  = _mm256_setzero_ps();
    __m256 na1  = _mm256_setzero_ps();
    __m256 nb0  = _mm256_setzero_ps();
    __m256 nb1  = _mm256_setzero_ps();

    uint32_t i = 0;
    uint32_t end16 = dims & ~15u;

    for (; i < end16; i += 16) {
        __m256 va0 = _mm256_loadu_ps(a + i);
        __m256 va1 = _mm256_loadu_ps(a + i + 8);
        __m256 vb0 = _mm256_loadu_ps(b + i);
        __m256 vb1 = _mm256_loadu_ps(b + i + 8);

        dot0 = _mm256_fmadd_ps(va0, vb0, dot0);
        dot1 = _mm256_fmadd_ps(va1, vb1, dot1);
        na0  = _mm256_fmadd_ps(va0, va0, na0);
        na1  = _mm256_fmadd_ps(va1, va1, na1);
        nb0  = _mm256_fmadd_ps(vb0, vb0, nb0);
        nb1  = _mm256_fmadd_ps(vb1, vb1, nb1);
    }

    /* Horizontal sum for AVX */
    __m256 dot_sum = _mm256_add_ps(dot0, dot1);
    __m256 na_sum  = _mm256_add_ps(na0, na1);
    __m256 nb_sum  = _mm256_add_ps(nb0, nb1);

    /* 256 -> 128 */
    __m128 dot_lo = _mm256_castps256_ps128(dot_sum);
    __m128 dot_hi = _mm256_extractf128_ps(dot_sum, 1);
    __m128 dot128 = _mm_add_ps(dot_lo, dot_hi);

    __m128 na_lo = _mm256_castps256_ps128(na_sum);
    __m128 na_hi = _mm256_extractf128_ps(na_sum, 1);
    __m128 na128 = _mm_add_ps(na_lo, na_hi);

    __m128 nb_lo = _mm256_castps256_ps128(nb_sum);
    __m128 nb_hi = _mm256_extractf128_ps(nb_sum, 1);
    __m128 nb128 = _mm_add_ps(nb_lo, nb_hi);

    /* 128 -> scalar (hadd twice) */
    dot128 = _mm_hadd_ps(dot128, dot128);
    dot128 = _mm_hadd_ps(dot128, dot128);
    na128  = _mm_hadd_ps(na128, na128);
    na128  = _mm_hadd_ps(na128, na128);
    nb128  = _mm_hadd_ps(nb128, nb128);
    nb128  = _mm_hadd_ps(nb128, nb128);

    float dot = _mm_cvtss_f32(dot128);
    float norm_a = _mm_cvtss_f32(na128);
    float norm_b = _mm_cvtss_f32(nb128);

    /* Remainder */
    for (; i < dims; i++) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    float denom = sqrtf(norm_a) * sqrtf(norm_b);
    return (denom > 1e-8f) ? (dot / denom) : 0.0f;
}

#elif defined(__x86_64__) && defined(__SSE__)
/* x86_64 SSE fallback — 4 floats/iteration */
float sws_cosine_similarity(const float *a, const float *b, uint32_t dims) {
    __m128 dot0 = _mm_setzero_ps();
    __m128 na0  = _mm_setzero_ps();
    __m128 nb0  = _mm_setzero_ps();

    uint32_t i = 0;
    uint32_t end4 = dims & ~3u;

    for (; i < end4; i += 4) {
        __m128 va = _mm_loadu_ps(a + i);
        __m128 vb = _mm_loadu_ps(b + i);
        dot0 = _mm_add_ps(dot0, _mm_mul_ps(va, vb));
        na0  = _mm_add_ps(na0, _mm_mul_ps(va, va));
        nb0  = _mm_add_ps(nb0, _mm_mul_ps(vb, vb));
    }

    /* Horizontal sum */
    dot0 = _mm_hadd_ps(dot0, dot0);
    dot0 = _mm_hadd_ps(dot0, dot0);
    na0  = _mm_hadd_ps(na0, na0);
    na0  = _mm_hadd_ps(na0, na0);
    nb0  = _mm_hadd_ps(nb0, nb0);
    nb0  = _mm_hadd_ps(nb0, nb0);

    float dot = _mm_cvtss_f32(dot0);
    float norm_a = _mm_cvtss_f32(na0);
    float norm_b = _mm_cvtss_f32(nb0);

    for (; i < dims; i++) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    float denom = sqrtf(norm_a) * sqrtf(norm_b);
    return (denom > 1e-8f) ? (dot / denom) : 0.0f;
}

#else
/* Scalar fallback */
float sws_cosine_similarity(const float *a, const float *b, uint32_t dims) {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (uint32_t i = 0; i < dims; i++) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    float denom = sqrtf(norm_a) * sqrtf(norm_b);
    return (denom > 1e-8f) ? (dot / denom) : 0.0f;
}
#endif

/* ============================================================
 * Hash Map (open addressing, for fuzzy hit counting + BM25 scoring)
 * ============================================================ */

static void hitmap_init(sws_hitmap_t *hm, uint32_t capacity) {
    hm->capacity = capacity;
    hm->count = 0;
    hm->keys = (sws_doc_t **)calloc(capacity, sizeof(sws_doc_t *));
    hm->values = (uint32_t *)calloc(capacity, sizeof(uint32_t));
}

static void hitmap_free(sws_hitmap_t *hm) {
    free(hm->keys);
    free(hm->values);
    hm->keys = NULL;
    hm->values = NULL;
    hm->capacity = 0;
    hm->count = 0;
}

/* Returns pointer to value slot for key (creates if not found) */
static uint32_t *hitmap_get_or_insert(sws_hitmap_t *hm, sws_doc_t *key) {
    uint32_t h = (uint32_t)((uintptr_t)key >> 4);
    uint32_t mask = hm->capacity - 1;
    uint32_t idx = h & mask;

    for (uint32_t i = 0; i < hm->capacity; i++) {
        uint32_t slot = (idx + i) & mask;
        if (hm->keys[slot] == key) {
            return &hm->values[slot];
        }
        if (hm->keys[slot] == NULL) {
            hm->keys[slot] = key;
            hm->values[slot] = 0;
            hm->count++;
            return &hm->values[slot];
        }
    }
    return NULL; /* full — shouldn't happen if sized correctly */
}

/* Float version for BM25 score accumulation */
typedef struct {
    sws_doc_t **keys;
    float *values;
    uint32_t capacity;
    uint32_t count;
} sws_scoremap_t;

static void scoremap_init(sws_scoremap_t *sm, uint32_t capacity) {
    sm->capacity = capacity;
    sm->count = 0;
    sm->keys = (sws_doc_t **)calloc(capacity, sizeof(sws_doc_t *));
    sm->values = (float *)calloc(capacity, sizeof(float));
}

static void scoremap_free(sws_scoremap_t *sm) {
    free(sm->keys);
    free(sm->values);
    sm->keys = NULL;
    sm->values = NULL;
}

static float *scoremap_get_or_insert(sws_scoremap_t *sm, sws_doc_t *key) {
    uint32_t h = (uint32_t)((uintptr_t)key >> 4);
    uint32_t mask = sm->capacity - 1;
    uint32_t idx = h & mask;

    for (uint32_t i = 0; i < sm->capacity; i++) {
        uint32_t slot = (idx + i) & mask;
        if (sm->keys[slot] == key) {
            return &sm->values[slot];
        }
        if (sm->keys[slot] == NULL) {
            sm->keys[slot] = key;
            sm->values[slot] = 0.0f;
            sm->count++;
            return &sm->values[slot];
        }
    }
    return NULL;
}

/* ============================================================
 * Index Lifecycle
 * ============================================================ */

static void hnsw_init(sws_hnsw_t *g, sws_config_t *cfg) {
    memset(g, 0, sizeof(sws_hnsw_t));
    g->m = cfg->hnsw_m;
    g->m_max0 = cfg->hnsw_m * 2;
    g->ef_construction = cfg->hnsw_ef_construct;
    g->ef_search = cfg->hnsw_ef_search;
    g->ml = (cfg->hnsw_m > 1) ? (1.0f / logf((float)cfg->hnsw_m)) : 1.0f;
    g->entry_point = UINT32_MAX;

    /* Pre-allocate node array */
    uint32_t init_cap = (cfg->max_docs > 0) ? cfg->max_docs : 1024;
    g->nodes = (sws_hnsw_node_t *)calloc(init_cap, sizeof(sws_hnsw_node_t));
    g->node_cap = g->nodes ? init_cap : 0;
    g->node_count = 0;
}

static void hnsw_destroy(sws_hnsw_t *g) {
    for (uint32_t i = 0; i < g->node_count; i++) {
        free(g->nodes[i].neighbors);
    }
    free(g->nodes);
    memset(g, 0, sizeof(sws_hnsw_t));
}

sws_index_t *sws_new_with_config(sws_config_t config) {
    sws_index_t *idx = (sws_index_t *)calloc(1, sizeof(sws_index_t));
    if (!idx) return NULL;

    idx->dims = config.dims;
    idx->config = config;
    arena_init(&idx->arena);
    pthread_rwlock_init(&idx->rwlock, NULL);

    if (config.dims > 0 && config.hnsw_m > 0) {
        hnsw_init(&idx->hnsw, &config);
    }

    return idx;
}

sws_index_t *sws_new(uint32_t dims) {
    sws_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.dims = dims;
    cfg.hnsw_m = 16;
    cfg.hnsw_ef_construct = 200;
    cfg.hnsw_ef_search = 50;
    cfg.max_docs = 0;
    return sws_new_with_config(cfg);
}

void sws_free(sws_index_t *idx) {
    if (!idx) return;

    /* Free all documents */
    for (int i = 0; i < SWS_DOC_BUCKETS; i++) {
        sws_doc_t *d = idx->doc_buckets[i];
        while (d) {
            sws_doc_t *next = d->next;
            free(d->text);
            free(d->embedding);
            free(d);
            d = next;
        }
    }

    /* Free trigram entries (entry structs are malloc'd, postings are arena'd) */
    for (int i = 0; i < SWS_TRIGRAM_BUCKETS; i++) {
        sws_trigram_entry_t *e = idx->trigram_buckets[i];
        while (e) {
            sws_trigram_entry_t *next = e->next;
            free(e);
            e = next;
        }
    }

    /* Free token entries */
    for (int i = 0; i < SWS_TOKEN_BUCKETS; i++) {
        sws_token_entry_t *e = idx->token_buckets[i];
        while (e) {
            sws_token_entry_t *next = e->next;
            free(e);
            e = next;
        }
    }

    /* Free HNSW graph */
    hnsw_destroy(&idx->hnsw);

    arena_destroy(&idx->arena);
    pthread_rwlock_destroy(&idx->rwlock);
    free(idx);
}

/* ============================================================
 * Document Lookup (internal)
 * ============================================================ */

static sws_doc_t *doc_find(sws_index_t *idx, uint64_t id) {
    uint32_t bucket = (uint32_t)(id % SWS_DOC_BUCKETS);
    sws_doc_t *d = idx->doc_buckets[bucket];
    while (d) {
        if (d->id == id) return d;
        d = d->next;
    }
    return NULL;
}

/* ============================================================
 * Trigram Index Operations (internal)
 * ============================================================ */

static sws_trigram_entry_t *trigram_find_or_create(sws_index_t *idx, uint32_t hash) {
    uint32_t bucket = hash % SWS_TRIGRAM_BUCKETS;
    sws_trigram_entry_t *e = idx->trigram_buckets[bucket];
    while (e) {
        if (e->trigram_hash == hash) return e;
        e = e->next;
    }

    /* Create new entry */
    e = (sws_trigram_entry_t *)malloc(sizeof(sws_trigram_entry_t));
    if (!e) return NULL;

    e->trigram_hash = hash;
    e->count = 0;
    e->postings = NULL;
    e->next = idx->trigram_buckets[bucket];
    idx->trigram_buckets[bucket] = e;
    idx->trigram_count++;
    return e;
}

static void trigram_add_posting(sws_index_t *idx, uint32_t hash, sws_doc_t *doc) {
    sws_trigram_entry_t *entry = trigram_find_or_create(idx, hash);
    if (!entry) return;

    sws_posting_t *p = (sws_posting_t *)arena_alloc(&idx->arena, sizeof(sws_posting_t));
    if (!p) return;

    p->doc = doc;
    p->next = entry->postings;
    entry->postings = p;
    entry->count++;
}

static void trigram_remove_doc(sws_index_t *idx, sws_doc_t *doc) {
    for (uint32_t t = 0; t < doc->num_trigrams; t++) {
        uint32_t hash = doc->trigram_hashes[t];
        uint32_t bucket = hash % SWS_TRIGRAM_BUCKETS;
        sws_trigram_entry_t *e = idx->trigram_buckets[bucket];

        while (e) {
            if (e->trigram_hash == hash) {
                sws_posting_t **pp = &e->postings;
                while (*pp) {
                    if ((*pp)->doc == doc) {
                        *pp = (*pp)->next;
                        e->count--;
                        break;
                    }
                    pp = &(*pp)->next;
                }
                break;
            }
            e = e->next;
        }
    }
}

/* ============================================================
 * BM25 Token Index Operations (internal)
 * ============================================================ */

static sws_token_entry_t *token_find_or_create(sws_index_t *idx, uint32_t hash) {
    uint32_t bucket = hash % SWS_TOKEN_BUCKETS;
    sws_token_entry_t *e = idx->token_buckets[bucket];
    while (e) {
        if (e->token_hash == hash) return e;
        e = e->next;
    }

    e = (sws_token_entry_t *)malloc(sizeof(sws_token_entry_t));
    if (!e) return NULL;

    e->token_hash = hash;
    e->doc_freq = 0;
    e->postings = NULL;
    e->next = idx->token_buckets[bucket];
    idx->token_buckets[bucket] = e;
    idx->token_count++;
    return e;
}

static void token_index_doc(sws_index_t *idx, sws_doc_t *doc) {
    /* Tokenize and count per-token TF using a small local map */
    uint32_t hashes[512];
    uint32_t n = tokenize_text(doc->text, doc->text_len, hashes, 512);
    doc->token_count = n;
    idx->total_doc_length += n;

    /* Count unique tokens and their TF (using temp array) */
    typedef struct { uint32_t hash; uint32_t tf; } tf_pair_t;
    tf_pair_t tfs[512];
    int unique = 0;

    uint32_t limit = (n < 512) ? n : 512;
    for (uint32_t i = 0; i < limit; i++) {
        int found = 0;
        for (int j = 0; j < unique; j++) {
            if (tfs[j].hash == hashes[i]) {
                tfs[j].tf++;
                found = 1;
                break;
            }
        }
        if (!found && unique < 512) {
            tfs[unique].hash = hashes[i];
            tfs[unique].tf = 1;
            unique++;
        }
    }

    /* Add postings */
    for (int i = 0; i < unique; i++) {
        sws_token_entry_t *entry = token_find_or_create(idx, tfs[i].hash);
        if (!entry) continue;

        sws_token_posting_t *p = (sws_token_posting_t *)arena_alloc(
            &idx->arena, sizeof(sws_token_posting_t));
        if (!p) continue;

        p->doc = doc;
        p->term_freq = tfs[i].tf;
        p->next = entry->postings;
        entry->postings = p;
        entry->doc_freq++;
    }
}

static void token_remove_doc(sws_index_t *idx, sws_doc_t *doc) {
    idx->total_doc_length -= doc->token_count;

    /* Walk all token buckets and remove postings for this doc */
    /* (We don't cache token hashes on doc like trigrams, so full scan) */
    /* For efficiency, re-tokenize to find which entries to clean */
    uint32_t hashes[512];
    uint32_t n = tokenize_text(doc->text, doc->text_len, hashes, 512);
    uint32_t limit = (n < 512) ? n : 512;

    /* Deduplicate */
    uint32_t unique_hashes[512];
    int unique = 0;
    for (uint32_t i = 0; i < limit; i++) {
        int dup = 0;
        for (int j = 0; j < unique; j++) {
            if (unique_hashes[j] == hashes[i]) { dup = 1; break; }
        }
        if (!dup && unique < 512) unique_hashes[unique++] = hashes[i];
    }

    for (int i = 0; i < unique; i++) {
        uint32_t bucket = unique_hashes[i] % SWS_TOKEN_BUCKETS;
        sws_token_entry_t *e = idx->token_buckets[bucket];
        while (e) {
            if (e->token_hash == unique_hashes[i]) {
                sws_token_posting_t **pp = &e->postings;
                while (*pp) {
                    if ((*pp)->doc == doc) {
                        *pp = (*pp)->next;
                        e->doc_freq--;
                        break;
                    }
                    pp = &(*pp)->next;
                }
                break;
            }
            e = e->next;
        }
    }
}

/* ============================================================
 * HNSW Graph Operations (internal)
 * ============================================================ */

/* Simple PRNG for level generation (per-thread would be better, but fine for now) */
static uint32_t hnsw_rng_state = 12345;

static float hnsw_random_float(void) {
    hnsw_rng_state = hnsw_rng_state * 1664525u + 1013904223u;
    return (float)(hnsw_rng_state & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

static uint32_t hnsw_random_level(sws_hnsw_t *g) {
    float r = hnsw_random_float();
    if (r < 1e-9f) r = 1e-9f;
    int level = (int)(-logf(r) * g->ml);
    return (uint32_t)level;
}

/* Get neighbor list offset for a given layer in a node's flat neighbor array.
 * Layout: [count0, n0, n1, ..., count1, n0, n1, ...] */
static uint32_t hnsw_layer_offset(sws_hnsw_t *g, sws_hnsw_node_t *node, uint32_t layer) {
    uint32_t offset = 0;
    for (uint32_t l = 0; l < layer; l++) {
        if (offset >= node->neighbors_size) return offset;
        uint32_t cnt = node->neighbors[offset];
        uint32_t max_conn = (l == 0) ? g->m_max0 : g->m;
        offset += 1 + max_conn;  /* count + max_conn slots */
        (void)cnt;
    }
    return offset;
}

static void hnsw_ensure_layer_space(sws_hnsw_t *g, sws_hnsw_node_t *node, uint32_t level) {
    /* Calculate needed size: for each layer 0..level, need 1 (count) + max_conn slots */
    uint32_t needed = 0;
    for (uint32_t l = 0; l <= level; l++) {
        uint32_t max_conn = (l == 0) ? g->m_max0 : g->m;
        needed += 1 + max_conn;
    }
    if (needed <= node->neighbors_cap) return;

    uint32_t *new_buf = (uint32_t *)realloc(node->neighbors, needed * sizeof(uint32_t));
    if (!new_buf) return;

    /* Zero new space */
    if (needed > node->neighbors_size)
        memset(new_buf + node->neighbors_size, 0, (needed - node->neighbors_size) * sizeof(uint32_t));

    node->neighbors = new_buf;
    node->neighbors_cap = needed;
    node->neighbors_size = needed;
}

static uint32_t hnsw_get_neighbors(sws_hnsw_t *g, uint32_t node_idx, uint32_t layer,
                                    uint32_t *out, uint32_t max_out) {
    sws_hnsw_node_t *node = &g->nodes[node_idx];
    if (layer > node->level) return 0;

    uint32_t offset = hnsw_layer_offset(g, node, layer);
    if (offset >= node->neighbors_size) return 0;

    uint32_t count = node->neighbors[offset];
    if (count > max_out) count = max_out;
    memcpy(out, &node->neighbors[offset + 1], count * sizeof(uint32_t));
    return count;
}

static void hnsw_add_connection(sws_hnsw_t *g, sws_index_t *idx,
                                uint32_t from_idx, uint32_t to_idx, uint32_t layer,
                                const float *target_vec) {
    sws_hnsw_node_t *node = &g->nodes[from_idx];
    hnsw_ensure_layer_space(g, node, node->level);

    uint32_t offset = hnsw_layer_offset(g, node, layer);
    if (offset >= node->neighbors_size) return;

    uint32_t max_conn = (layer == 0) ? g->m_max0 : g->m;
    uint32_t count = node->neighbors[offset];

    /* Check for duplicate */
    for (uint32_t i = 0; i < count; i++) {
        if (node->neighbors[offset + 1 + i] == to_idx) return;
    }

    if (count < max_conn) {
        node->neighbors[offset + 1 + count] = to_idx;
        node->neighbors[offset]++;
    } else if (target_vec && node->doc && node->doc->embedding) {
        /* Prune: replace the farthest neighbor if new connection is closer */
        float new_dist = 1.0f - sws_cosine_similarity(node->doc->embedding, target_vec, idx->dims);
        uint32_t worst_i = 0;
        float worst_dist = 0.0f;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t ni = node->neighbors[offset + 1 + i];
            if (ni >= g->node_count) { worst_i = i; worst_dist = 2.0f; break; }
            sws_hnsw_node_t *nn = &g->nodes[ni];
            if (!nn->doc || !nn->doc->embedding) { worst_i = i; worst_dist = 2.0f; break; }
            float d = 1.0f - sws_cosine_similarity(node->doc->embedding, nn->doc->embedding, idx->dims);
            if (d > worst_dist) { worst_dist = d; worst_i = i; }
        }
        if (new_dist < worst_dist) {
            node->neighbors[offset + 1 + worst_i] = to_idx;
        }
    }
}

/* Distance = 1 - cosine_similarity */
static float hnsw_distance(sws_hnsw_t *g, sws_index_t *idx, uint32_t node_idx, const float *vec) {
    sws_hnsw_node_t *node = &g->nodes[node_idx];
    if (!node->doc || !node->doc->embedding) return 2.0f;
    float sim = sws_cosine_similarity(node->doc->embedding, vec, idx->dims);
    return 1.0f - sim;
}

/* Min-heap candidate for HNSW beam search */
typedef struct {
    uint32_t node_idx;
    float distance;
} hnsw_candidate_t;

/* Simple insertion-sorted candidate list (good enough for ef <= 400) */
typedef struct {
    hnsw_candidate_t *data;
    int count;
    int capacity;
} hnsw_candidate_list_t;

static void clist_init(hnsw_candidate_list_t *cl, int capacity) {
    cl->data = (hnsw_candidate_t *)malloc(capacity * sizeof(hnsw_candidate_t));
    cl->count = 0;
    cl->capacity = capacity;
}

static void clist_free(hnsw_candidate_list_t *cl) {
    free(cl->data);
    cl->data = NULL;
    cl->count = 0;
}

static void clist_insert(hnsw_candidate_list_t *cl, uint32_t node_idx, float dist) {
    /* Find insertion point (sorted ascending by distance) */
    int pos = cl->count;
    while (pos > 0 && cl->data[pos - 1].distance > dist) {
        pos--;
    }

    /* If full and new item is worse than worst, skip */
    if (cl->count >= cl->capacity && pos >= cl->count) return;

    /* Shift right */
    int end = (cl->count < cl->capacity) ? cl->count : cl->capacity - 1;
    for (int i = end; i > pos; i--) {
        cl->data[i] = cl->data[i - 1];
    }

    cl->data[pos].node_idx = node_idx;
    cl->data[pos].distance = dist;
    if (cl->count < cl->capacity) cl->count++;
}

/* Visited set — simple bitmap for nodes */
typedef struct {
    uint8_t *bits;
    uint32_t capacity;
} hnsw_visited_t;

static void visited_init(hnsw_visited_t *v, uint32_t capacity) {
    uint32_t bytes = (capacity + 7) / 8;
    v->bits = (uint8_t *)calloc(bytes, 1);
    v->capacity = capacity;
}

static void visited_free(hnsw_visited_t *v) {
    free(v->bits);
    v->bits = NULL;
}

static int visited_test(hnsw_visited_t *v, uint32_t idx) {
    if (idx >= v->capacity) return 0;
    return (v->bits[idx / 8] >> (idx % 8)) & 1;
}

static void visited_set(hnsw_visited_t *v, uint32_t idx) {
    if (idx >= v->capacity) return;
    v->bits[idx / 8] |= (1 << (idx % 8));
}

/* HNSW search at a single layer — returns candidates sorted by distance.
 * Uses separate candidate queue + result set with processed bitmap.
 * Always processes the nearest unprocessed candidate. */
static void hnsw_search_layer(sws_hnsw_t *g, sws_index_t *idx,
                               const float *query_vec, uint32_t entry_idx,
                               int ef, uint32_t layer,
                               hnsw_candidate_list_t *result) {
    hnsw_visited_t visited;
    visited_init(&visited, g->node_count);

    /* Candidate queue with processed flags */
    int cand_cap = ef * 8 + 32;
    hnsw_candidate_t *candidates = (hnsw_candidate_t *)malloc(cand_cap * sizeof(hnsw_candidate_t));
    uint8_t *processed = (uint8_t *)calloc(cand_cap, 1);
    int cand_count = 0;

    float entry_dist = hnsw_distance(g, idx, entry_idx, query_vec);
    candidates[0].node_idx = entry_idx;
    candidates[0].distance = entry_dist;
    cand_count = 1;
    clist_insert(result, entry_idx, entry_dist);
    visited_set(&visited, entry_idx);

    while (1) {
        /* Find closest unprocessed candidate */
        int best = -1;
        float best_dist = 1e30f;
        for (int i = 0; i < cand_count; i++) {
            if (!processed[i] && candidates[i].distance < best_dist) {
                best_dist = candidates[i].distance;
                best = i;
            }
        }
        if (best < 0) break;

        /* If closest unprocessed is farther than the worst in result set, done */
        if (result->count >= ef && best_dist > result->data[result->count - 1].distance)
            break;

        processed[best] = 1;
        uint32_t cur = candidates[best].node_idx;

        uint32_t neighbors[128];
        uint32_t n_count = hnsw_get_neighbors(g, cur, layer, neighbors, 128);

        for (uint32_t i = 0; i < n_count; i++) {
            uint32_t ni = neighbors[i];
            if (ni >= g->node_count) continue;
            if (visited_test(&visited, ni)) continue;
            visited_set(&visited, ni);

            float d = hnsw_distance(g, idx, ni, query_vec);

            if (result->count < ef || d < result->data[result->count - 1].distance) {
                clist_insert(result, ni, d);
                /* Add to candidate queue */
                if (cand_count < cand_cap) {
                    candidates[cand_count].node_idx = ni;
                    candidates[cand_count].distance = d;
                    cand_count++;
                }
            }
        }
    }

    free(candidates);
    free(processed);
    visited_free(&visited);
}

static void hnsw_insert(sws_index_t *idx, sws_doc_t *doc) {
    sws_hnsw_t *g = &idx->hnsw;
    if (!doc->embedding || idx->dims == 0 || g->m == 0) {
        doc->hnsw_node_idx = UINT32_MAX;
        return;
    }

    /* Grow node array if needed */
    if (g->node_count >= g->node_cap) {
        uint32_t new_cap = g->node_cap * 2;
        if (new_cap < 1024) new_cap = 1024;
        sws_hnsw_node_t *new_nodes = (sws_hnsw_node_t *)realloc(
            g->nodes, new_cap * sizeof(sws_hnsw_node_t));
        if (!new_nodes) { doc->hnsw_node_idx = UINT32_MAX; return; }
        memset(new_nodes + g->node_cap, 0, (new_cap - g->node_cap) * sizeof(sws_hnsw_node_t));
        g->nodes = new_nodes;
        g->node_cap = new_cap;
    }

    uint32_t new_idx = g->node_count;
    uint32_t new_level = hnsw_random_level(g);

    sws_hnsw_node_t *new_node = &g->nodes[new_idx];
    new_node->doc = doc;
    new_node->level = new_level;
    new_node->neighbors = NULL;
    new_node->neighbors_size = 0;
    new_node->neighbors_cap = 0;
    hnsw_ensure_layer_space(g, new_node, new_level);

    doc->hnsw_node_idx = new_idx;
    g->node_count++;

    /* First node */
    if (g->entry_point == UINT32_MAX) {
        g->entry_point = new_idx;
        g->max_level = new_level;
        return;
    }

    /* Greedy search from top to new_level + 1 */
    uint32_t cur = g->entry_point;
    for (int layer = (int)g->max_level; layer > (int)new_level; layer--) {
        /* Greedy walk: find closest at this layer */
        int improved = 1;
        while (improved) {
            improved = 0;
            float cur_dist = hnsw_distance(g, idx, cur, doc->embedding);
            uint32_t neighbors[64];
            uint32_t n_count = hnsw_get_neighbors(g, cur, (uint32_t)layer, neighbors, 64);
            for (uint32_t i = 0; i < n_count; i++) {
                if (neighbors[i] >= g->node_count) continue;
                float d = hnsw_distance(g, idx, neighbors[i], doc->embedding);
                if (d < cur_dist) {
                    cur = neighbors[i];
                    cur_dist = d;
                    improved = 1;
                }
            }
        }
    }

    /* At each layer from min(new_level, max_level) down to 0:
     * beam search to find ef_construction nearest, connect to m best */
    int top = (int)new_level;
    if (top > (int)g->max_level) top = (int)g->max_level;

    for (int layer = top; layer >= 0; layer--) {
        hnsw_candidate_list_t result;
        clist_init(&result, (int)g->ef_construction);

        hnsw_search_layer(g, idx, doc->embedding, cur, (int)g->ef_construction,
                          (uint32_t)layer, &result);

        /* Connect to m nearest */
        uint32_t max_conn = ((uint32_t)layer == 0) ? g->m_max0 : g->m;
        uint32_t to_connect = (uint32_t)result.count;
        if (to_connect > max_conn) to_connect = max_conn;

        for (uint32_t i = 0; i < to_connect; i++) {
            uint32_t neighbor_idx = result.data[i].node_idx;
            if (neighbor_idx == new_idx) continue;

            /* Add bidirectional connection */
            sws_hnsw_node_t *nn = &g->nodes[neighbor_idx];
            const float *nn_emb = (nn->doc && nn->doc->embedding) ? nn->doc->embedding : NULL;
            hnsw_add_connection(g, idx, new_idx, neighbor_idx, (uint32_t)layer, nn_emb);
            hnsw_add_connection(g, idx, neighbor_idx, new_idx, (uint32_t)layer, doc->embedding);
        }

        /* Update entry for next layer */
        if (result.count > 0) cur = result.data[0].node_idx;
        clist_free(&result);
    }

    /* Update entry point if new node is higher level */
    if (new_level > g->max_level) {
        g->entry_point = new_idx;
        g->max_level = new_level;
    }
}

/* ============================================================
 * Add / Remove
 * ============================================================ */

int sws_add(sws_index_t *idx, uint64_t id, const char *text,
            const float *embedding, uint32_t embedding_len) {
    if (!idx || !text) return -1;

    pthread_rwlock_wrlock(&idx->rwlock);

    /* Check for duplicate */
    if (doc_find(idx, id)) {
        pthread_rwlock_unlock(&idx->rwlock);
        return -1;
    }

    /* Create document */
    sws_doc_t *doc = (sws_doc_t *)calloc(1, sizeof(sws_doc_t));
    if (!doc) { pthread_rwlock_unlock(&idx->rwlock); return -1; }

    doc->id = id;
    doc->text_len = (uint32_t)strlen(text);
    doc->text = (char *)malloc(doc->text_len + 1);
    if (!doc->text) { free(doc); pthread_rwlock_unlock(&idx->rwlock); return -1; }
    memcpy(doc->text, text, doc->text_len + 1);
    doc->hnsw_node_idx = UINT32_MAX;

    /* Copy embedding if provided */
    if (embedding && embedding_len > 0 && embedding_len == idx->dims) {
        doc->embedding = (float *)malloc(embedding_len * sizeof(float));
        if (doc->embedding) {
            memcpy(doc->embedding, embedding, embedding_len * sizeof(float));
        }
    }

    /* Extract and index trigrams */
    doc->num_trigrams = extract_trigrams(doc->text, doc->text_len,
                                         doc->trigram_hashes, SWS_MAX_TRIGRAMS);
    for (uint32_t i = 0; i < doc->num_trigrams; i++) {
        trigram_add_posting(idx, doc->trigram_hashes[i], doc);
    }

    /* BM25 token index */
    token_index_doc(idx, doc);

    /* HNSW insert */
    hnsw_insert(idx, doc);

    /* Insert into doc hash table */
    uint32_t bucket = (uint32_t)(id % SWS_DOC_BUCKETS);
    doc->next = idx->doc_buckets[bucket];
    idx->doc_buckets[bucket] = doc;
    idx->doc_count++;

    pthread_rwlock_unlock(&idx->rwlock);
    return 0;
}

int sws_remove(sws_index_t *idx, uint64_t id) {
    if (!idx) return -1;

    pthread_rwlock_wrlock(&idx->rwlock);

    uint32_t bucket = (uint32_t)(id % SWS_DOC_BUCKETS);
    sws_doc_t **pp = &idx->doc_buckets[bucket];

    while (*pp) {
        if ((*pp)->id == id) {
            sws_doc_t *doc = *pp;
            *pp = doc->next;

            /* Remove from trigram index */
            trigram_remove_doc(idx, doc);

            /* Remove from BM25 token index */
            token_remove_doc(idx, doc);

            /* HNSW: mark node as removed (lazy deletion — don't rebuild graph) */
            if (doc->hnsw_node_idx != UINT32_MAX && doc->hnsw_node_idx < idx->hnsw.node_count) {
                idx->hnsw.nodes[doc->hnsw_node_idx].doc = NULL;
            }

            free(doc->text);
            free(doc->embedding);
            free(doc);
            idx->doc_count--;

            pthread_rwlock_unlock(&idx->rwlock);
            return 0;
        }
        pp = &(*pp)->next;
    }

    pthread_rwlock_unlock(&idx->rwlock);
    return -1;
}

/* ============================================================
 * Fuzzy Search (Trigram Jaccard) — now with hash map hit counter
 * ============================================================ */

int sws_fuzzy_search(sws_index_t *idx, const char *query,
                     sws_result_t *results, int limit) {
    if (!idx || !query || !results || limit <= 0) return 0;

    /* Extract query trigrams */
    uint32_t query_hashes[SWS_MAX_TRIGRAMS];
    uint32_t query_len = (uint32_t)strlen(query);
    uint32_t query_trigrams = extract_trigrams(query, query_len,
                                               query_hashes, SWS_MAX_TRIGRAMS);
    if (query_trigrams == 0) return 0;

    pthread_rwlock_rdlock(&idx->rwlock);

    if (idx->doc_count == 0) { pthread_rwlock_unlock(&idx->rwlock); return 0; }

    /* Use hash map for hit counting — O(1) per lookup instead of O(n) scan */
    uint32_t map_cap = idx->doc_count * 4;  /* load factor ~25% */
    if (map_cap < 64) map_cap = 64;
    /* Round up to power of 2 */
    map_cap--;
    map_cap |= map_cap >> 1;
    map_cap |= map_cap >> 2;
    map_cap |= map_cap >> 4;
    map_cap |= map_cap >> 8;
    map_cap |= map_cap >> 16;
    map_cap++;

    sws_hitmap_t hitmap;
    hitmap_init(&hitmap, map_cap);
    if (!hitmap.keys) { pthread_rwlock_unlock(&idx->rwlock); return 0; }

    for (uint32_t q = 0; q < query_trigrams; q++) {
        uint32_t hash = query_hashes[q];
        uint32_t bucket = hash % SWS_TRIGRAM_BUCKETS;
        sws_trigram_entry_t *e = idx->trigram_buckets[bucket];

        while (e) {
            if (e->trigram_hash == hash) {
                sws_posting_t *p = e->postings;
                while (p) {
                    uint32_t *val = hitmap_get_or_insert(&hitmap, p->doc);
                    if (val) (*val)++;
                    p = p->next;
                }
                break;
            }
            e = e->next;
        }
    }

    /* Score and top-K */
    int result_count = 0;

    for (uint32_t i = 0; i < hitmap.capacity; i++) {
        if (hitmap.keys[i] == NULL) continue;
        sws_doc_t *doc = hitmap.keys[i];
        uint32_t hits = hitmap.values[i];

        uint32_t doc_trigrams = doc->num_trigrams;
        uint32_t union_size = query_trigrams + doc_trigrams - hits;
        float score = (union_size > 0) ? (float)hits / (float)union_size : 0.0f;

        /* Insertion sort into results (descending by score) */
        if (result_count < limit || score > results[result_count - 1].score) {
            int pos = (result_count < limit) ? result_count : limit - 1;
            while (pos > 0 && results[pos - 1].score < score) {
                if (pos < limit) results[pos] = results[pos - 1];
                pos--;
            }
            results[pos].id = doc->id;
            results[pos].score = score;
            results[pos].text = doc->text;
            if (result_count < limit) result_count++;
        }
    }

    hitmap_free(&hitmap);
    pthread_rwlock_unlock(&idx->rwlock);
    return result_count;
}

/* ============================================================
 * BM25 Search
 * ============================================================ */

int sws_bm25_search(sws_index_t *idx, const char *query,
                    sws_result_t *results, int limit) {
    if (!idx || !query || !results || limit <= 0) return 0;

    pthread_rwlock_rdlock(&idx->rwlock);

    if (idx->doc_count == 0) { pthread_rwlock_unlock(&idx->rwlock); return 0; }

    /* Tokenize query */
    sws_query_token_t qtokens[SWS_MAX_QUERY_TOKENS];
    int n_qtokens = tokenize_query(query, qtokens, SWS_MAX_QUERY_TOKENS);
    if (n_qtokens == 0) { pthread_rwlock_unlock(&idx->rwlock); return 0; }

    /* BM25 params */
    float k1 = SWS_BM25_K1;
    float b = SWS_BM25_B;
    float N = (float)idx->doc_count;
    float avgdl = (idx->doc_count > 0) ?
                  (float)idx->total_doc_length / (float)idx->doc_count : 1.0f;

    /* Score accumulator (hash map: doc -> score) */
    uint32_t map_cap = idx->doc_count * 4;
    if (map_cap < 64) map_cap = 64;
    map_cap--;
    map_cap |= map_cap >> 1;
    map_cap |= map_cap >> 2;
    map_cap |= map_cap >> 4;
    map_cap |= map_cap >> 8;
    map_cap |= map_cap >> 16;
    map_cap++;

    sws_scoremap_t scores;
    scoremap_init(&scores, map_cap);
    if (!scores.keys) { pthread_rwlock_unlock(&idx->rwlock); return 0; }

    for (int qi = 0; qi < n_qtokens; qi++) {
        uint32_t hash = qtokens[qi].hash;
        uint32_t bucket = hash % SWS_TOKEN_BUCKETS;
        sws_token_entry_t *e = idx->token_buckets[bucket];

        while (e) {
            if (e->token_hash == hash) {
                /* IDF = log(1 + (N - df + 0.5) / (df + 0.5)) */
                float df = (float)e->doc_freq;
                float idf = logf(1.0f + (N - df + 0.5f) / (df + 0.5f));

                sws_token_posting_t *p = e->postings;
                while (p) {
                    if (p->doc) {
                        float tf = (float)p->term_freq;
                        float dl = (float)p->doc->token_count;
                        /* BM25 score for this term in this doc */
                        float tf_score = (tf * (k1 + 1.0f)) /
                                         (tf + k1 * (1.0f - b + b * dl / avgdl));
                        float term_score = idf * tf_score;

                        float *val = scoremap_get_or_insert(&scores, p->doc);
                        if (val) *val += term_score;
                    }
                    p = p->next;
                }
                break;
            }
            e = e->next;
        }
    }

    /* Top-K from score map */
    int result_count = 0;
    for (uint32_t i = 0; i < scores.capacity; i++) {
        if (scores.keys[i] == NULL) continue;
        sws_doc_t *doc = scores.keys[i];
        float score = scores.values[i];

        if (result_count < limit || score > results[result_count - 1].score) {
            int pos = (result_count < limit) ? result_count : limit - 1;
            while (pos > 0 && results[pos - 1].score < score) {
                if (pos < limit) results[pos] = results[pos - 1];
                pos--;
            }
            results[pos].id = doc->id;
            results[pos].score = score;
            results[pos].text = doc->text;
            if (result_count < limit) result_count++;
        }
    }

    scoremap_free(&scores);
    pthread_rwlock_unlock(&idx->rwlock);
    return result_count;
}

/* ============================================================
 * Vector Search (Cosine Similarity) — HNSW if available, else brute-force
 * ============================================================ */

int sws_vector_search(sws_index_t *idx, const float *query_vec,
                      uint32_t query_len, sws_result_t *results, int limit) {
    if (!idx || !query_vec || !results || limit <= 0) return 0;
    if (idx->dims == 0 || query_len != idx->dims) return 0;

    pthread_rwlock_rdlock(&idx->rwlock);

    /* Use HNSW if available and has nodes */
    sws_hnsw_t *g = &idx->hnsw;
    if (g->m > 0 && g->node_count > 0 && g->entry_point != UINT32_MAX) {
        /* HNSW search */
        uint32_t cur = g->entry_point;

        /* Greedy descend from top to layer 1 */
        for (int layer = (int)g->max_level; layer > 0; layer--) {
            int improved = 1;
            while (improved) {
                improved = 0;
                float cur_dist = hnsw_distance(g, idx, cur, query_vec);
                uint32_t neighbors[64];
                uint32_t n_count = hnsw_get_neighbors(g, cur, (uint32_t)layer, neighbors, 64);
                for (uint32_t i = 0; i < n_count; i++) {
                    if (neighbors[i] >= g->node_count) continue;
                    float d = hnsw_distance(g, idx, neighbors[i], query_vec);
                    if (d < cur_dist) {
                        cur = neighbors[i];
                        cur_dist = d;
                        improved = 1;
                    }
                }
            }
        }

        /* Beam search at layer 0 */
        hnsw_candidate_list_t result;
        clist_init(&result, (int)g->ef_search);
        hnsw_search_layer(g, idx, query_vec, cur, (int)g->ef_search, 0, &result);

        /* Convert to results */
        int result_count = 0;
        for (int i = 0; i < result.count && result_count < limit; i++) {
            sws_hnsw_node_t *node = &g->nodes[result.data[i].node_idx];
            if (!node->doc || !node->doc->embedding) continue;
            results[result_count].id = node->doc->id;
            results[result_count].score = 1.0f - result.data[i].distance;  /* cosine sim */
            results[result_count].text = node->doc->text;
            result_count++;
        }

        clist_free(&result);
        pthread_rwlock_unlock(&idx->rwlock);
        return result_count;
    }

    /* Brute-force fallback */
    int result_count = 0;

    for (int b = 0; b < SWS_DOC_BUCKETS; b++) {
        sws_doc_t *d = idx->doc_buckets[b];
        while (d) {
            if (d->embedding) {
                float score = sws_cosine_similarity(query_vec, d->embedding, idx->dims);

                /* Insertion sort (descending) */
                if (result_count < limit || score > results[result_count - 1].score) {
                    int pos = (result_count < limit) ? result_count : limit - 1;
                    while (pos > 0 && results[pos - 1].score < score) {
                        if (pos < limit) results[pos] = results[pos - 1];
                        pos--;
                    }
                    results[pos].id = d->id;
                    results[pos].score = score;
                    results[pos].text = d->text;
                    if (result_count < limit) result_count++;
                }
            }
            d = d->next;
        }
    }

    pthread_rwlock_unlock(&idx->rwlock);
    return result_count;
}

/* ============================================================
 * Hybrid Search (BM25 + Vector, Reciprocal Rank Fusion)
 * ============================================================ */

int sws_hybrid_search(sws_index_t *idx, const char *query,
                      const float *query_vec, uint32_t query_len,
                      float alpha, sws_result_t *results, int limit) {
    if (!idx || !results || limit <= 0) return 0;

    /* Run BM25 and vector searches separately (larger limit for better fusion) */
    int fusion_limit = limit * 3;
    if (fusion_limit < 20) fusion_limit = 20;
    if (fusion_limit > 200) fusion_limit = 200;

    sws_result_t *bm25_results = NULL;
    sws_result_t *vec_results = NULL;
    int n_bm25 = 0, n_vec = 0;

    if (query && query[0]) {
        bm25_results = (sws_result_t *)malloc(fusion_limit * sizeof(sws_result_t));
        if (bm25_results)
            n_bm25 = sws_bm25_search(idx, query, bm25_results, fusion_limit);
    }

    if (query_vec && query_len > 0) {
        vec_results = (sws_result_t *)malloc(fusion_limit * sizeof(sws_result_t));
        if (vec_results)
            n_vec = sws_vector_search(idx, query_vec, query_len, vec_results, fusion_limit);
    }

    /* If only one modality has results, use it directly */
    if (n_bm25 == 0 && n_vec == 0) {
        free(bm25_results);
        free(vec_results);
        return 0;
    }
    if (n_bm25 == 0) {
        int n = (n_vec < limit) ? n_vec : limit;
        memcpy(results, vec_results, n * sizeof(sws_result_t));
        free(bm25_results);
        free(vec_results);
        return n;
    }
    if (n_vec == 0) {
        int n = (n_bm25 < limit) ? n_bm25 : limit;
        memcpy(results, bm25_results, n * sizeof(sws_result_t));
        free(bm25_results);
        free(vec_results);
        return n;
    }

    /* RRF: score = alpha/(k + rank_bm25) + (1-alpha)/(k + rank_vec) */
    const float rrf_k = 60.0f;

    /* Collect all unique doc IDs and their RRF scores */
    /* Use a simple array since fusion_limit is small */
    typedef struct { uint64_t id; float score; const char *text; } rrf_entry_t;
    int max_entries = n_bm25 + n_vec;
    rrf_entry_t *entries = (rrf_entry_t *)calloc(max_entries, sizeof(rrf_entry_t));
    if (!entries) { free(bm25_results); free(vec_results); return 0; }
    int n_entries = 0;

    /* Add BM25 results */
    for (int i = 0; i < n_bm25; i++) {
        float rrf_score = alpha / (rrf_k + (float)(i + 1));
        int found = 0;
        for (int j = 0; j < n_entries; j++) {
            if (entries[j].id == bm25_results[i].id) {
                entries[j].score += rrf_score;
                found = 1;
                break;
            }
        }
        if (!found) {
            entries[n_entries].id = bm25_results[i].id;
            entries[n_entries].score = rrf_score;
            entries[n_entries].text = bm25_results[i].text;
            n_entries++;
        }
    }

    /* Add vector results */
    for (int i = 0; i < n_vec; i++) {
        float rrf_score = (1.0f - alpha) / (rrf_k + (float)(i + 1));
        int found = 0;
        for (int j = 0; j < n_entries; j++) {
            if (entries[j].id == vec_results[i].id) {
                entries[j].score += rrf_score;
                found = 1;
                break;
            }
        }
        if (!found) {
            entries[n_entries].id = vec_results[i].id;
            entries[n_entries].score = rrf_score;
            entries[n_entries].text = vec_results[i].text;
            n_entries++;
        }
    }

    /* Top-K from entries */
    int result_count = 0;
    for (int i = 0; i < n_entries; i++) {
        float score = entries[i].score;
        if (result_count < limit || score > results[result_count - 1].score) {
            int pos = (result_count < limit) ? result_count : limit - 1;
            while (pos > 0 && results[pos - 1].score < score) {
                if (pos < limit) results[pos] = results[pos - 1];
                pos--;
            }
            results[pos].id = entries[i].id;
            results[pos].score = entries[i].score;
            results[pos].text = entries[i].text;
            if (result_count < limit) result_count++;
        }
    }

    free(entries);
    free(bm25_results);
    free(vec_results);
    return result_count;
}

/* ============================================================
 * Persistence (SWS1 + SWS2 binary format)
 * ============================================================ */

/*
 * SWS1 Format:
 *   [4]  magic "SWS1"
 *   [4]  dims
 *   [4]  doc_count
 *   per doc:
 *     [8]  id
 *     [4]  text_len
 *     [N]  text (no null terminator)
 *     [1]  has_embedding (0 or 1)
 *     [dims*4]  embedding (if has_embedding)
 *
 * SWS2 Format (extends SWS1):
 *   [4]  magic "SWS2"
 *   [4]  dims
 *   [4]  doc_count
 *   [4]  hnsw_m
 *   [4]  hnsw_ef_construct
 *   [4]  hnsw_ef_search
 *   per doc: (same as SWS1)
 *   HNSW graph:
 *     [4]  node_count
 *     [4]  entry_point
 *     [4]  max_level
 *     per node:
 *       [8]  doc_id (to re-link)
 *       [4]  level
 *       [4]  neighbors_size
 *       [neighbors_size*4]  neighbors data
 */

int sws_save(sws_index_t *idx, const char *path) {
    if (!idx || !path) return -1;

    pthread_rwlock_rdlock(&idx->rwlock);

    FILE *f = fopen(path, "wb");
    if (!f) { pthread_rwlock_unlock(&idx->rwlock); return -1; }

    int has_hnsw = (idx->hnsw.m > 0 && idx->hnsw.node_count > 0);
    uint32_t magic = has_hnsw ? SWS_MAGIC_V2 : SWS_MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(&idx->dims, 4, 1, f);
    fwrite(&idx->doc_count, 4, 1, f);

    if (has_hnsw) {
        fwrite(&idx->config.hnsw_m, 4, 1, f);
        fwrite(&idx->config.hnsw_ef_construct, 4, 1, f);
        fwrite(&idx->config.hnsw_ef_search, 4, 1, f);
    }

    /* Write docs (build id->node_idx map for HNSW) */
    for (int b = 0; b < SWS_DOC_BUCKETS; b++) {
        sws_doc_t *d = idx->doc_buckets[b];
        while (d) {
            fwrite(&d->id, 8, 1, f);
            fwrite(&d->text_len, 4, 1, f);
            fwrite(d->text, 1, d->text_len, f);

            uint8_t has_emb = (d->embedding != NULL) ? 1 : 0;
            fwrite(&has_emb, 1, 1, f);
            if (has_emb && idx->dims > 0) {
                fwrite(d->embedding, sizeof(float), idx->dims, f);
            }

            d = d->next;
        }
    }

    /* Write HNSW graph */
    if (has_hnsw) {
        sws_hnsw_t *g = &idx->hnsw;
        fwrite(&g->node_count, 4, 1, f);
        fwrite(&g->entry_point, 4, 1, f);
        fwrite(&g->max_level, 4, 1, f);

        for (uint32_t i = 0; i < g->node_count; i++) {
            sws_hnsw_node_t *node = &g->nodes[i];
            uint64_t doc_id = node->doc ? node->doc->id : UINT64_MAX;
            fwrite(&doc_id, 8, 1, f);
            fwrite(&node->level, 4, 1, f);
            fwrite(&node->neighbors_size, 4, 1, f);
            if (node->neighbors_size > 0) {
                fwrite(node->neighbors, sizeof(uint32_t), node->neighbors_size, f);
            }
        }
    }

    fclose(f);
    pthread_rwlock_unlock(&idx->rwlock);
    return 0;
}

sws_index_t *sws_load(const char *path) {
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint32_t magic, dims, doc_count;
    if (fread(&magic, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (magic != SWS_MAGIC && magic != SWS_MAGIC_V2) { fclose(f); return NULL; }
    if (fread(&dims, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&doc_count, 4, 1, f) != 1) { fclose(f); return NULL; }

    int is_v2 = (magic == SWS_MAGIC_V2);

    sws_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.dims = dims;
    cfg.hnsw_m = 16;
    cfg.hnsw_ef_construct = 200;
    cfg.hnsw_ef_search = 50;

    if (is_v2) {
        if (fread(&cfg.hnsw_m, 4, 1, f) != 1) { fclose(f); return NULL; }
        if (fread(&cfg.hnsw_ef_construct, 4, 1, f) != 1) { fclose(f); return NULL; }
        if (fread(&cfg.hnsw_ef_search, 4, 1, f) != 1) { fclose(f); return NULL; }
    }

    /* For V2 with HNSW, we load docs without building HNSW (we'll restore it),
     * but we still need BM25 + trigrams. Use a temp config with m=0 to skip HNSW
     * during sws_add, then restore the graph. */
    sws_config_t load_cfg = cfg;
    if (is_v2) load_cfg.hnsw_m = 0;  /* skip HNSW insert during add */

    sws_index_t *idx = sws_new_with_config(load_cfg);
    if (!idx) { fclose(f); return NULL; }

    for (uint32_t i = 0; i < doc_count; i++) {
        uint64_t id;
        uint32_t text_len;
        if (fread(&id, 8, 1, f) != 1) goto fail;
        if (fread(&text_len, 4, 1, f) != 1) goto fail;

        char *text = (char *)malloc(text_len + 1);
        if (!text) goto fail;
        if (text_len > 0 && fread(text, 1, text_len, f) != text_len) { free(text); goto fail; }
        text[text_len] = '\0';

        uint8_t has_emb;
        if (fread(&has_emb, 1, 1, f) != 1) { free(text); goto fail; }

        float *embedding = NULL;
        if (has_emb && dims > 0) {
            embedding = (float *)malloc(dims * sizeof(float));
            if (!embedding) { free(text); goto fail; }
            if (fread(embedding, sizeof(float), dims, f) != dims) {
                free(embedding); free(text); goto fail;
            }
        }

        int rc = sws_add(idx, id, text, embedding, has_emb ? dims : 0);
        free(text);
        free(embedding);
        if (rc != 0) goto fail;
    }

    /* Restore HNSW graph for V2 */
    if (is_v2) {
        uint32_t node_count, entry_point, max_level;
        if (fread(&node_count, 4, 1, f) != 1) goto fail;
        if (fread(&entry_point, 4, 1, f) != 1) goto fail;
        if (fread(&max_level, 4, 1, f) != 1) goto fail;

        /* Initialize HNSW with correct config */
        idx->config = cfg;
        hnsw_init(&idx->hnsw, &cfg);

        sws_hnsw_t *g = &idx->hnsw;
        g->entry_point = entry_point;
        g->max_level = max_level;

        /* Grow node array */
        if (node_count > g->node_cap) {
            sws_hnsw_node_t *new_nodes = (sws_hnsw_node_t *)realloc(
                g->nodes, node_count * sizeof(sws_hnsw_node_t));
            if (!new_nodes) goto fail;
            memset(new_nodes + g->node_cap, 0, (node_count - g->node_cap) * sizeof(sws_hnsw_node_t));
            g->nodes = new_nodes;
            g->node_cap = node_count;
        }
        g->node_count = node_count;

        for (uint32_t i = 0; i < node_count; i++) {
            uint64_t doc_id;
            uint32_t level, neighbors_size;
            if (fread(&doc_id, 8, 1, f) != 1) goto fail;
            if (fread(&level, 4, 1, f) != 1) goto fail;
            if (fread(&neighbors_size, 4, 1, f) != 1) goto fail;

            sws_hnsw_node_t *node = &g->nodes[i];
            node->level = level;
            node->neighbors_size = neighbors_size;
            node->neighbors_cap = neighbors_size;

            /* Link to doc */
            if (doc_id != UINT64_MAX) {
                node->doc = doc_find(idx, doc_id);
                if (node->doc) node->doc->hnsw_node_idx = i;
            } else {
                node->doc = NULL;
            }

            if (neighbors_size > 0) {
                node->neighbors = (uint32_t *)malloc(neighbors_size * sizeof(uint32_t));
                if (!node->neighbors) goto fail;
                if (fread(node->neighbors, sizeof(uint32_t), neighbors_size, f) != neighbors_size) goto fail;
            } else {
                node->neighbors = NULL;
            }
        }
    }

    fclose(f);
    return idx;

fail:
    fclose(f);
    sws_free(idx);
    return NULL;
}

/* ============================================================
 * Info
 * ============================================================ */

uint32_t sws_count(sws_index_t *idx) {
    if (!idx) return 0;
    pthread_rwlock_rdlock(&idx->rwlock);
    uint32_t c = idx->doc_count;
    pthread_rwlock_unlock(&idx->rwlock);
    return c;
}

void sws_info(sws_index_t *idx, sws_info_t *info) {
    if (!idx || !info) return;
    memset(info, 0, sizeof(sws_info_t));

    pthread_rwlock_rdlock(&idx->rwlock);

    info->doc_count = idx->doc_count;
    info->token_count = idx->token_count;
    info->trigram_count = idx->trigram_count;
    info->dims = idx->dims;
    info->hnsw_levels = idx->hnsw.max_level + 1;
    info->hnsw_nodes = idx->hnsw.node_count;

    /* Estimate memory */
    info->memory_bytes = sizeof(sws_index_t);
    info->memory_bytes += idx->arena.total_allocated;

    /* Doc memory */
    for (int b = 0; b < SWS_DOC_BUCKETS; b++) {
        sws_doc_t *d = idx->doc_buckets[b];
        while (d) {
            info->memory_bytes += sizeof(sws_doc_t) + d->text_len + 1;
            if (d->embedding) info->memory_bytes += idx->dims * sizeof(float);
            d = d->next;
        }
    }

    /* HNSW memory */
    for (uint32_t i = 0; i < idx->hnsw.node_count; i++) {
        info->memory_bytes += sizeof(sws_hnsw_node_t);
        info->memory_bytes += idx->hnsw.nodes[i].neighbors_cap * sizeof(uint32_t);
    }

    pthread_rwlock_unlock(&idx->rwlock);
}
