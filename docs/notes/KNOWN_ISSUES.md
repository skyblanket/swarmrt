# Known issues

Tracked publicly because users will hit them. Each open issue should have
a repro, impact, and current hypothesis.

## Open

These are genuine limitations, not crashes. Each is reproducible with the
shipped `bin/swc`.

### Compiled `receive` has no default timeout (interpreter/compiled divergence)

A bare `receive` with no `after` clause blocks forever in a compiled binary
(codegen emits an infinite wait), whereas the interpreter defaults to a 5s
timeout. The compiled behavior is the correct Erlang-style selective receive;
the divergence is the issue.

**Impact:** code that relies on the interpreter's implicit 5s timeout will hang
when compiled. **Workaround:** add an explicit `after MS -> ...` clause to any
`receive` that might not match, so compiled and interpreted runs behave the same.

### No static type or shape checking

`sw` is dynamically typed by design — there is no compile-time type or arity
checking. A typo'd variable name compiles cleanly and becomes an atom at
runtime instead of erroring:

```sw
print(undefined_var)   # compiles; prints :undefined_var
```

**Impact:** name typos surface as silent runtime atoms rather than compile
errors. This is a deliberate tradeoff (matching the dynamic, Erlang-shaped
model), recorded here so the behavior is not a surprise.

## Recently cleared

### No `swc run` subcommand

Cleared. `swc run file.sw` exists (parse → codegen → `cc` → run the resulting
binary in one shot; see `run_file` in `src/swc.c`). The interpreter path
(`swc run`, REPL, `swc test`) shares the runtime's builtins with the compiled
path.

### Multi-head cons patterns are unimplemented

Cleared on 2026-06-03. The list-pattern parser now accepts any number of
leading heads before the bar, so `[a, b | rest]` (and `[a, b, c | rest]`,
etc.) parse into a right-nested cons chain `cons(a, cons(b, rest))`. This
matches a list of length >= 2, binding `a`/`b` to the first two elements
and `rest` to the remainder — identically in the interpreter and the
compiler (verified by `tests/sw/test_patterns_codegen.sw`). Construction
position (`[a, b | rest]` building a list) works too.

### High-process-count spawn/send stress crash

Cleared on 2026-05-29 after re-testing the 80k-spawn send/receive stress
bench on native Linux x86_64:

- Host: `sushi`, Ubuntu 24.04, Linux 6.17, AMD EPYC 9554, 128 CPUs.
- Default multi-scheduler variant: 50/50 completed, 0/50 crashed.
- `SW_SCHEDULERS=1` variant: 50/50 completed, 0/50 crashed.

The stress gate now defaults to a strict threshold: every run in both
variants must print `ok 80000`. Lower thresholds can still be supplied
manually with `SW_STRESS_THRESHOLD` for exploratory bisects, but CI and
normal reviewer runs should treat any crash as a regression.
