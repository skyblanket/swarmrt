/*
 * fuzz_parse.c — fuzz the sw lexer/parser with arbitrary bytes.
 *
 * sw_lang_parse() takes a NUL-terminated source string and returns a
 * module AST (or NULL on parse error). It's a pure function — no
 * scheduler, no context switching — so it's clean under ASAN/UBSAN and
 * the obvious first fuzz target: every .sw file a user (or an LLM) writes
 * flows through here.
 *
 * We're hunting memory-safety bugs (OOB reads in the lexer, unbounded
 * recursion / stack overflow in the parser, UB on malformed UTF-8 or
 * unterminated strings), NOT logic bugs — a NULL return is fine.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* From swarmrt_lang.h */
void *sw_lang_parse(const char *source);

#include "fuzz_standalone.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* sw_lang_parse wants a NUL-terminated C string. */
    char *src = (char *)malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    /* Parser must not crash on any input. Result (AST or NULL) is
     * intentionally discarded — we only care about memory safety. */
    void *ast = sw_lang_parse(src);
    (void)ast;

    free(src);
    return 0;
}
