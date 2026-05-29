/*
 * fuzz_marshal.c — fuzz the distribution deserializer with arbitrary bytes.
 *
 * sw_unmarshal(buf, len, node) turns a wire blob back into an sw_val_t.
 * It runs on bytes received from a remote node over TCP — untrusted
 * network input — which is exactly the surface hardened in swarmrt_node.c
 * (frame length caps + overflow checks). This target feeds it random and
 * mutated bytes to confirm it never reads out of bounds, integer-overflows
 * a length, or recurses without bound on a hostile nested value.
 *
 * Pure deserializer: no scheduler, no context switching, ASAN-clean.
 */
#include <stdint.h>
#include <stddef.h>

/* From swarmrt_node.h — declared as void* here so the harness needs no
 * runtime headers; the result is passed through opaquely. */
void *sw_unmarshal(const uint8_t *buf, uint32_t len, const char *default_remote_node);

#include "fuzz_standalone.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 0xFFFFFFFFu) return 0;       /* len is uint32_t */
    /* The result (if any) is leaked deliberately — we run with
     * detect_leaks=0; the target is memory safety, not leak accounting. */
    void *v = sw_unmarshal(data, (uint32_t)size, "fuzz");
    (void)v;
    return 0;
}
