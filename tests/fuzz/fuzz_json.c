/*
 * fuzz_json.c — fuzz the JSON decoder with arbitrary bytes.
 *
 * sw_lang_json_decode() is the agent-facing input boundary: every LLM
 * tool-call response, every http_get body an agent parses, flows through
 * here. It must not crash, over-read, or stack-overflow on malformed or
 * adversarially-nested input (the depth guard g_jd_depth bounds nesting;
 * this proves it under fuzzing). A nil/partial return on bad input is fine
 * — we only hunt memory-safety bugs.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* From swarmrt_lang.h. The decoded value is a global-heap sw_val_t tree
 * (no process arena in the harness); free it recursively so 20k iterations
 * don't accumulate into an OOM (detect_leaks=0 would hide it otherwise). */
struct sw_val;
void *sw_lang_json_decode(const char *s);
void sw_val_free(struct sw_val *v);

#include "fuzz_standalone.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *src = (char *)malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    /* Decoder must be memory-safe on any input (no crash, no over-read,
     * no stack overflow — the depth guard bounds nesting). */
    struct sw_val *v = (struct sw_val *)sw_lang_json_decode(src);
    sw_val_free(v);

    free(src);
    return 0;
}
