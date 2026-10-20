# attic/

Legacy prototypes, kept for reference only. Nothing here is built by the
Makefile: plain `make` builds `bin/swc` and `bin/libswarmrt.a` from `src/`.

- `swarmrt_simple.c`, `main.c`, `parser.c`, `benchmark.c` — the original
  "v1" prototype (`swarmrt-v1`), with stubbed linking and `run`.
- `swarmrt.c` — an early runtime prototype with TODO stubs.
- `swarmrt_proc.c`, `swarmrt_context.S`, `test_proc*.c` — the "proc"
  prototype (`swarmrt-proc`), where `sw_proc_gc` is a no-op.
- `test_runner.c` — a small test driver for the v1 prototype.

Headers stay in `src/` where live code still includes them. If you want to
revive one of these, move it back and add a Makefile target for it.
