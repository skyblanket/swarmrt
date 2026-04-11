# SwarmRT Codebase Audit Report

**Date**: 2026-03-14
**Scope**: Full codebase — runtime, MCP server, build system, security, performance
**Codebase**: ~15,000 lines C across 54 source files + 2,900-line MCP server

---

## Executive Summary

SwarmRT is an ambitious BEAM-inspired C runtime with genuinely impressive engineering in its arena allocator, lock-free messaging, and SIMD-accelerated search. The **native runtime** (`swarmrt_native.c`) is production-quality with sub-microsecond process spawning and correct concurrent data structures. The **MCP server** is feature-rich (27 tools, workspace orchestration, BM25 search, autopilot) but has critical command injection vulnerabilities. The **process subsystem prototype** (`swarmrt_proc.c`) has a non-functional GC. Test coverage and CI are minimal.

**Finding Totals**: 13 Critical, 10 High, 12 Medium, 9 Low

---

## 1. Critical Findings (13)

### 1.1 Core Runtime

| # | File | Line | Issue | Impact |
|---|------|------|-------|--------|
| C1 | `swarmrt.c` | `sw_yield()` | `longjmp` to same `jmp_buf` — fundamentally broken context switching | Undefined behavior, stack corruption |
| C2 | `swarmrt_gc.c` | compaction | Mark-compact moves words without updating pointers | Dangling pointers after any GC cycle |
| C3 | `swarmrt_proc.c` | `sw_proc_yield()` | Function is a NO-OP — processes never actually yield | No preemptive scheduling in proc subsystem |

### 1.2 HTTP/WebSocket

| # | File | Line | Issue | Impact |
|---|------|------|-------|--------|
| C4 | `swarmrt_http.c` | 159 | WebSocket `payload_len` cast from `uint64_t` to `uint32_t` — integer overflow on >4GB payloads | Heap overflow via memcpy |
| C5 | `swarmrt_http.c` | 172 | Unbounded `malloc(payload_len + 1)` with no NULL check | NULL deref or OOM DOS |
| C6 | `swarmrt_http.c` | 40-75 | HTTP connection struct accessed after mutex unlock — race condition | Use-after-free, data corruption |

### 1.3 Language Parser

| # | File | Line | Issue | Impact |
|---|------|------|-------|--------|
| C7 | `swarmrt_lang.c` | 1587-1592 | Unbounded `strcat()` on 1024-byte stack buffer during string concatenation | Stack buffer overflow → RCE |
| C8 | `swarmrt_lang.c` | 387-420 | String interpolation overflow — no size check on AST node `sval` field | Heap corruption |

### 1.4 MCP Server (Command Injection — 7 locations)

| # | File | Line | Issue | Impact |
|---|------|------|-------|--------|
| C9 | `swarmrt_mcp.c` | 1958 | `tool_git_diff`: `ref` param passed to shell via `popen` — single quotes not escaped | Arbitrary command execution |
| C10 | `swarmrt_mcp.c` | 1999 | `tool_git_log`: `path` param injected into shell command | Arbitrary command execution |
| C11 | `swarmrt_mcp.c` | 2365 | `tool_workspace_create`: `name` param in `git worktree` command | Arbitrary command execution |
| C12 | `swarmrt_mcp.c` | 2534 | `tool_checkpoint_save`: `label` param in `git commit -m` | Arbitrary command execution |
| C13 | `swarmrt_mcp.c` | 2601 | `tool_checkpoint_restore`: `ref` param in `git reset --hard` | Arbitrary command execution + data destruction |

**Recommended fix for C9-C13**: Sanitize all shell arguments — reject characters outside `[a-zA-Z0-9._@:/-]`, or replace `popen()` with `fork()+execvp()` to avoid shell interpretation entirely.

---

## 2. High Findings (10)

### 2.1 Security

| # | File | Line | Issue |
|---|------|------|-------|
| H1 | `swarmrt_node.c` | 66-109 | Remote message deserialization: `payload_len` from network used in `malloc+memcpy` without upper bound — DOS or heap overflow via integer wrap |
| H2 | `swarmrt_mcp.c` | 1838 | `tool_codebase_grep`: regex error message (`errbuf`) injected raw into JSON via `%s` — broken JSON output |
| H3 | `swarmrt_mcp.c` | 1891, 2344 | Multiple MCP tools: user-supplied strings in error responses use `%s` instead of `jb_append_escaped` — malformed JSON |
| H4 | `swarmrt_mcp.c` | 2648 | `tool_workspace_diff`: mutex released before running git commands — workspace can be archived concurrently |
| H5 | `swarmrt_mcp.c` | 2428 | `tool_workspace_list`: `popen()` called while holding mutex — git hang causes deadlock |
| H6 | Multiple | — | Missing NULL checks after `malloc`/`realloc` in proc, HTTP, and lang modules |

