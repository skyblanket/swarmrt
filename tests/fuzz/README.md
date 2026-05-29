# Fuzzing SwarmRT

Two fuzz targets cover the code paths that take **untrusted input**:

| Target | Entry point | Why it matters |
|---|---|---|
| `fuzz_parse` | `sw_lang_parse(const char *)` | every `.sw` file a human or an LLM writes flows through the lexer/parser |
| `fuzz_marshal` | `sw_unmarshal(buf, len, node)` | every byte received from a remote node over TCP (the distribution deserializer) |

Both functions are **pure** — no scheduler, no `swarmrt_asm.S` context switching —
so they're clean under AddressSanitizer + UndefinedBehaviorSanitizer. (A
full-runtime ASAN build false-positives on the fiber stack swaps, which is
why we fuzz these in isolation.)

## Quick run (any box, stock clang)

```bash
make fuzz            # build both with ASAN+UBSAN, replay corpus + 20k mutations each
make fuzz-parse
make fuzz-marshal
SW_FUZZ_ITERS=1000000 make fuzz-parse   # longer soak
```

The standalone driver (`fuzz_standalone.h`) replays every file in the seed
corpus, then runs `SW_FUZZ_ITERS` (default 20000) deterministic
xorshift-mutated variants. A memory-safety violation trips ASAN/UBSAN and
aborts non-zero, so this doubles as a CI regression gate. The seed is fixed,
so runs reproduce exactly.

## Coverage-guided (libFuzzer)

Stock Apple clang ships no libFuzzer runtime. Use a real LLVM clang
(`brew install llvm`, or Linux clang) and build without `SW_FUZZ_STANDALONE`:

```bash
clang -fsanitize=fuzzer,address,undefined -fno-stack-protector -I../../src -Itests/fuzz \
  tests/fuzz/fuzz_parse.c $(core .c sources) -o fuzz_parse -lz -lsqlite3 -lm
./fuzz_parse -max_len=65536 tests/fuzz/corpus/parse
```

libFuzzer supplies its own `main()`; `fuzz_standalone.h` compiles its driver
out when `SW_FUZZ_STANDALONE` is undefined.

## Corpus

- `corpus/parse/` — real examples (hello, counter, json_pipeline) plus
  adversarial seeds: empty, unterminated string, deep `(((…)))` nesting,
  unbalanced braces, invalid UTF-8.
- `corpus/marshal/` — valid marshalled blobs (nil/int/string/atom/list) plus
  adversarial ones: oversized length/count claims (`\x04\xff\xff\xff\xff`),
  truncated values, unknown type tags.

## Bugs found

The first run of each target found a real memory-safety bug:

- **`fuzz_marshal`** — `unmarsh_val` checked `*pos + len > buflen` with an
  attacker-controlled 32-bit length. A length near `UINT32_MAX` overflowed
  the addition, passed the check, then `memcpy`'d ~4 GB out of bounds. Fixed
  by switching every bound to overflow-safe subtraction (`buflen - *pos < n`).
- **`fuzz_parse`** — a string literal ending in a lone backslash at EOF made
  the lexer's escape handler `ladv()` past the NUL terminator (heap overflow
  read). Fixed by stopping when the backslash is the final byte.

Add a target by writing `tests/fuzz/fuzz_<area>.c` that defines
`LLVMFuzzerTestOneInput`, `#include "fuzz_standalone.h"`, and a matching
Makefile target.
