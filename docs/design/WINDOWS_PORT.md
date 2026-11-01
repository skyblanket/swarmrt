# Windows port: what is verified and the path to a build (#50, #51)

Status: the cross-compile has been run and is gated in CI. Windows does not
build yet. The per-unit table is in `docs/notes/PLATFORM_PARITY.md` (T2).

## What was verified from Linux (2026-11-01)

The toolchain was MinGW-w64 GCC 13 (`gcc-mingw-w64-x86-64`, posix threads),
MinGW zlib, and wine 9.0.

- **25 of 29 units compile.** That covers libswarmrt, swc and sws.
- **`sws.exe` runs.** It links statically, and under wine it indexes a
  directory and finds the expected hit.
- **CMake.** `cmake -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake` configures and
  builds `sws`.
- **Two units fixed in this step.** Both compiled on Linux but not for
  Windows:
  - `swarmrt_lsp.c` assigned to `stderr`. It now redirects fd 2 instead.
  - `swarmrt_fsindex.c` called `mkdir(path, mode)`, but MinGW's `mkdir`
    takes no mode.
- **Gate.** `make win-xcheck` and the CI job `windows-cross` keep all of
  the above from regressing. Units that are not ported yet are listed with
  their blockers, and the script tells you when one starts compiling.

## What blocks a Windows build

| Unit | Work | Size |
|---|---|---|
| `swarmrt_native.c` | Replace the signal-based crash and SIGPIPE handling with SEH (`SetUnhandledExceptionFilter` / vectored handlers) and the alternate signal stack with the guard-page handler. Replace `backtrace` with `CaptureStackBackTrace`, `sysconf` with `GetSystemInfo`, and `usleep` with `Sleep`. | Large. It is the scheduler core and is being changed heavily on main. |
| `swarmrt_lang.c` | Process builtins (`shell`, `exec_*`): `fork`/`waitpid` become `CreateProcess` + `WaitForSingleObject`. Also SQLite headers. | Large |
| `swarmrt_node.c` | Winsock headers and `WSAStartup` (io.c already has both), OpenSSL. | Small once OpenSSL exists |
| `swarmrt_http.c` | OpenSSL | Small once OpenSSL exists |
| Dependencies | OpenSSL and SQLite for MinGW are not in Ubuntu's archive. | CI choice, see below |

Only after these build can anyone check the things that need the runtime
to actually run:

- the Windows x64 context switch in `swarmrt_asm.S`;
- WSAPoll IO;
- preemption timers;
- the Winsock edge cases (T3);
- any sw program;
- `swc.exe` itself.

## Options for the dependencies and the CI leg

- **A. MSYS2 on a `windows-latest` runner (recommended).** Install
  `pacman -S mingw-w64-x86_64-{gcc,openssl,sqlite3,zlib}`, then
  `make swc libswarmrt` with a Windows branch in the Makefile, then run
  the tests natively.
  - **For:** real Windows, with real Winsock and SEH behaviour. That is
    what T3 needs.
  - **Against:** about 5 minutes of package setup per run, and
    Windows-specific Makefile work.
- **B. Cross-compile on Linux with vendored OpenSSL and SQLite,** run under
  wine.
  - **For:** cheap, and it extends today's `windows-cross` job.
  - **Against:** wine is not Windows (SEH, Winsock and timer behaviour
    differ). Vendoring OpenSSL is a maintenance cost.
- **C. Drop Windows and keep WSL as the answer.**
  - **For:** honest and cheap.
  - **Against:** gives up the native desktop-automation and clipboard
    work that already compiles.

Recommendation: keep B's compile gate, which landed in this step, as the
fast regression net. Add A as the real Windows leg once `swarmrt_native.c`
is ported, because before that it can only fail.

## #50 (package manager and `swc new`)

The code compiles for Windows. The Windows branches are deliberate stubs
that print "not supported on Windows yet" and exit 1. Enabling them means
three changes:

- run git with `_spawnvp` / `CreateProcess` instead of `fork`/`execvp`,
  and keep the argv-only, no-shell property the security audit relies on;
- replace `realpath` with `_fullpath`;
- copy templates with `FindFirstFile` (or MinGW's `dirent`, which is
  present).

None of that can be run until `swc.exe` links (the table above), so it is
deferred rather than shipped untested.

## Decisions for the maintainer

1. The cross-compile gate is per unit: known-blocked units are listed, not
   hidden. A blocked unit that starts compiling is reported so the list
   gets tightened.
2. The CI job `windows-cross` runs on the same trigger as the macOS leg
   (any non-docs change) and is required, not report-only.
3. README now says "Windows does not build yet" instead of "best-effort".
4. The `SwitchToFiber` placeholder in the legacy `swarmrt_agent.c` (T4) is
   not on any build path. Recommendation: delete it rather than finish it.
5. #50 stays open, blocked on the core-runtime port.
