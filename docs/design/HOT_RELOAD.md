# sw-level hot code reload (#37)

Status: design only, recommendation below. Nothing in this step changes
the runtime or the compiler (see "Why no prototype in this step").

## Where we are

- `swarmrt_hotload.{c,h}` is a C-embedder registry.
  `sw_module_register(name, fn)` and `sw_module_upgrade(name, fn2)` re-point
  a named slot to another C function that is already linked into the binary,
  bump its version, keep the previous version for rollback, and send
  `SW_TAG_CODE_CHANGE` to processes spawned with `sw_module_spawn`. It never
  loads new code.
- `swc` compiles every module of a program into one C file and links it
  statically against `libswarmrt.a`. Each sw function becomes a `static`
  C function such as `Hr_worker`, with `__b` (body) and `__rs` (resume)
  variants.
- Three kinds of runtime state point into the program's machine code:
  - stack frames on the carrier stacks: return addresses into function
    bodies;
  - spilled heap frames (`sw_frame_t`): their resume function pointer
    (`Hr_worker__rs`) and metadata (`Hr_worker__meta`, in `.rodata`);
  - tail-call thunks and closure values: a function pointer each.
- Literal strings and atoms are copied (`val_strdup`) or interned into
  runtime memory. A value never points into a module's `.rodata`.

So "reload" needs three things that do not exist today:

1. a way to get new machine code into a running process;
2. a call path that can reach the new code;
3. a rule for when the old code may go away.

## Options

### A. Reloadable module as a shared object, with Erlang's two-version rule (recommended)

- `swc build --reloadable Mod` (or a `@reloadable` module attribute)
  compiles `Mod` into `Mod.<hash>.so` with `-fPIC`. The host binary is
  linked `-rdynamic`, so the `.so` resolves runtime symbols such as
  `sw_val_*`, `sw_send` and `sw_frame_new` against the host. There is one
  runtime and one heap.
- The `.so` exports one symbol, `sw_module_exports_<Mod>`. It is a table
  of `{name, arity, fn}` entries, plus an ABI stamp (swc version, runtime
  version, `sizeof(sw_val_t)`, frame layout version). A mismatched stamp is
  a load error.
- **Call semantics (Erlang's).**
  - A fully-qualified call `Mod.f(x)` from any module, including from
    `Mod` itself, goes through `Mod`'s slot: one atomic load of the
    current table, then an indirect call.
  - Unqualified local calls stay direct calls inside the same `.so`.
  - So a process running `loop(state)` stays on the old version until it
    makes a qualified call `Mod.loop(state)`. That is exactly how an
    Erlang server picks up new code, and it gives the programmer the
    switch-over point.
- **Builtins.**
  - `code_load(Mod, path)` → `{'ok', vsn}` | `{'error', reason}`. It
    dlopens the file, checks the stamp, and swaps the slot. The previous
    version becomes "old". It also sends `{'code_change', Mod, old_vsn,
    new_vsn}` to processes registered for the module, reusing the
    `sw_module_*` tracking.
  - `code_which(Mod)` → `vsn`.
  - `code_purge(Mod)` → `'ok'` | `{'error', 'in_use'}`.
- **Two versions at most.** Loading a third version requires the old one
  to be purged first, as in Erlang.
- **When old code may be unmapped.**
  - Never in step 1: `dlclose` is not called. The leak is bounded by the
    number of reloads, and it removes every use-after-unmap hazard (frames,
    thunks, closures and meta pointers all stay valid).
  - Later: each version keeps a count of live heap frames and closures
    created by its code (`sw_frame_new` and closure construction already
    receive a meta pointer, so they can carry the version too), plus the
    set of processes currently executing it.
  - `code_purge` then kills those processes, as Erlang's `purge` does, or
    refuses (`soft_purge`). It unmaps only when the count reaches zero.
- **State migration** is the user's code, as in Erlang. The server's
  receive loop handles `{'code_change', ...}` by calling
  `Mod.loop(Mod.migrate(state))`. A GenServer-style helper can wrap that.

### B. Whole-binary re-exec ("blue/green in place")

`code_upgrade(path)` checkpoints durable state (`durable_checkpoint`),
`execve`s the new binary with its listening sockets inherited by fd, and
the new image restores.

- **For:** sound, small, works for any change, including runtime changes.
- **Against:** it is a restart, not hot reload. Every process, mailbox
  and in-flight call is lost unless the program checkpoints it. It is
  worth having anyway as "zero-downtime restart", but it does not answer
  #37.

### C. Interpreted reloadable modules

Load `.sw` source at runtime and run it on the REPL's tree-walking
interpreter.

- **For:** no compiler changes.
- **Against:**
  - The interpreter does not support spawn, receive or the scheduler
    primitives (README "REPL"), which are exactly what long-running
    reloadable servers use.
  - Semantics and performance would split between reloaded and compiled
    code.

### D. Patch code in place

Rewrite function prologues or a global trampoline per function.

- **Against:** unsound with frames suspended mid-function (spilled
  `__rs` frames resume at a label inside the old body), and impossible
  with W^X text.

## Recommendation

A, in three steps. Each step is shippable and testable.

1. **Compiler and runtime plumbing, no unloading.**
   - Add `swc build --shared-module Mod`, the export table plus ABI stamp,
     and `-rdynamic` host linking.
   - Route qualified calls to reloadable modules through the slot.
   - Add `code_load` and `code_which`, and the `{'code_change', ...}`
     message.
   - Old versions are never unmapped.
   - Test: a server in a reloadable module answers `1`. `code_load` a
     rebuilt `.so` that answers `2`. The server answers `2` after its
     next qualified self-call, and a local-call-only loop keeps
     answering `1`.
2. **Purge.** Per-version frame and closure counts, then `code_purge`
   and `soft_purge`. Under `SW_SUSPEND_TORTURE` and gc-stress, a purge
   never unmaps code with a live frame.
3. **Tooling.** `swc reload <running-node> Mod` over distribution, so it
   needs `SW_NODE_TLS` (#40), and `code_change` support in the OTP
   helpers.

B can ship independently as "zero-downtime restart".

## Why no prototype in this step

Every sound step of A touches `swarmrt_codegen.c`: qualified-call
dispatch, the export table, and making the `static` functions of a
reloadable module reachable. It also touches swc's link line. Those files
are being changed in parallel by other work right now. A "prototype" that
dlopens a hand-built `.so` without the codegen side would only show that
`dlopen` works. It would not show reload semantics, and it would invite
an unsafe `dlclose`. So this step is the design, and step 1 above is the
next PR.

## Decisions for the maintainer

1. Erlang semantics: only qualified calls switch versions, and local
   calls stay on the running version. The alternative, "everything
   switches at the next call", is unsound with spilled frames.
2. Opt-in per module (`--reloadable` / `@reloadable`), so ordinary
   programs keep direct calls and static linking.
3. No `dlclose` until the purge accounting exists. A long-lived node that
   reloads N times keeps N copies of the module mapped.
4. State migration is user code (`code_change` message), not automatic.
5. Whole-binary re-exec (B) is a separate feature and does not close #37.
