# Contributing to SwarmRT

Thanks for taking the time. SwarmRT is a C runtime + an ahead-of-time
compiler for the `sw` language; contributions to either half are welcome.

## Getting set up

```bash
git clone https://github.com/skyblanket/swarmrt && cd swarmrt
# System libraries (see README "Quickstart" for the full list):
#   Ubuntu/Debian: sudo apt-get install build-essential libsqlite3-dev libssl-dev zlib1g-dev
#   macOS:         brew install sqlite openssl@3 zlib
make swc libswarmrt
./bin/swc build examples/counter.sw -o /tmp/counter && /tmp/counter
```

## Running the tests

Before opening a PR, run what CI runs:

```bash
make test-sw                       # sw-language suite — 50 files (compiled + interpreter + swc-run paths)
for p in 2 3 4 5 6 7 8 9 10; do make test-phase$p; done   # C-side runtime tests (phases 2–10)
make stress                        # high-process-count race guard (Linux x86_64)
```

When iterating on a builtin or adding tests under `tests/sw/repl/`, you
can run a single interpreter-path test directly without rebuilding
everything:

```bash
swc test tests/sw/repl/my_test.sw
```

The `linux-quickstart` workflow gates every push and PR on all of the
above. If it's red, the change isn't ready — a red `make test-phase*`
or a stress run that doesn't achieve 100 % completion (all 50 multi-sched
and all 50 single-sched runs) blocks merge.

`make clean` removes every build artefact, including the compiled `.sw`
programs that test targets leave at the repo root.

## Code style

- **C** — match the surrounding file: 4-space indent, K&R braces,
  comments that explain *why* rather than *what*. The runtime is
  concurrent and lock-sensitive; if a change touches the scheduler,
  arena, or mailbox path, say so in the PR and explain the ordering.
- **sw** — brace-delimited, see [docs/SW_LANGUAGE.md](docs/SW_LANGUAGE.md).
  New language features need a `tests/sw/test_<topic>.sw` (and, where it
  applies, a `tests/sw/repl/` counterpart) so both the compiled and
  interpreter paths stay in sync.
- Keep the compiler and the tree-walking interpreter at parity — a
  builtin added to `swarmrt_builtins_studio.h` + codegen should also be
  reachable from `swarmrt_lang.c`.

## Commits & pull requests

- One logical change per PR. Describe what changed and why.
- Reference the issue it closes.
- Note any new platform assumptions or system-library dependencies.
- New runtime behaviour belongs in [docs/CHANGELOG.md](docs/CHANGELOG.md).

## Where things live

| Area | Path |
|---|---|
| Runtime (scheduler, arena, mailboxes, GC, distribution) | `src/swarmrt_*.c` |
| Compiler (`swc`) — lexer, parser, codegen | `src/swarmrt_lang.c`, `src/swarmrt_codegen.c`, `src/swc.c` |
| Builtins exposed to `sw` | `src/swarmrt_builtins_studio.h` |
| Standard library (sw modules) | `lib/` |
| Tests | `tests/` |
| Docs | `docs/` |
| Open bugs & design notes | `docs/notes/` |

There are no open known runtime issues right now
([docs/notes/KNOWN_ISSUES.md](docs/notes/KNOWN_ISSUES.md) tracks them).
The external review history is in
[docs/notes/REVIEW_HARDENING.md](docs/notes/REVIEW_HARDENING.md) if you
want background on past hardening work.

## License

By contributing you agree your work is licensed under the
[MIT License](LICENSE).
