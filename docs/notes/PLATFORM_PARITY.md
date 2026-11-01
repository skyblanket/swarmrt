# SwarmRT Cross-Platform Parity Report

**Date:** 2026-03-12
**Baseline:** macOS (ARM64 + x86_64) — fully working
**Targets:** Linux (x86_64 + ARM64), Windows (x86_64 via MinGW-w64)

---

## Summary

| Category | Linux | Windows (MinGW-w64) |
|----------|-------|---------------------|
| Core runtime (scheduler, actors, messaging) | **Works** | **Does not compile** — `swarmrt_native.c` still uses POSIX signals, `sigaltstack`, `execinfo.h` (see T2) |
| IO system (TCP, event loop) | **Works** (epoll) | **FIXED** — WSAPoll event loop |
| Assembly context switch | **Works** (x86_64 + ARM64) | **FIXED** — Windows x64 ABI + XMM regs |
| HTTP/WebSocket server | **Works** (needs OpenSSL) | **Does not compile** — needs a MinGW OpenSSL (see T2, T3) |
| Desktop automation | **Works** (uinput + X11/Wayland) | **Works** (SendInput + Win32) |
| Clipboard | **Works** (wl-copy/xclip) | **Works** (Win32 API) |
| Search/SIMD | **Works** (SSE + NEON) | **Works** (MinGW uses same intrinsics) |
| Compiler (swc) | **Works** | swc's own units compile; `swc.exe` cannot link until the core runtime does. `swc add/install/...` and `swc new` are stubs on Windows (#50) |
| FS Index (sws) | **Works** | **Works** — cross-compiled `sws.exe` indexes and searches under wine (`make win-xcheck`, CI `windows-cross`) |
| Build system | **Works** (Makefile) | `cmake -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake` configures and builds `sws`; the core library stops at the units in T2 |

**Linux: Fully functional.**
**Windows: not buildable yet.** The MinGW-w64 cross-compile was run on 2026-11-01
(`make win-xcheck`, also the CI job `windows-cross`): 25 of the 29 units of
libswarmrt, swc and sws compile and `sws.exe` runs under wine, but four core units
do not compile (T2), so no sw program and no `swc.exe` can be built for Windows.
The "FIXED" entries below are the code changes made in March; they were never
compiled until this check, and the units in T2 show they were incomplete.

---

## Completed Fixes

### Platform Abstraction Layer — `swarmrt_platform.h` (NEW)
Cross-platform header providing unified API:
- OS/arch detection macros (`SW_OS_WINDOWS/MACOS/LINUX`, `SW_ARCH_X86_64/ARM64`)
- `sw_tmpdir()` — `/tmp` vs `GetTempPath()`
- `sw_homedir()` — `HOME` vs `USERPROFILE`
- `sw_popen/sw_pclose/sw_getpid_os` — popen vs _popen
- `sw_random_u32()/sw_random_uniform()` — arc4random vs rand_s
- `sw_fd_t` / `sw_close_socket` / `sw_socket_read/write` — BSD sockets vs Winsock
- `sw_set_nonblocking()` — fcntl vs ioctlsocket
- `sw_wake_pipe_create()` — pipe() vs loopback TCP socket pair
- `sw_mmap_anon()/sw_munmap()` — mmap vs VirtualAlloc
- `sw_thread_yield()` — sched_yield vs SwitchToThread
- `sw_preempt_timer_t` — Windows timer queue for preemption

### swarmrt_native.c — Arena + Preemption
- `#ifdef _WIN32` platform includes (windows.h/process.h vs POSIX)
- Arena allocator: VirtualAlloc + MEM_COMMIT|MEM_RESERVE on Windows
- Stack allocation: VirtualAlloc + VirtualProtect(PAGE_NOACCESS) for guard page
- Stack/arena cleanup: VirtualFree on Windows
- sysconf(_SC_PAGESIZE) → hardcoded 4096 on Windows
- Preemption timer: CreateTimerQueue + CreateTimerQueueTimer on Windows
- preempt_handler: WAITORTIMERCALLBACK signature on Windows
- sigaction guarded with `#ifndef _WIN32`

### swarmrt_io.c — 3-Platform Event Loop
- Full rewrite with kqueue (macOS), epoll (Linux), WSAPoll (Windows)
- Winsock init/cleanup, loopback socket pair for wake pipe
- recv()/send() on Windows, read()/write() on POSIX

### swarmrt_asm.S — Windows x64 ABI
- System V section guarded with `!_WIN32`
- Windows x64 section: args in rcx/rdx, saves rdi/rsi + xmm6-xmm15
- Shadow space (32 bytes) before calls in trampoline
- No leading underscore on Windows symbols

### swarmrt_context.S — Windows x64 ABI
- Same treatment as swarmrt_asm.S

### swc.c — Compiler CLI
- `/tmp` → `sw_tmpdir()`
- `mkstemps` → Windows fallback path
- `dirname()` → manual separator search on Windows
- `unlink()` → `_unlink()` on Windows

### swarmrt_builtins_studio.h — Studio Builtins
- `arc4random()/arc4random_uniform()` → `sw_random_u32()/sw_random_uniform()`
- `/tmp` → `sw_tmpdir()`
- `getpid()` → `sw_getpid_os()`
- `popen()/pclose()` → `sw_popen()/sw_pclose()`
- `WEXITSTATUS()` → direct status on Windows
- `mkdir()` → `_mkdir()` on Windows
- `unlink()` → `_unlink()` on Windows
- `sleep()` → `Sleep()` on Windows

