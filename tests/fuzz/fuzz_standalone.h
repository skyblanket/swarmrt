/*
 * fuzz_standalone.h — a tiny driver so our fuzz targets run anywhere.
 *
 * Each target defines:
 *     int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
 *
 * Build modes:
 *
 *   1. libFuzzer (coverage-guided) — needs a clang with the fuzzer runtime
 *      (Linux clang, or `brew install llvm` on macOS):
 *          clang -fsanitize=fuzzer,address,undefined fuzz_parse.c ... -o fuzz_parse
 *          ./fuzz_parse corpus/parse
 *      libFuzzer supplies main(); we compile ours out (SW_FUZZ_STANDALONE unset).
 *
 *   2. Standalone (no libFuzzer) — works with stock Apple clang under
 *      ASAN/UBSAN. The Makefile defines SW_FUZZ_STANDALONE, so this file
 *      supplies a main() that:
 *        - replays every file passed on argv (recursing into directories),
 *        - then runs SW_FUZZ_ITERS (default 20000) deterministic xorshift-
 *          mutated variants of the seed inputs — a poor-man's fuzzer that
 *          finds crashes, just not coverage-guided.
 *      A crash trips ASAN/UBSAN and aborts non-zero, so this doubles as a
 *      CI memory-safety gate over the seed corpus.
 */
#ifndef SW_FUZZ_STANDALONE_H
#define SW_FUZZ_STANDALONE_H

#include <stdint.h>
#include <stddef.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#ifdef SW_FUZZ_STANDALONE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define _FZ_MAX_SEEDS 4096
#define _FZ_MAX_LEN   (1u << 20)   /* 1 MB per input */

static uint8_t *_fz_seeds[_FZ_MAX_SEEDS];
static size_t   _fz_seed_lens[_FZ_MAX_SEEDS];
static int      _fz_nseeds = 0;

/* Deterministic PRNG — fixed seed so CI reproduces exactly. */
static uint64_t _fz_state = 0x9e3779b97f4a7c15ULL;
static uint64_t _fz_rand(void) {
    uint64_t x = _fz_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return (_fz_state = x);
}

static void _fz_add_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (size_t)n > _FZ_MAX_LEN || _fz_nseeds >= _FZ_MAX_SEEDS) { fclose(f); return; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    _fz_seeds[_fz_nseeds] = buf;
    _fz_seed_lens[_fz_nseeds] = got;
    _fz_nseeds++;
    /* Replay the seed itself immediately (corpus regression check). */
    LLVMFuzzerTestOneInput(buf, got);
}

static void _fz_add_path(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *e;
        char child[4096];
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            _fz_add_path(child);
        }
        closedir(d);
    } else {
        _fz_add_file(path);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) _fz_add_path(argv[i]);

    /* Always exercise the empty input + a couple of degenerate ones. */
    LLVMFuzzerTestOneInput((const uint8_t *)"", 0);

    if (_fz_nseeds == 0) {
        /* No corpus — read one input from stdin so the binary is still
         * useful as `./fuzz_x < someblob`. */
        static uint8_t buf[_FZ_MAX_LEN];
        size_t n = fread(buf, 1, sizeof(buf), stdin);
        LLVMFuzzerTestOneInput(buf, n);
        return 0;
    }

    long iters = 20000;
    const char *env = getenv("SW_FUZZ_ITERS");
    if (env) iters = strtol(env, NULL, 10);

    /* Mutate seeds: pick a seed, copy it, apply a few random byte ops. */
    uint8_t *scratch = (uint8_t *)malloc(_FZ_MAX_LEN + 1);
    for (long it = 0; it < iters; it++) {
        int si = (int)(_fz_rand() % (uint64_t)_fz_nseeds);
        size_t len = _fz_seed_lens[si];
        if (len > _FZ_MAX_LEN) len = _FZ_MAX_LEN;
        memcpy(scratch, _fz_seeds[si], len);
        int nmut = 1 + (int)(_fz_rand() % 8);
        for (int m = 0; m < nmut && len > 0; m++) {
            uint64_t r = _fz_rand();
            switch (r % 4) {
                case 0: scratch[r % len] ^= (uint8_t)(r >> 8); break;       /* bit/byte flip */
                case 1: scratch[r % len] = (uint8_t)(r >> 16); break;        /* set byte */
                case 2: if (len > 1) { len = (size_t)(r % len) + 1; } break; /* truncate */
                case 3: scratch[r % len] = (uint8_t)(0xff); break;          /* sentinel byte */
            }
        }
        LLVMFuzzerTestOneInput(scratch, len);
    }
    free(scratch);
    for (int i = 0; i < _fz_nseeds; i++) free(_fz_seeds[i]);
    printf("standalone fuzz: replayed %d seeds + %ld mutations, no crash\n",
           _fz_nseeds, iters);
    return 0;
}
#endif /* SW_FUZZ_STANDALONE */
#endif /* SW_FUZZ_STANDALONE_H */
