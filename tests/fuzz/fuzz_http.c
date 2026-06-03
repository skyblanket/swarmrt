/*
 * fuzz_http.c — fuzz the HTTP request-header parser with arbitrary bytes.
 *
 * The HTTP server now delivers parsed request headers to sw handlers (a
 * lowercased-key MAP in {'http_request', ...} and via ws_request_headers).
 * Header parsing is a classic memory-safety surface: split on CRLF, scan
 * for a colon, copy name/value into bounded buffers. This target drives
 * sw_http_fuzz_parse() — the request-line sscanf + find_header_end +
 * http_parse_headers path in src/swarmrt_http.c (compiled with
 * -DSW_FUZZ_HTTP) — over arbitrary bytes, hunting OOB reads, missing NUL
 * terminators, and unbounded copies.
 *
 * A NULL/empty parse is fine; we only care about memory safety.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* From swarmrt_http.c (only compiled when SW_FUZZ_HTTP is defined). */
void sw_http_fuzz_parse(const char *buf, uint32_t len);

#include "fuzz_standalone.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The parser scans with strstr/strchr/sscanf, so it needs a NUL
     * terminator one past the bytes (conn_on_data guarantees this in the
     * real server). */
    char *buf = (char *)malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    sw_http_fuzz_parse(buf, (uint32_t)size);

    free(buf);
    return 0;
}
