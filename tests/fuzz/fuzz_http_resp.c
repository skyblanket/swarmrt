/*
 * fuzz_http_resp.c — fuzz the native HTTP client's response parser.
 *
 * The native client (src/swarmrt_http_native.c) reads whatever a server sends
 * back: status line, header block, then Content-Length, chunked or
 * read-to-EOF framing. That is untrusted input with length fields, hex sizes
 * and line scanning, so it is a memory-safety surface. This target drives
 * sw_http_native_fuzz_parse() (the same parser the socket path uses, fed from
 * memory) over arbitrary bytes, hunting OOB reads, unbounded growth and
 * integer overflow in the framing arithmetic.
 *
 * A status of 0 (malformed or truncated reply) is expected for most inputs;
 * we only care about memory safety.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* From swarmrt_http_native.c. */
int sw_http_native_fuzz_parse(const uint8_t *buf, size_t len);

#include "fuzz_standalone.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sw_http_native_fuzz_parse(data, size);
    return 0;
}
