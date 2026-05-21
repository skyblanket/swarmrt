# Known issues

Tracked publicly because users will hit them. Each has a repro and a
hypothesis; PRs welcome.

---

## SIGSEGV on shutdown after `main()` returns (reported Linux x86_64, NOT reproducible on current main)

**Status:** Could not reproduce on current main (`eda85db` and later)
across 50 stress runs of the 100k-spawn microbench on Ubuntu 24.04
x86_64 (Docker, emulated under macOS arm64). Valgrind memcheck on a
1k-spawn run was also clean. Original reviewer hit this 2/3 runs on
real Ubuntu 24.04 hardware against an older `main` commit.

**Likely outcome:** one or more of the v2/v3/v4 codegen fixes landed
2026-05-21 incidentally closed the race. Plausible candidates:

- **Parser:** `_ -> { ... }` clause body now parses as block, not tuple
  (pre-fix, certain receive arms emitted malformed C that could
  scribble over scheduler state in obscure cases).
- **Codegen:** lambda-local vars no longer get treated as outer-scope
  captures (`scan_lambdas` now consults `collect_assigned_names`).
  Pre-fix, captures could include uninitialised pointers handed to the
  closure registry — a candidate for "ghost pointer" UAF.
- **Codegen:** `_` placeholder uniqueness in destructure patterns
  (pre-fix, two `_` in one pattern emitted `sw_val_t *_ = ...;
  sw_val_t *_ = ...;` — UB in C, value depended on compiler).

We can't be 100% sure without re-running the exact failing version
on the reviewer's hardware. If you hit this again, please report
with:
- `uname -a` (kernel + arch)
- `cc --version` (compiler)
- exact swarmrt commit hash
- `valgrind --tool=memcheck` output if you can capture it
- `ldd ./your_program` to see which libc you've got

**Repro (no longer fires):** the spawn-100k microbench.

```sw
module Bench
export [main]

fun child(parent) { send(parent, 'done') ; 'ok' }
fun spawn_n(n) {
    if (n == 0) { 'ok' } else { spawn(child(self())) ; spawn_n(n - 1) }
}
fun await_n(n) {
    if (n == 0) { 'ok' } else { receive { 'done' -> await_n(n - 1) } }
}
fun main() {
    n = 100000
    t0 = timestamp()
    spawn_n(n); await_n(n)
    print(f"spawned + joined {n} procs in {timestamp() - t0} ms")
}
```

**Originally observed (commit `927cb30` or older, real Ubuntu 24.04):**

```
spawned + joined 100000 procs in 864 ms
[SwarmRT] CRASH: SIGSEGV at address (nil)
Backtrace: (empty)
```

**Now observed (commit `eda85db`+, Ubuntu 24.04 in Docker):**

```
$ for i in $(seq 1 50); do ./sbench >/dev/null; echo $?; done | sort -u
0
```

50/50 clean exits. Average run time ~580 ms.

---
