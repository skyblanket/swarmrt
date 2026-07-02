/*
 * fuzz_ws.c — fuzz the WebSocket frame decoder with arbitrary bytes.
 *
 * Drives sw_ws_fuzz_frames() (src/swarmrt_http.c, compiled with
 * -DSW_FUZZ_HTTP), which feeds the input through ws_try_parse exactly as
 * conn_on_data would after a WS upgrade: frame-header decode (7/16/64-bit
 * payload lengths), mask handling, the 16MB payload/reassembly caps, the
 * control frames (close/ping/pong), and the fragmentation buffer's
 * realloc-growth path. Hunting OOB reads on truncated headers, integer
 * trouble around the 64-bit length, and reassembly-buffer accounting bugs.
 *
 * Delivery targets are NULL under fuzz (handler + port), so parse safety —
 * not messaging — is the surface. Each input is treated as one connection's
 * inbound byte stream (state fully reset between inputs).
 */
#include <stdint.h>
#include <stddef.h>

/* From swarmrt_http.c (only compiled when SW_FUZZ_HTTP is defined). */
void sw_ws_fuzz_frames(const uint8_t *data, uint32_t len);

#include "fuzz_standalone.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1u << 20)) return 0;   /* match the harness's 1MB input cap */
    sw_ws_fuzz_frames(data, (uint32_t)size);
    return 0;
}