### 2.2 Performance

| # | File | Line | Issue |
|---|------|------|-------|
| H7 | `swarmrt_search.c` | 119-123 | O(n²) trigram deduplication — linear scan for each of up to 512 trigrams |
| H8 | `swarmrt_search.c` | 183-198 | O(n²) query token deduplication — same linear scan pattern |

### 2.3 Build/Test

| # | Issue |
|---|-------|
| H9 | No tests run in CI — `release-mcp.yml` only builds, never tests. Ships without verification. |
| H10 | MCP server (2,900 lines) has zero test coverage |

---

## 3. Medium Findings (12)

### 3.1 Security

| # | File | Issue |
|---|------|-------|
| M1 | `swarmrt_mcp.c` | `checkpoint_save` runs `git add -A && git commit` silently — adds secrets, binaries, .env files |
| M2 | `swarmrt_mcp.c` | `checkpoint_restore` runs `git reset --hard` — destroys uncommitted work without confirmation |
| M3 | `swarmrt_mcp.c` | No input validation on workspace names — path traversal with `../` possible |
| M4 | `swarmrt_lang.c:176` | Signed int `i` as array index in lexer number parsing — overflow on huge number literals |
| M5 | `swarmrt_native.c` | No resource limits on timers, monitors, or message queue depth per process |

### 3.2 Performance

| # | File | Issue |
|---|------|-------|
| M6 | `swarmrt_mcp.c:2112` | `fgetc()` per character to count lines in `codebase_overview` — should use buffered `fread()` |
| M7 | `swarmrt_proc.c:133-188` | `copy_term_recursive()` does deep copy with malloc per binary — missing ref-counted large binary optimization |
| M8 | `swarmrt_ets.c:24` | Only 64 hash buckets — no dynamic resizing. Chain length grows linearly with entries. |
| M9 | `swarmrt_proc.c:415-446` | Work stealing scans ALL schedulers linearly — should use randomized victim selection |

### 3.3 Build/Test

| # | Issue |
|---|-------|
| M10 | `make test-all` only runs v1/v2/proc/native — phases 2-10, OTP, and search tests not included |
| M11 | `Makefile` MCP target uses `-march=native` — built binaries won't run on older CPUs |
| M12 | 13 compiled binaries committed to repo root (counter_test, hello_test, ets_test, etc.) |

---

## 4. Low Findings (9)

| # | File | Issue |
|---|------|-------|
| L1 | `swarmrt_proc.c:198-209` | GC is a no-op — heap only grows, never shrinks. Long-lived processes leak memory. |
| L2 | `swarmrt_ets.c:60` | Float equality uses `==` — IEEE 754 means `0.1+0.2 != 0.3` |
| L3 | `swarmrt_native.c:443` | macOS preemption timer has empty handler — only Linux actually preempts via SIGALRM |
| L4 | `swarmrt_arena.h` | Missing bounds checks on partition push operations |
| L5 | `swarmrt_proc.c` | Atom table has no overflow handling — unbounded growth |
| L6 | `swarmrt_mcp.c` | Inconsistent error response format — some tools return `"Error: ..."`, others `{"error":"..."}` |
| L7 | `swarmrt_mcp.c` | No signal handling — SIGTERM/SIGINT don't save session state |
| L8 | `swarmrt_lang.c:383` | No recursion depth limit in string interpolation parser |
| L9 | Repo root | `--version` directory created by accidental `./swc --version`; stale `2/` directory |

---

## 5. Architecture Assessment

### What's Excellent

1. **Arena Allocator** (`swarmrt_native.c`): Single `mmap` for all process memory. Per-scheduler partitioned free stacks with spinlocks. Contiguous PID ranges for hardware prefetcher locality. This is genuinely novel — eliminates ALL syscalls from the spawn hot path. Sub-microsecond process creation.