### swarmrt_fsindex.c — FS Search CLI
- HOME/getpwuid → `sw_homedir()` from platform.h
- `lstat()` → `stat()` on Windows
- `sigaction()` → `signal()` on Windows
- `sleep()` → `Sleep()` on Windows
- `fnmatch()` → simple glob implementation for Windows

### swarmrt_agent.c — Legacy Agent
- `ucontext.h` → `#ifndef _WIN32` guard
- `swapcontext()` → `SwitchToFiber()` placeholder on Windows

### swarmrt_simple.c
- `pthread_yield_np()/pthread_yield()` → `sched_yield()` (all platforms)

---

## Remaining TODO

### T1. CMakeLists.txt for Cross-Platform Build — files exist
Done: `CMakeLists.txt` and `mingw-w64.cmake` are in the repo. `cmake -S . -B <build>` configures on
Linux (checked 2026-10-19). Covers the MinGW-w64 toolchain file, `.S` assembly support (GAS), and
platform link flags (`ws2_32` on Windows, pthread/zlib elsewhere). The cross-compile itself is
unverified until T2 runs.

### T2. Windows Cross-Compile — run; four units blocked
Run it: `make win-xcheck` (needs `gcc-mingw-w64-x86-64` and `libz-mingw-w64-dev`, plus
`wine64` to run `sws.exe`). CI: the `windows-cross` job runs it with the compiler
required. Last result (2026-11-01, MinGW-w64 GCC 13, wine 9.0):

| Unit | State | First blockers |
|---|---|---|
| `swarmrt_asm.S`, otp, task, ets, phase4, phase5, hotload, io, gc, pdf, varena | compile | — |
| swc units (`swc.c`, codegen, obfusc, repl, test, lsp, battery_interp, new, swc_pkg, swc_imports) | compile | — |
| `swarmrt_desktop.c`, `swarmrt_clipboard.c` | compile | not run (need a real desktop) |
| `swarmrt_fsindex.c`, `swarmrt_search.c` → `sws.exe` | compile, link, **run under wine** (index + search) | — |
| `swarmrt_native.c` | **does not compile** | `sigaction`/`sigaltstack`/`siginfo_t`/`stack_t` (crash and SIGPIPE handlers; needs SEH / `SetUnhandledExceptionFilter`), `execinfo.h` `backtrace` (needs `CaptureStackBackTrace`), `sysconf`, `usleep` |
| `swarmrt_node.c` | **does not compile** | `arpa/inet.h` (needs `winsock2.h`/`ws2tcpip.h`), OpenSSL headers |
| `swarmrt_lang.c` | **does not compile** | `sys/wait.h`, `fork`/`waitpid` (needs `CreateProcess`), SQLite headers |
| `swarmrt_http.c` | **does not compile** | OpenSSL (`SHA1`, TLS) |

Dependencies: Ubuntu packages MinGW zlib, but not MinGW OpenSSL or SQLite. A Windows
build needs them from MSYS2 (`mingw-w64-x86_64-openssl`, `-sqlite3`) or vendored.
Unverified: everything that needs `swarmrt_native.c`, i.e. the scheduler, the
`swarmrt_asm.S` Windows x64 context switch at runtime, preemption timers, and every
sw program.

### T3. HTTP Server — Winsock Testing
Unverified, and blocked on T2: `swarmrt_http.c` and `swarmrt_node.c` do not compile
for Windows yet. `swarmrt_io.c` (the WSAPoll event loop) compiles but has never run.
Edge cases to check once it links: `SO_REUSEADDR` semantics (Windows allows port
stealing; use `SO_EXCLUSIVEADDRUSE`), `TCP_NODELAY`, `WSAEWOULDBLOCK` vs `EAGAIN`
in `sw_tcp_send` (callers check `errno`), and non-blocking `connect`.

### T4. Agent.c — Windows Fiber Integration
Not on any build path: `swarmrt_agent.c` / `swarmrt_full.c` are legacy and are not
part of libswarmrt, swc or sws (they include `ucontext.h` and do not compile for
Windows). The runtime switches contexts with `swarmrt_asm.S`, which has a Windows
x64 path that compiles but has not run. Recommendation: drop the fiber placeholder
rather than finish it.

### T5. Platform gaps that are loud errors, not silent ones
- `shell_managed` returns `{-1, "error: shell_managed unsupported on this platform"}`
  on Windows (process groups and `setsid` have no direct equivalent; a Job Object
  port is the path).
- `swc add/install/update/remove/deps` and `swc new` print "not supported on Windows
  yet" and exit 1 (#50). The code compiles for Windows; enabling it means replacing
  `fork`/`execvp` of git with `_spawnvp`/`CreateProcess`, and `realpath` with
  `_fullpath`. It cannot be verified until `swc.exe` links (T2).
- Linux mouse position (`sw_desktop_get_mouse_pos`) shells out to `xdotool` and
  returns `SWD_ERR_PLATFORM` when it is missing.

---

## Build Instructions

### macOS (current)
```bash
make swc sws
```

### Linux
```bash
make swc sws  # needs: gcc, libz-dev, libssl-dev
```

### Windows (MinGW-w64 cross-compile from macOS/Linux)
```bash
cmake -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake -B build-win .
cmake --build build-win --target sws   # builds today; the core library stops at T2
make win-xcheck                        # per-unit status + sws.exe under wine
```
