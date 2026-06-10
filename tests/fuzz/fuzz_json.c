/*
 * fuzz_json.c — fuzz BOTH of swarmrt's independent JSON decoders.
 *
 * swarmrt has two completely separate JSON decode implementations:
 *
 *   1. Interpreter path: sw_lang_json_decode(const char *s)
 *      swarmrt_lang.h:316 / lang.c:5341 — depth-capped via g_jd_depth,
 *      uses the _jd_parse family. Returns sw_val_nil() on any error.
 *
 *   2. Compiled path: _builtin_json_decode(sw_val_t **a, int n)
 *      swarmrt_builtins_studio.h:5082 — depth-capped via g_json_depth +
 *      g_json_abort, uses the _json_parse / _json_parse_hex4 family.
 *      Returns sw_val_nil() on any error (same failure signal).
 *
 * Both are reachable from untrusted input (any sw program calling
 * json_decode/json_get with user-supplied JSON). We hunt:
 *   - Memory-safety bugs (OOB reads/writes, UAF, stack overflow on deeply-
 *     nested input). ASAN+UBSAN is the primary oracle — any sanitizer fault
 *     exits non-zero and CI fails.
 *   - Divergence: one parser accepting input the other rejects suggests an
 *     inconsistency in the language surface. Reported as a WARNING on stderr
 *     (not abort) because a handful of edge cases (trailing garbage, NUL
 *     in string) may be legitimately implementation-defined. We count them.
 *
 * Build + run via `make fuzz-json` (see Makefile).
 * For coverage-guided fuzzing on a host with libFuzzer:
 *     clang -fsanitize=fuzzer,address,undefined -Isrc ... fuzz_json.c RT...
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Pull in both parsers. Include order matches generated .gen.c preamble. */
#include "swarmrt_native.h"
#include "swarmrt_lang.h"
#include "swarmrt_varena.h"
#include "swarmrt_ets.h"
#include "swarmrt_builtins_studio.h"

/* Divergence counter — printed at exit in standalone mode. */
static int g_divergences = 0;

/*
 * Helper: "failed" means sw_val_nil() returned.
 * Both parsers use the same signal so the differential is clean.
 */
static int is_nil(sw_val_t *v) {
    return !v || v->type == SW_VAL_NIL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Both parsers take NUL-terminated C strings. */
    char *src = (char *)malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    /* --- Parser 1: interpreter path (sw_lang_json_decode) --- */
    sw_val_t *r1 = sw_lang_json_decode(src);
    int failed1 = is_nil(r1);

    /* --- Parser 2: compiled path (_builtin_json_decode) --- */
    /* Build the single sw_val_t* string argument the builtin expects. */
    sw_val_t *sv = sw_val_string(src);
    sw_val_t *args[1];
    args[0] = sv;
    sw_val_t *r2 = _builtin_json_decode(args, 1);
    int failed2 = is_nil(r2);

    /* --- Differential check --- */
    if (failed1 != failed2) {
        g_divergences++;
        /* Log but don't abort — some edge cases may be implementation-defined
         * (trailing garbage, embedded NUL). Real bugs show as ASAN faults. */
        fprintf(stderr, "[fuzz_json] divergence #%d: interp=%s compiled=%s input(%zu)=",
                g_divergences, failed1 ? "nil" : "ok", failed2 ? "nil" : "ok", size);
        for (size_t i = 0; i < (size < 80 ? size : 80); i++) {
            unsigned char c = (unsigned char)src[i];
            if (c >= 0x20 && c < 0x7f) fputc(c, stderr);
            else fprintf(stderr, "\\x%02x", c);
        }
        if (size > 80) fprintf(stderr, "...");
        fputc('\n', stderr);
    }

    free(src);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Standalone driver (see fuzz_standalone.h for the replay+mutate loop).
 * Print divergence count at exit so CI can flag unexpected divergences. */
#ifdef SW_FUZZ_STANDALONE

#include "fuzz_standalone.h"

/* fuzz_standalone.h defines main(); we override atexit to print the count. */
static void print_divergences(void) {
    if (g_divergences > 0)
        fprintf(stderr, "[fuzz_json] total divergences: %d (see above)\n", g_divergences);
}

/* Hook into the standalone main via a constructor attribute. */
__attribute__((constructor)) static void register_atexit(void) {
    atexit(print_divergences);
}

#endif /* SW_FUZZ_STANDALONE */