2. **Lock-free Mailbox** (`swarmrt_native.h`): Vyukov MPSC signal stack (atomic CAS) for cross-scheduler message delivery + private FIFO queue for sequential consumption. Correct memory ordering. Matches Erlang BEAM's recent migration from mutex-based to lock-free mailboxes.

3. **SIMD Search** (`swarmrt_search.c`): Production-quality ARM64 NEON and x86_64 AVX+FMA vectorized cosine similarity with 4x unrolling. BM25 inverted index with arena-allocated posting lists. Binary persistence for fast startup.

4. **MCP Server Design** (`swarmrt_mcp.c`): Clean tool registry with function pointers. JSON-RPC protocol handling is solid. Session context with BM25 search gives AI memory. Autopilot state machine with JSON persistence. Conductor-inspired workspace/worktree orchestration is architecturally sound. 27 tools in a single 2,900-line C file with zero dependencies.

5. **Per-thread Freelists** (`swarmrt_native.c`): Message and timer freelists avoid malloc/free on hot paths. 128-entry capacity with overflow to heap. Simple and effective.

### What Needs Work

1. **Two Incompatible Process Models**: `swarmrt_proc.c` (mutex-based, term types) and `swarmrt_native.c` (lock-free, arena-backed) are completely separate implementations with different type systems, scheduling, and messaging. The proc subsystem appears to be an earlier prototype. Recommend deprecating it or clearly marking it as experimental.

2. **GC**: The proc subsystem's GC is a no-op. The native subsystem's GC (`swarmrt_gc.c`) has a broken compactor that creates dangling pointers. Neither is usable in production. This is the biggest gap — without working GC, long-running processes will exhaust memory.

3. **Test Infrastructure**: No test framework — each test is its own `main()` with printf assertions. No pass/fail summary, no CI pipeline for runtime tests. Coverage is concentrated on happy-path scenarios; no adversarial/fuzzing tests.

4. **Shell Safety in MCP**: All git/workspace tools use `snprintf` + `popen`, passing user input through the shell. This is the #1 security priority to fix — either sanitize inputs aggressively or use `fork()`+`execvp()`.

---

## 6. Recommended Fix Priority

### P0 — Fix Before Any External Use
1. **MCP command injection** (C9-C13): Add `sanitize_shell_arg()` that rejects `'`, `"`, `;`, `$`, `` ` ``, `|`, `&`, `(`, `)`, newlines
2. **WebSocket integer overflow** (C4-C5): Cap `payload_len` at 16MB, check `malloc` return
3. **HTTP connection race** (C6): Hold mutex across entire connection use, or use per-connection locks
4. **String concat overflow** (C7): Replace `strcat` with `strncat` or dynamic buffer

### P1 — Fix Before Production
5. **Remote message validation** (H1): Cap `payload_len` at reasonable maximum (e.g., 64MB)
6. **MCP JSON escaping** (H2-H3): Use `jb_append_escaped` for all user-visible strings in error paths
7. **GC implementation** (C2, L1): Either implement proper mark-sweep or document that processes are short-lived only
8. **CI test pipeline** (H9): Add GitHub Actions workflow that runs `make test-all` on push

### P2 — Improve Quality
9. **Search deduplication** (H7-H8): Replace O(n²) linear scan with hash set
10. **ETS bucket count** (M8): Increase to 256+ or add dynamic resizing
11. **Work stealing** (M9): Randomize victim selection
12. **Clean repo** (M12, L9): Add binaries to `.gitignore`, remove stale directories

---

## 7. Code Statistics

| Component | Files | Lines | Test Files | Test Coverage |
|-----------|-------|-------|------------|---------------|
| Core Runtime (native) | 6 | ~3,500 | 3 | Partial |
| Core Runtime (proc) | 2 | ~1,100 | 3 | Partial |
| OTP Behaviours | 4 | ~2,000 | 5 | Partial |
| Language Frontend | 2 | ~2,500 | 1 | Minimal |
| Search Engine | 2 | ~1,500 | 1 | Good |
| MCP Server | 1 | 2,900 | 0 | **None** |
| HTTP/WebSocket | 1 | ~500 | 0 | **None** |
| Node/Distribution | 1 | ~400 | 1 | Minimal |
| GC | 1 | 277 | 1 | Broken impl |
| Other (IO, PDF, etc.) | 8 | ~2,500 | 0 | **None** |

---

*Report generated during SwarmRT MCP autopilot audit session.*
