# Design: a SwarmRT process under 5 KB

Status: proposed, not started. Owner: maintainers (not O). Tracking issue: #161.
Baseline: `main` @ f9e4929. Line numbers below are from that commit.

## How to use this plan (for the implementing session)

- Work phase by phase, in order. Each phase is one PR, or a few PRs where a
  phase lists sub-steps. Stop after each phase and ask for review before
  starting the next.
- Every phase has a **Done when** list. A phase is not done until every item
  passes and the numbers are pasted into the PR.
- Never trade correctness for bytes. The full test battery runs on every PR:
  `make test-full`, `make tsan-gate`, `make lsan-gate`, `make gc-slope`,
  `make gc-stress`, `make fuzz`, and `bench/actor/run.sh` for throughput.
- If something in this plan turns out to be wrong in the code, stop and say
  so in the PR rather than working around it. The plan is a map, not a
  contract.

## 1. The goal

A process of a compiled sw program (`swc build`) costs **under 5 KB of
resident memory while it is actively passing messages**, and under 2 KB when
idle. This holds with:

- 100,000 and 1,000,000 live processes;
- on Linux x86-64 and macOS arm64;
- with default OS settings (no `sysctl`, no `ulimit` changes) and default
  runtime settings;
- with no message-throughput regression worse than 10% on `bench/actor`.

"Per process" means `(RSS(N) - RSS(0)) / N`. Here RSS(0) is the same program
with no worker processes, and the workload is the #161 ring (`MODE=busy`:
every process forwards tokens) measured after warm-up. The §2 numbers are
plain `VmRSS / N`, which includes a few MB of fixed runtime overhead, so they
overstate the per-process cost slightly.

For reference, Erlang documents 338 words (2,704 bytes on 64-bit) for a newly
spawned process. A Go goroutine starts with a 2 KB stack plus about 400 bytes
of `g` struct, and its stack grows. This plan targets about 1–2 KB for a
typical parked process, which is at or below both.

## 2. Where the memory goes today (measured)

Measured on Linux x86-64 (4 vCPU container, glibc), `SW_SCHEDULERS=4`, with
N = 10,000 and the #161 ring program (`bench/memory/ring.sw`), built with
default `swc build` flags. `bench/memory/measure_smaps.py` attributes RSS to
mappings.

| Configuration | Busy at 3 s / 5 s / 8 s / 11 s (KB/process) | Idle (KB/process) |
|---|---|---|
| `main` today | 68 / 140–149 / 280 / **252**: climbs to the 256 KB reset threshold, then saw-tooths | 11.1 |
| `MALLOC_ARENA_MAX=1` (one glibc arena) | – / 54.7 / – / – | – |
| `SW_SCHEDULERS=1` | – / 58.7 / – / – | – |
| `SW_TURN_RESET_BYTES` = 2 KB (compile-time) | – / 16.1 / – / – | – |
| … + first arena chunk 1 KB + trace ring 8 entries | 10.9 / 10.5–11.4 / 10.7 / **10.4**: flat | **6.7** |

#161 reported 110–220 KB because it sampled before steady state. The
steady state is the reset threshold itself, exactly as cause 1 below
predicts, and single samples vary ±30% between runs. The harness (Phase 0)
must take the median of several samples taken after steady state.

The busy breakdown on `main` today, at 5 s:
- glibc per-thread malloc arenas: 89 KB/process;
- other malloc mappings: 20 KB/process;
- process stacks: exactly 4.0 KB/process, one touched page each;
- the slab and file mappings: under 1 KB.

**Causes, largest first:**

1. **Turn garbage.** A process's value arena is reset only at a self-tail-call,
   and only once more than `SW_TURN_RESET_BYTES` (256 KB,
   `src/swarmrt_varena.h:139`) has been allocated since the floor
   (`sw_turn_checkpoint`, `src/swarmrt_lang.c:2976`). A receive loop therefore
   carries a saw-tooth of 0–256 KB of dead values. Every received message's
   region is spliced into the arena (`sw_varena_adopt` in the generated
   receive) and sits there until that reset. Freed chunks stay with the
   allocator, so resident memory settles near the peak, not the average:
   measured 252–280 KB per active process at 8–11 s.
   - `SW_TURN_RESET_BYTES` is a compile-time `#define`. Setting it as an
     environment variable does nothing, so #161's "env helped a bit" was noise.
2. **Cross-thread malloc churn.** Message regions are allocated on the
   sender's scheduler thread and freed on the receiver's. glibc's per-thread
   arenas retain that memory: forcing one arena cuts busy memory 2.7×.
3. **One stack mapping per process.** Each process has its own mmap'd
   128 KB stack plus a guard page (`src/swarmrt_native.c:835-877`).
   - It costs at least one page resident: 4 KB on Linux x86-64, 16 KB on
     Apple Silicon.
   - It costs **2 kernel mappings (VMAs) per process**. Linux's default
     `vm.max_map_count` is 65,530, so on a stock Linux box spawning
     **33,000 idle processes crashes with SIGSEGV**; 32,000 works. The mmap
     failure in `process_init_arena` is ignored at `src/swarmrt_native.c:2351`.
     The README's "100K processes" does not hold on default Linux today.
4. **A fixed per-process overhead of about 10 KB:**
   - an 8 KB first arena chunk (`sw_varena_create(8192)`,
     `src/swarmrt_native.c:809`, floored at 8 KB in
     `src/swarmrt_varena.c`);
   - a 1,560 B trace ring (`sw_gen_exec_t`, `trace[64]`,
     `src/swarmrt_native.h:91-97`), calloc'd and zeroed at every spawn;
   - a 480 B process control block (`sw_process_t`);
   - a dead 2 KB "heap block" per slot in the arena pool
     (`src/swarmrt_native.c:631-690`). It is set up but never written, so it
     costs virtual memory only.
5. **Fat values.** `sw_val_t` is 72 B, because the `fun` member of the union
   holds 8 words. Small ints and five atoms (`true`, `false`, `nil`, `ok`,
   `error`) are shared immortals; every other atom, including the literal
   `'tok'` in the hot loop, allocates 72 B plus a copied string on every use.
6. **Message regions** start at a 256 B chunk (`sw_varena_create_kind`) plus
   malloc overhead, whatever the payload size.

**What can't be fixed without changing the architecture:** causes 1, 2, 4, 5
and 6 are fixable in place. The measured prototype gets busy processes from
about 250 KB at steady state to a flat 10.4–11.4 KB, roughly 24× smaller.
Cause 3 is structural. A process that owns a native stack costs at least one
page while parked (16 KB on macOS), and two VMAs. So the goal needs
processes that **don't own a stack while parked**. That is the core of this
design.

## 3. Target architecture

### 3.1 The idea: stackless processes, captured continuations

Compiled sw code keeps running on a native C stack, so the fast path is as
fast as today. That stack no longer belongs to a process: it is a **carrier**
owned by the scheduler. When a process must stop (receive with an empty
mailbox, reduction yield, sleep, blocking builtin, deep-recursion spill), it
doesn't context-switch away and keep its stack. Instead each generated frame
on the carrier:

1. saves the locals it still needs into a small **heap frame**;
2. records where to resume (a suspend-point number);
3. returns the sentinel `SW_SUSPEND` to its caller.

The C stack unwinds back to the scheduler. A parked process is then:

- its control block;
- a short chain of heap frames, typically 1–3 frames of 64–128 B;
- its arena, compacted to live data at park;
- its mailbox.

None of this is page-granular, so the numbers are the same on Linux and
macOS, and the process has no VMAs.

To resume a process, the driver calls the innermost frame's resume function.
It restores the locals, jumps to the suspend point, and continues on the
carrier. When it finishes, its value is delivered to the parent frame the
same way.

**Cost:**
- **Fast path:** one compare-and-branch after each call.
- **Suspension:** proportional to the frames created since the last resume.
  A receive loop that re-parks reuses its existing heap frame, so it costs a
  handful of stores.
- **Migration:** a parked process has no native stack, so any scheduler can
  resume it.

Prior art:
- Pettyjohn et al., "Continuations from Generalized Stack Inspection"
  (ICFP 2005): one-shot continuations by unwinding and rebuilding;
- C# async and Kotlin coroutines: resume through a state-number switch;
- protothreads: the same switch-dispatch trick in C.

What's different here is that capture is **lazy**. No frame is allocated
unless a suspension actually happens.

### 3.2 Carriers, and the always-correct fallback (pinning)

A **carrier** is a fiber stack: an mmap with a guard page, lazily touched,
default 1 MB virtual. Each scheduler has a current carrier plus a small free
pool.

- The scheduler loop stays on the OS thread stack, as today. To run a
  stackless process it swaps onto its carrier, and `carrier_main` calls the
  process's driver (§3.5).
- When the driver returns (suspended or exited), `carrier_main` sets
  `status = DONE` and swaps back. That's two swaps per run, the same as today.
- **Pinning.** If code running on a carrier blocks the old way, the carrier
  becomes the process's private stack (`proc->carrier = c`) and the
  scheduler takes a fresh carrier. "The old way" means an existing C
  primitive that calls `sw_context_swap` from inside C: `sw_mailbox_wait_new`,
  `sw_yield`, `sw_park_until`, `sw_sleep_ms`, `sw_receive_tagged`, an
  interpreter `receive`, and so on.
  - Detection needs **no change to those primitives**. After the swap back,
    if `c->status != DONE` and the process isn't exiting, the process
    swapped out from the middle of a run, so the scheduler pins `c` to it.
  - A pinned process resumes exactly as a fiber does today
    (`sw_safe_swap_into` on `proc->ctx`).
  - When its code later unwinds back to `carrier_main` (it finished, or it
    suspended the new way), the episode is over. The scheduler unpins it and
    the carrier returns to the pool.
- **Why this matters:** correctness never depends on converting every
  blocking path. A path that hasn't been converted, or can't be (the
  tree-walking interpreter used by `swc run` and dynamic agent tools, C
  callbacks, `sys_exit`, gen_server internals, distribution sends), simply
  pins a carrier for as long as it is blocked there. Conversion only buys
  memory. Each phase below converts more paths; none is required for
  correctness.
- **Panics** keep working. `sw_process_panic` swaps to the scheduler from
  wherever it is. The scheduler sees `EXITING` and destroys the process. A
  carrier that wasn't pinned stays the scheduler's carrier, because its saved
  `c->ctx` is still the last `DONE` point inside `carrier_main`, so the next
  run just loops on a fresh frame. A pinned carrier is returned to the pool.
  Kills follow the same path.
- **Two kinds of process stay stackful-on-carrier for life:** processes
  spawned by C runtime code with a `void (*)(void*)` entry (supervisors,
  bridges, internal servers, of which there are few), and interpreted
  processes under `swc run` (see D3). Root processes that need a big stack
  (the interpreter root, #100) get a dedicated large carrier.
- **Thread-locals:** code that runs after a context switch (`carrier_main`
  and the commit paths) must read thread-locals only through out-of-line
  accessors, never through `extern __thread` directly. That is the lesson of
  #141. Stackless activations never migrate threads mid-activation, so the
  whole #141 bug class disappears for them.

### 3.3 The suspend protocol

Three sentinels, as distinct static `sw_val_t` objects:

```c
extern sw_val_t sw_sentinels[3];
#define SW_SUSPEND (&sw_sentinels[0])   /* the callee parked: capture yourself and return SW_SUSPEND */
#define SW_RAISE   (&sw_sentinels[1])   /* error() inside a try: jump to your catch or propagate (§3.6) */
#define SW_HOP     (&sw_sentinels[2])   /* replaces SW_TC_HOP: a resumed body tail-called (§3.5) */
/* integer compare: subtracting pointers into different objects is undefined in C */
#define SW_IS_SENTINEL(v) ((uintptr_t)(v) - (uintptr_t)sw_sentinels < sizeof(sw_sentinels))
```

A suspension is **intent first, commit later**:

1. **Intent.** The suspending primitive records what the process waits for in
   the process struct (`wait_kind` plus arguments) and returns `SW_SUSPEND`.
   It publishes nothing: no `mailbox.waiting`, no timer, no job submitted. The
   process is still `RUNNING`, so concurrent senders only push to `sig_head`.
2. **Unwind.** Generated frames capture themselves as the sentinel propagates
   (§3.4). The driver records the chain in `proc->cont`.
3. **Commit.** Back in the scheduler loop on the OS stack, with nothing of
   the process left on any stack, the scheduler performs the wait:

| `wait_kind` | Commit (scheduler loop, after the carrier returns) |
|---|---|
| `YIELD` | re-enqueue at the back of the run queue |
| `RECV(deadline)` | the existing Dekker park from `sw_mailbox_wait_new` (`src/swarmrt_native.c:2955`): state `WAITING`, `mailbox.waiting = 1` (seq_cst), re-check `sig_head`; if mail arrived, undo and make it runnable again (or resume it at once); otherwise arm the deadline timer |
| `SLEEP(ms)` | arm a `SW_TAG_SLEEP` timer, state `WAITING` |
| `OFFLOAD(job)` | state `WAITING`, then `sw_offload_submit(job)`; the worker stores the result and wakes the process with `mailbox_wake` |
| `CALL` | never reaches the scheduler: the driver handles it (spill, §3.5) |

Because commit happens after capture, a waker that enqueues the process can
never race with frames still being saved. Today that ordering rests on careful
sequencing inside each blocking primitive (`sw_mailbox_wait_new`'s "sender
already enqueued us, swap for a clean dequeue" path, plus `ctx_lock` and
generation checks in `sw_safe_swap_into`). This design gets it by
construction.

New runtime API (`src/swarmrt_frames.h`, new):

```c
int  sw_suspend_ok(void);                      /* stackless-capable process, no_suspend == 0 */
void sw_suspend_yield(void);
void sw_suspend_recv(uint64_t deadline_ns);    /* UINT64_MAX = forever */
void sw_suspend_sleep(uint64_t ms);
void sw_suspend_offload(struct _sw_offl_job *j);
sw_val_t *sw_spill_call(sw_val_t *(*body)(sw_val_t **, int), sw_val_t **args, int n);
sw_val_t *sw_resume_value(void);               /* the value delivered to the frame being resumed */
int  sw_resume_timed_out(void);                /* for RECV with a deadline */
void sw_no_suspend_enter(void);                /* per PROCESS, not per scheduler */
void sw_no_suspend_exit(void);
```

`no_suspend` is a per-process counter. While it is above zero, every suspend
point takes its old-style path, which pins a carrier. It is entered:

- by C code that calls back into sw code and cannot be captured: the
  `ets_update` callback (`src/swarmrt_ets.c:414`,
  `src/swarmrt_builtins_studio.h:515`), the generated `map`/`filter`/
  `reduce`/`pmap` helpers until they're converted, and the interpreter entry
  (`sw_lang_call`, `tool_call`);
- for the whole life of interpreted and C-entry processes.

### 3.4 Frames and the codegen transformation

**Frame layout:**

```c
typedef struct sw_fn_meta { const char *module, *fn; int line; } sw_fn_meta_t;
typedef struct sw_frame {
    struct sw_frame *parent;                 /* caller's frame; NULL = the process root */
    sw_val_t *(*resume)(struct sw_frame *);  /* re-enters the function at pt */
    const sw_fn_meta_t *meta;                /* panic traces walk the chain */
    uint32_t pt;                             /* suspend point to resume at */
    uint32_t size;                           /* bytes, for the allocator */
} sw_frame_t;                                /* 32 B; generated slots follow */
```

New process-struct fields go at the **end** of `struct sw_process`. The asm
pins `ctx` at offset 0x70, and the existing comments explain why everything
new goes last.

```c
sw_frame_t *cont;                 /* innermost parked frame, or NULL */
sw_frame_t *cap_inner, *cap_outer;/* capture in progress (innermost / outermost so far) */
sw_val_t   *resume_val;  uint8_t resume_timed_out;
uint8_t     wait_kind;   uint64_t wait_arg;  void *wait_job;
struct sw_carrier *carrier;       /* non-NULL while pinned */
uint32_t    no_suspend;
uint8_t     started, stackless;   /* stackless = has an entry_v */
sw_val_t *(*entry_v)(void *);     /* value-returning entry; NULL for C-entry processes */
int32_t     try_depth;            /* replaces try_chain (§3.6) */
/* pending call for spill (and hops, replacing the TLS sw_tc_next_* of #121/#141) */
void       *pend_fn;  int pend_n;  sw_val_t *pend_args[SW_TC_MAX];  sw_varena_mark_t pend_floor;
```

**Linking** (`sw_frame_link`) runs from inside each capturing frame, so the
chain builds innermost-first:

```c
/* called by a frame F that is capturing; `resumed` = F came from the heap */
if (p->cap_outer) p->cap_outer->parent = F; else p->cap_inner = F;
p->cap_outer = F;          /* a resumed F keeps its existing parent pointer */
```

**The codegen transformation.** Below is the real `Main.wloop` from the #161
ring, as `swc emit` prints it today (abridged), followed by what it must
become. Every generated function and every lambda gets the same treatment.

Today:

```c
static sw_val_t *Main_wloop__b(sw_val_t **_args, int _nargs, sw_varena_mark_t *_sw_tc_fl) {
    sw_val_t *next = _nargs > 0 ? _args[0] : sw_val_nil();
    _sw_trace_push("Main", "wloop", 8);
    if (sw_stack_low()) _sw_runtime_panic("stack overflow in Main.wloop …");
    if (sw_check_reds()) sw_yield();
    sw_varena_mark_t _gc_floor = _sw_tc_fl ? *_sw_tc_fl : sw_varena_mark(sw_self_varena());
_tail:;
    sw_val_t *_t22 = sw_val_nil();
    { int _matched = 0;
      while (!_matched) {
        sw_mailbox_drain_signals();
        /* … scan; on {'tok', k}: send, turn checkpoint, `if (sw_check_reds()) sw_yield(); goto _tail;` … */
        if (!_matched) { if (!sw_mailbox_wait_new(UINT64_MAX)) break; }
      } }
    _sw_trace_pop();
    return _t22;
}
```

After:

```c
typedef struct { sw_frame_t h; sw_val_t *next, *_t22, *k, *_o; int _matched; sw_varena_mark_t floor; } Main_wloop_F;
static const sw_fn_meta_t Main_wloop_meta = { "Main", "wloop", 8 };
static sw_val_t *Main_wloop__rs(sw_frame_t *f) { return Main_wloop__b(NULL, 0, NULL, f); }

static sw_val_t *Main_wloop__b(sw_val_t **_args, int _nargs, sw_varena_mark_t *_sw_tc_fl, sw_frame_t *_rs) {
    /* (1) every C local is hoisted to function scope and initialised */
    sw_val_t *next = NULL, *_t22 = NULL, *k = NULL, *_o = NULL;
    int _matched = 0, _pt = 0;
    sw_varena_mark_t _gc_floor = {0};
    if (SW_UNLIKELY(_rs != NULL)) {                    /* (2) resume dispatch */
        Main_wloop_F *_f = (Main_wloop_F *)_rs;
        next = _f->next; _t22 = _f->_t22; k = _f->k; _o = _f->_o;
        _matched = _f->_matched; _gc_floor = _f->floor;
        _sw_trace_push("Main", "wloop", 8);
        switch (_f->h.pt) {
        case 1: goto _sw_rs1;      /* entry reduction check */
        case 2: goto _sw_rs2;      /* receive wait: rescan from the top */
        case 3: goto _sw_rs3;      /* back-edge reduction check */
        default: sw_frame_bad_pt(_rs);
        }
    }
    next = _nargs > 0 ? _args[0] : sw_val_nil();
    if (SW_UNLIKELY(sw_stack_low()) && sw_suspend_ok())
        return sw_spill_call(Main_wloop, _args, _nargs);  /* (5) re-issue through the public wrapper */
    _sw_trace_push("Main", "wloop", 8);
    if (sw_check_reds()) {                             /* (3) suspend point: reduction yield */
        if (sw_suspend_ok()) { sw_suspend_yield(); _pt = 1; goto _sw_capture; }
        sw_yield();                                    /* old style: pins a carrier */
    }
_sw_rs1:;
    _gc_floor = _sw_tc_fl ? *_sw_tc_fl : sw_varena_mark(sw_self_varena());
_tail:;
    _t22 = sw_val_nil();
    _matched = 0;
    while (!_matched) {
_sw_rs2:;
        sw_mailbox_drain_signals();
        /* … scan unchanged; the back edge becomes:
               if (sw_check_reds()) { if (sw_suspend_ok()) { sw_suspend_yield(); _pt = 3; goto _sw_capture; } sw_yield(); }
           _sw_rs3:; goto _tail;                                                                  … */
        if (!_matched) {
            if (sw_suspend_ok()) { sw_suspend_recv(UINT64_MAX); _pt = 2; goto _sw_capture; }   /* (3) */
            if (!sw_mailbox_wait_new(UINT64_MAX)) break;
        }
    }
    _sw_trace_pop();
    return _t22;

_sw_capture: {                                         /* (4) one capture block per function */
    Main_wloop_F *_f = (Main_wloop_F *)(_rs ? _rs : sw_frame_new(sizeof(Main_wloop_F), Main_wloop__rs, &Main_wloop_meta));
    _f->h.pt = _pt; _f->next = next; _f->_t22 = _t22; _f->k = k; _f->_o = _o;
    _f->_matched = _matched; _f->floor = _gc_floor;
    sw_frame_link(&_f->h, _rs != NULL);
    _sw_trace_pop();
    return SW_SUSPEND;
  }
}
```

The rules behind it:

1. **Hoisting.** Every C local of a generated function moves to function
   scope and is initialised there: sw variables, `_tNN` temporaries, pattern
   binds, `_matched`, loop indices, `_gc_floor`. Labels can then be placed
   anywhere at statement level, and the restore block can assign them.
   - Implementation: emit the body into a memory stream (`open_memstream`),
     collect local names as they're declared, then print declarations, the
     dispatch block, the body and the capture block.
   - Scoped scratch arrays that never live across a call stay block-local:
     `sw_val_t *_t28[] = {…}`, `_gc_v[]`, argument vectors.
   - **Shadowing:** if one sw name is bound twice in a function (nested `case`
     arms, a rebinding), give each binding a unique C name (`k__1`, `k__2`)
     before hoisting. `tests/sw/test_case_shadow_scrutinee.sw` must keep
     passing.
2. **Resume dispatch.** One `switch` on `pt` at the top, after restoring
   every hoisted local. Resume points are labels at statement level, never
   inside a sub-expression.
3. **Suspend points:**
   - every call to a module function, a closure (`sw_val_apply`) or a
     builtin;
   - every `sw_check_reds()` site (function entry and loop back-edges);
   - every receive wait.

   A call site looks like this. The label goes *before* the sentinel check,
   so a delivered `SW_RAISE` is handled exactly like a returned one:

   ```c
   _t5 = Main_g(_argv5, 2);
   _sw_rs5:
   if (SW_UNLIKELY(SW_IS_SENTINEL(_t5))) {
       if (_t5 == SW_SUSPEND) { _pt = 5; goto _sw_capture; }
       goto _sw_raise_5;               /* the enclosing try's catch, or `_sw_trace_pop(); return SW_RAISE;` */
   }
   /* resume case: `case 5: _t5 = sw_resume_value(); goto _sw_rs5;` */
   ```

   Every builtin call gets the check, so no list of "builtins that may block"
   is needed. Builtins that can't suspend never return a sentinel, and the
   cost is one compare.
4. **Capture block.** One per function. It reuses `_rs` when the function was
   resumed from the heap, so a receive loop that re-parks allocates nothing.
   - Phase 2 saves **all** hoisted locals, which is simple and correct.
   - Phase 2h adds liveness (save only what is live at that `pt`) to shrink
     frames in large functions.
5. **Spill (unbounded recursion).** When the carrier runs low (less than
   128 KB left; make it configurable), the function at entry, before its
   trace push, parks its own call: `sw_spill_call`
   copies the args into `pend_args` (the caller's argument array is about to
   unwind), sets `wait_kind = CALL` and returns `SW_SUSPEND`. The callers
   capture as usual, and the driver re-issues the call on the now-empty
   carrier.
   - Non-tail recursion becomes limited by heap, not stack, which also closes
     #100 for compiled code.
   - The cost is proportional to frames created since the last spill:
     O(1) amortised per call.
   - Inside `no_suspend` the old panic stays.
6. **Hops (#121) and floors.** A body that tail-calls another module function
   still parks the call and returns `SW_HOP`.
   - The park moves from `__thread sw_tc_next_*`
     (`src/swarmrt_lang.h:270`) to `proc->pend_*`. That fixes #141 for every
     path.
   - The public wrapper's `sw_tc_drive` loop is unchanged and is transparent
     to `SW_SUSPEND`.
   - `_gc_floor` is saved in the frame. A resumed function uses the saved
     floor, and the hop floor parameter is `NULL` on resume.

**Process entry.** Generated trampolines (`emit_spawn_trampolines`,
`src/swarmrt_codegen.c:1522`) and runtime entry functions that apply a
closure (`src/swarmrt_builtins_studio.h:3784, 7045, 7075`) change from
`void (*)(void*)` to `sw_val_t *(*)(void*)`.
- Each copies what it needs (fn, args) into locals, frees its spawn struct,
  then `return sw_val_apply(fn, args, n);`.
- They must be transparent: no work after the call.
- Post-exit work belongs in `on_destroy`, which already exists for this.

**`sw_val_apply` (`src/swarmrt_lang.c:2579`)** is already transparent. Its
stack `inlinebuf` is read only by the callee's prologue, which copies its
params into locals. The other C callers of compiled closures must either
handle sentinels or run their callback under `no_suspend`. That list:
- `ets.c:414` and `builtins_studio.h:515` (`ets_update` with a function);
- `builtins_studio.h:8171`;
- the generated `map`/`filter`/`reduce`/`pmap` helpers
  (`src/swarmrt_codegen.c:1360-1480`).

### 3.5 The driver

The driver runs on a carrier, called by `carrier_main`. It treats `SW_RAISE`
as an ordinary value; frames dispatch on it.

```c
/* returns SW_RUN_SUSPENDED (p->cont, p->wait_kind set) or SW_RUN_EXITED */
static int sw_run_stackless(sw_process_t *p) {
    sw_frame_t *k;            /* heap frame being run; NULL = a fresh activation */
    sw_frame_t *waiting;      /* frames waiting for the fresh activation's value */
    sw_val_t *r;
    if (!p->started) { p->started = 1; k = NULL; waiting = NULL; r = p->entry_v(p->arg); }
    else             { k = p->cont; p->cont = NULL; waiting = NULL; r = k->resume(k); }
    for (;;) {
        if (r == SW_SUSPEND) {
            if (!k && p->cap_outer) p->cap_outer->parent = waiting;   /* fresh activation hangs under `waiting` */
            sw_frame_t *top = p->cap_inner ? p->cap_inner : waiting;
            p->cap_inner = p->cap_outer = NULL;
            if (p->wait_kind == SW_WAIT_CALL) {                       /* spill: re-issue on an empty carrier */
                p->wait_kind = SW_WAIT_NONE; waiting = top; k = NULL;
                r = sw_call_spilled(p);       /* pend_fn is a PUBLIC wrapper: it drives its own hops */
                continue;
            }
            p->cont = top;
            return SW_RUN_SUSPENDED;
        }
        if (r == SW_HOP) {                                            /* a resumed body tail-called */
            if (k) { waiting = k->parent; sw_frame_free(k); k = NULL; }
            r = sw_call_hop(p);               /* pend_fn is a BODY: call it with &pend_floor; it may HOP again */
            continue;
        }
        sw_frame_t *next;                                             /* an activation finished with r */
        if (k) { next = k->parent; sw_frame_free(k); } else next = waiting;
        if (!next) return SW_RUN_EXITED;                              /* the root returned */
        p->resume_val = r; k = next; waiting = NULL;
        r = k->resume(k);
    }
}
```

**Changes to the scheduler loop** (`src/swarmrt_native.c:1585`):

- **Pick:** keep the `generation` and `kill_flag` checks.
- **Running a process:**
  - pinned: `sw_safe_swap_into` exactly as today;
  - stackless: set `c->proc = proc`, `c->status = RUNNING`, swap onto the
    carrier.
- **Back from the swap:**
  - `EXITING`: exit or panic, as today;
  - `c->status == DONE`: handle the run result. For `SUSPENDED`, commit
    `wait_kind` (§3.3); for `EXITED`, `process_exit` and `process_destroy`;
  - otherwise the process swapped out old-style from a carrier, so pin it:
    `proc->carrier = c; sched->carrier = NULL`.
- **Destroy:** free the frame chain in addition to the arena.

### 3.6 try/catch without setjmp

Today `try` pushes a `jmp_buf` on the fiber stack, and `error()` longjmps to
it (`src/swarmrt_builtins_studio.h:4660-4670`). A `jmp_buf` can't survive
capture, so:

- `try` entry does `try_depth++`; the normal exit and the catch entry do
  `try_depth--`.
- `error(x)` sets `gen_error = x` and, if `try_depth > 0`, returns `SW_RAISE`.
  With no live try it keeps the documented behaviour and returns `nil`.
- Every call site already checks sentinels. On `SW_RAISE`, it jumps to the
  innermost enclosing try's catch label in this function, or pops its trace
  and returns `SW_RAISE`. The codegen knows the try nesting at every call
  site statically.
- Errors raised inside callbacks reached through C must be propagated by
  those C callers (the list in §3.4). The `no_suspend` callers return
  `SW_RAISE` when the callback did.
- Panics never use this path; they stay uncatchable.

This is the one semantic-risk change in the plan. It needs its own tests
(§5), and the maintainer decides it (D2).

### 3.7 Memory: what a parked process holds, and keeping it that way

- **Compaction at park.** When a stackless process parks, its roots are known
  exactly: the frame slots, `gen_error`, `resume_val` and `pend_args`.
  - If `arena->total_bytes > max(PARK_COMPACT_MIN, 2 × live_last)` (default
    minimum 2 KB), deep-copy the roots into a fresh right-sized region
    (`deep_copy_rec` with a sharing map, as `sw_turn_checkpoint` does), free
    the old arena, and set every frame's saved floor to the new arena's mark.
  - Each compaction copies only live data, with the existing 2× rule keeping
    it amortised.
  - Mailbox messages stay in their own regions until they are received.
  - Pinned processes don't compact: their C stack holds unknown roots.
- **Turn checkpoints stay.** They still bound garbage for long loops that
  never park. Their default threshold drops from 256 KB (Phase 1), and it
  becomes a runtime setting instead of a `#define`.
- **Messages.** Size the payload first (one pass), then allocate one region
  of exactly that size. There is no 256 B minimum. Interned atoms and
  immortal small ints are shared, never copied.
- **Values (Phase 3).**
  - `sw_val_t` shrinks from 72 B to 32 B. `fun` moves out of line (a pointer
    to a `sw_fun_t` allocated in the same block), `map` becomes
    `{ sw_map_body *b; int count; }`, and `tuple` and list stay 24 B.
  - All atoms are interned and immortal: compile-time literals are hoisted by
    the codegen, and runtime-created ones (`to_atom`) go into a global table.
    Like Erlang, the table has a cap, and exceeding it is a clear error.
- **Allocator (Phase 4).** Frames, arena chunks, message regions and
  `sw_msg_t` come from size-class slabs carved from one large `MAP_NORESERVE`
  reservation, so the VMA count stays 1.
  - Per-thread free lists; frees from other threads go onto an atomic
    remote-free list per slab page, like mimalloc.
  - Fully free pages go back to the OS with `madvise`.
  - There are no malloc headers, and cross-thread frees don't bloat memory.
- **Trace state.** `_sw_current_line`, `_sw_current_file` and the trace ring
  move to the scheduler. A panic trace is the carrier ring plus a walk of the
  process's heap frames through `meta`. The 1,560 B per-process
  `sw_gen_exec_t` goes away.

**The per-process budget after all phases.** These are targets to verify,
not promises.

| Part | Ring worker, parked | Note |
|---|---|---|
| Process control block | 480 B → ~320 B | Phase 5 drops dead fields (`heap`, `htop`, `stop`; `ctx` and stack moved to the carrier) |
| Heap frames | ~96 B | one `wloop` frame; `worker` hopped into `wloop`, so it holds no frame |
| Arena (compacted) | ~96–160 B | the `next` pid (32 B) plus region header and chunk |
| Mailbox | 0–2 messages × ~160 B | `sw_msg_t` (64 B) plus an exact-size region |
| Stack | 0 | carriers are per scheduler |
| Trace and line state | 0 | per scheduler |
| **Total** | **~0.7–1.1 KB**, up to ~1.4 KB with 2 queued messages | Linux and macOS identical: nothing per process is page-granular |

### 3.8 What stays stackful, and what it costs

These hold a carrier, about 8–64 KB touched, only while blocked inside the
region, and they never limit the process count:

- interpreted code: `swc run`, `swc test`, the REPL, and agent tools defined
  from source and run by `tool_call` → `sw_lang_call`;
- C-entry runtime processes: supervisors and internal servers;
- callbacks called from C that block: `ets_update` with a function, and
  `map`/`filter`/`reduce`/`pmap` callbacks until Phase 2e;
- `sys_exit`, gen_server internals in `swarmrt_otp.c`, and distribution
  sends.

Carriers are pooled per scheduler. The idle pool is capped (default 4 per
scheduler, with the extra ones unmapped), and a returned carrier that touched
more than 64 KB is `madvise`d.

### 3.9 Alternatives considered (and why not)

| Alternative | Why not |
|---|---|
| Smaller per-process stacks (16–32 KB) | Still at least one page resident while parked (16 KB on macOS), still 2 VMAs per process (the 32K ceiling on stock Linux), and smaller stacks overflow sooner |
| `madvise` the stack on every park | A syscall per park plus a page fault per resume: about 1–3 µs per message, a throughput collapse. The VMA ceiling remains |
| Copy the stack to the heap on park, back on resume (greenlet-style) | The copy must be restored at the same address: C frames hold pointers into themselves. That breaks work-stealing migration, and relocating C frames isn't safe |
| Split stacks (`-fsplit-stack`) | GCC-only (not clang or macOS), deprecated, and no help for C runtime frames |
| A BEAM-style bytecode VM with heap frames | Rewrites the compiler and runtime, and loses the C compiler's speed |
| CPS with a heap frame per call (Kotlin-style eager) | An allocation on every call |
| **Chosen: lazy capture plus carriers** | Zero cost on the fast path, pays only at suspension, migrates freely, and degrades safely to pinned when a path isn't converted |

## 4. Phases

Every phase lists files, steps, tests and a **Done when**. Expected numbers
are Linux x86-64; macOS numbers are recorded from Phase 0 on.

### Phase 0: measurement harness and baseline (1 PR, small)

- Extend `bench/memory/`, which already holds the prototype used for §2:
  - `ring.sw` is #161's program.
  - `measure_smaps.py` is Linux-only and attributes RSS by mapping kind.
  - Add `measure.sh` and `measure.py`: run `N=0` and `N=target`, sample RSS
    (`/proc/PID/smaps_rollup` on Linux, `ps -o rss=` on macOS) and count
    mappings, then print JSON:
    `{n, mode, rss_bytes, per_process_bytes, vma_count, spawn_ms}`.
  - Have `ring.sw` print a ready line once warm, so the sample time doesn't
    depend on `sleep`.
- Run `bench/actor/run.sh` (pingpong, spawn, fanout, parallel) and save the
  baseline.
- Add a CI job, `memory-bench`, on Linux and macOS. It reports only and does
  not gate yet. Until Phase 2 it can run N=30,000 at most, because N ≥ 33,000
  crashes on stock Linux (§2); do not raise `vm.max_map_count` in CI.
- **Done when:** the baseline JSON (busy and idle at N = 10K and 30K, Linux
  and macOS) and the `bench/actor` numbers are committed in
  `bench/memory/BASELINE.md`.

### Phase 1: quick wins, stacks unchanged (1–2 PRs)

1. Make the turn-reset threshold a runtime setting: `SW_TURN_RESET_BYTES`
   from the environment, read once in `sw_init`, into a global that the
   generated code reads. Change the default from 256 KB to 4 KB, keeping the
   `2 × turn_live` adaptive rule.
   - (#205: the carry later moved into each loop's floor, and the trigger
     counts the chunk walk too; a per-arena carry made nested loops
     quadratic at the 4 KB default.)
   - Measure 2, 4, 8 and 16 KB against `bench/actor` and `gc-slope`, and pick
     the smallest default that costs ≤ 3% throughput.
2. Change the process arena's first chunk from 8 KB to 1 KB: `sw_varena_create`
   floor and `src/swarmrt_native.c:809`.
3. Reduce the trace ring from 64 to 16 entries: `sw_gen_exec_t` and the
   generated `_sw_trace_push` bound at `src/swarmrt_codegen.c:1122`. Allocate
   it with `malloc` instead of `calloc` plus `memset`; only `trace_top` needs
   resetting.
4. Hoist atom literals in the codegen into module-level immortal values, so
   the hot loop's `sw_val_atom("tok")` becomes a load.
5. Size message regions exactly (a sizing pass, then one region), with no
   256 B minimum.
6. Remove the dead 2 KB-per-slot block pool from `sw_arena_init`
   (`src/swarmrt_native.c:631-690`) and the `heap`/`htop`/`stop` setup in
   `process_init_arena`. Keep the struct fields until Phase 5 (asm offsets).
7. Make a failed spawn clean. If `process_init_arena` fails (stack mmap
   refused), `spawn` returns an error (or the documented panic) naming the
   limit and `vm.max_map_count`, instead of SIGSEGV
   (`src/swarmrt_native.c:2351`). Add a test that spawns past the limit under
   a lowered `ulimit -v`.
8. On glibc only: measure `mallopt(M_ARENA_MAX, nsched)` and `M_TRIM_THRESHOLD`.
   Adopt one only if it saves ≥ 10% with ≤ 2% throughput cost; otherwise
   leave this to Phase 4.

- **Expected:** busy about 11 KB/process, flat over time (from about 250 KB at
  steady state today), and idle about 6.5 KB/process on Linux. The prototype
  measured 10.4–11.4 and 6.7. macOS is about 12 KB higher per
  process from the stack page alone.
- **Done when:** the Phase 0 harness shows the numbers, all gates pass,
  `bench/actor` is within 3%, and `KNOWN_ISSUES.md` plus the README's
  process-count claims say what is true after this phase: idle versus active
  memory, and the Linux mapping ceiling.

### Phase 2: stackless processes (the core; about 8 PRs, in this order)

Each sub-step leaves `main` green and is independently revertable.

- **2a. Carriers and pinning, no codegen change.**
  - Add `src/swarmrt_carrier.c` with carrier alloc, free and pool,
    `carrier_main`, and the raw context swap. Add
    `sw_context_swap_raw(sw_context_t *from, sw_context_t *to)` to
    `src/swarmrt_asm.S`: the same register save and restore as
    `sw_context_swap`, at offset 0.
  - Change the scheduler loop as in §3.5. Every process still blocks
    old-style, so every process pins. Behaviour should match today, with
    stacks now pooled carriers.
  - Remove per-process stack mmaps from `process_init_arena`.
  - Tests: the whole battery, plus a new `tests/sw/test_carrier_pin.sw` that
    churns 10K processes with receive, sleep, panic and kill and checks the
    carrier pool size.
  - Done when: everything is green, `bench/actor` is within 5%, and the VMA
    count is O(schedulers + blocked processes).
- **2b. Hoisting and unique local names, no behaviour change.**
  - In `emit_function` (`src/swarmrt_codegen.c:3715`), the lambda emission
    (`emit_lambda_functions`, line 1610) and their callees: emit into a
    memory stream, hoist all locals, and give each binding a unique C name.
  - Done when: the generated C differs only in declarations, everything is
    green, and compiling all of `examples/` plus `swarm-code` at `-O2` shows
    no warnings other than today's.
- **2c. Frames, the suspend protocol and the driver**, for module functions
  and lambdas:
  - suspend points at calls, at `sw_check_reds`, and at receive waits
    (`emit_receive`, line 2787), including the receive `after` deadline;
  - the capture block and resume dispatch;
  - `sw_run_stackless` and the commit phase for `YIELD` and `RECV`;
  - the spill (`sw_spill_call`, `wait_kind = CALL`);
  - hops moved to `proc->pend_*` (closes #141);
  - entry trampolines that return values.
  - **Torture modes, required in this PR:**
    - `SW_SUSPEND_TORTURE=n`: `sw_check_reds()` reports "yield" every nth
      check, so every function suspends at every point;
    - `SW_SPILL_TORTURE=n`: `sw_stack_low()` fires every nth entry.

    Run `make test-sw` and `tests/sw/run_conform.sh` under each and diff the
    outputs against a normal run.
  - Done when:
    - torture runs match normal runs byte for byte;
    - a 1,000,000-deep non-tail recursion test passes;
    - the ring at N = 100,000 runs on stock Linux and busy memory is under
      5 KB/process;
    - `bench/actor` is within 10%.
- **2d. try/catch via `SW_RAISE`** (§3.6, after D2 is decided).
  - Tests: an error in a callee, a lambda, a pipe, a comprehension, a callback
    through `map`, and `ets_update` inside a try; an error after a receive and
    after a sleep inside a try; nested tries; an error with no try still
    returns nil.
- **2e. Async builtins.**
  - `sleep` uses `sw_suspend_sleep`.
  - Offloaded builtins (`http_*`, `shell*`, `exec_*`, which covers
    `llm_complete`) use `sw_suspend_offload` instead of parking inside
    `_sw_offload_builtin` (`src/swarmrt_builtins_studio.h:1242`). A resume
    hook adopts `j->in` and `j->out` into the arena before the frame sees
    the result.
  - The `pmap` collector and the `map`/`filter`/`reduce` helpers become
    resumable: either generated as frames, or reimplemented in a built-in sw
    prelude compiled with the program.
  - Done when: a 10,000-process `Std.task_stream` run against
    `bench/rollout`'s mock server holds under 5 KB per waiting process.
- **2f. Compaction at park** (§3.7): precise roots and floor reset.
  - Tests: `gc-slope` and `gc-stress` under `SW_SUSPEND_TORTURE`, plus a
    test that parks with a live map, list and closure and checks they
    survive.
- **2g. Trace and line state per scheduler**, with traces taken from frames.
  Remove the per-process `sw_gen_exec_t`. A panic after resume must still
  print the full chain (existing panic-trace tests).
  - As built: the state lives on the carrier, not the scheduler, so a process
    that blocks the old way keeps its trace with its carrier. A parked stackless
    process keeps its line in `sw_process_t` (`park_line`/`park_file`).
    `trace_xproc_repro` now also checks that the chain keeps its callers after a
    resume and that the banner keeps the parked line; on the code before 2g it
    printed only the innermost frame.
- **2h. Liveness for frame slots.** Save only the locals live at each `pt`.
  - A simple def/last-use interval per local over the emitted body is
    enough. Treat sw variables and anything live into a loop header as live
    across that whole loop.
  - Done when: the largest frame in `swarm-code`'s compiled output is ≤ 512 B,
    or explained in the PR.

### Phase 3: value diet (2 PRs)

1. Add accessor macros for the fat union members (`SW_FUN(v)->…`,
   `SW_MAP_KEYS(v)`, and so on), with no layout change. Repoint the sw-value
   access sites; AST nodes share some member names, so don't touch those.
2. Change the layout to a 32 B `sw_val_t` (§3.7), and intern all atoms
   (global table, cap, clear error). In the receive codegen, replace atom
   `strcmp` with pointer compare.

- Done when: `_Static_assert(sizeof(sw_val_t) == 32)`, all gates pass,
  `bench/actor` is within 5% or better, and the busy ring is about 1.5 KB.

### Phase 4: the allocator (1–2 PRs)

- Add `src/swarmrt_alloc.c`: size classes 16–2048 B, 64 KB pages carved from
  one reservation, per-thread free lists, remote-free lists, and `madvise` on
  fully free pages. Above 2 KB, fall back to malloc.
- Route frames, arena chunks, message regions and `sw_msg_t` through it.
- Add an allocator unit test plus TSan.
- Done when: N = 1,000,000 busy, over 10 minutes, shows RSS flat (no creep)
  and within 15% of the sum of live sizes.

### Phase 5: a million processes by default (1 PR)

- The process slab grows in chunks (for example 16K slots each) on demand, up
  to `SW_MAX_PROCS`. The default is 1,048,576 (D4). Spawning past the limit
  is a clear error. This closes #165.
- Slim `sw_process_t`: drop the dead `heap`/`htop`/`stop` fields, move
  `ctx`/`stack_mem`/`stack_size` to the carrier, and update the asm offsets
  and their `_Static_assert`s together.
- Done when: the ring at N = 1,000,000 runs busy under 5 KB/process on Linux
  and macOS, on a 16 GB machine.

### Phase 6: gates, introspection and claims (1 PR)

- Turn `memory-bench` into a gate:
  - Linux and macOS, N = 100,000: busy ≤ 5,120 B/process, idle ≤ 2,048 B;
  - nightly at N = 1,000,000.
- Add `bench/actor` throughput gates (±10% of baseline).
- `process_info` and `swarm_stats()` report per-process resident bytes:
  frames, arena, mailbox (#161 suggestion 3).
- Update the README, `docs/BENCHMARKS.md` and `docs/ARCHITECTURE.md` with the
  measured numbers for idle and active, at 100K and 1M, with an Erlang and Go
  comparison from `bench/actor` plus a matching `ring.erl`. This supersedes
  #171's wording fix.

### Phase 7: decision D3, `swc run` through the compiler (optional)

If the maintainer picks D3(b), `swc run` builds to a cache directory and runs
the binary. The interpreter would stay for the REPL and source-defined tools.

## 5. Verification strategy

- **Differential torture:** every PR from 2c on runs the sw suite and the
  conformance suite under `SW_SUSPEND_TORTURE=1,7,64` and
  `SW_SPILL_TORTURE=13`, and diffs the results against a normal run. That is
  the main defence against a missed local or a wrong resume label.
- **Sanitizers:** run ASAN and UBSAN on the torture runs, and TSan on
  `test_carrier_pin`, the ring and the scheduler stress tests. Build the
  generated C with `-Wjump-misses-init -Werror=maybe-uninitialized` in CI.
- **New tests:**
  - a suspension in every construct: receive inside a case inside a try
    inside a lambda inside a comprehension, and inside a pipe;
  - recursion 1M deep;
  - migration stress: `SW_SCHEDULERS=8` with 64 processes ping-ponging,
    yielding and doing hops;
  - kill while parked, panic while stackless, panic while pinned;
  - `error()` across suspensions;
  - each pinned path: `tool_call`, the `ets_update` callback, a `map`
    callback that receives, `sys_exit`.
- **Existing gates must stay green:** `test-full`, `tsan-gate`, `lsan-gate`,
  `gc-slope`, `gc-stress`, `fuzz`, `test-install`, `test-pkg` and
  `check-docs`.
- **Performance:** `bench/actor` on every phase PR, compared against
  `bench/memory/BASELINE.md`, plus compile time and binary size for
  `examples/` and `swarm-code` (expect generated C about 30–60% larger).

## 6. Risks

| Risk | Mitigation |
|---|---|
| A codegen resume bug: a missed local, a wrong label, a jump into a scope | Hoisting makes every label legal C; torture-mode diffs on every PR; compiler warnings as errors; frames save all locals until 2h |
| Throughput regression from the hoisted, multi-entry control flow | Suspend points only at calls, receive and back-edges; `SW_UNLIKELY` hints; benchmark every PR; capture reuses the frame, so the re-park is cheap |
| GC invariants (floors, adoption, the escape audit) | Floors are saved in frames and reset after compaction; compaction only with precise roots (never for pinned processes); `gc-slope` and `gc-stress` under torture |
| try/catch semantics change (D2) | A dedicated test matrix (2d); C callers of closures audited (§3.4 list) |
| Memory regresses for tool-heavy agent programs (pinned) | Measured in 2e; carriers capped and `madvise`d; tools defined from compiled functions stay stackless |
| Debuggability: gdb backtraces stop at the driver | `sw_dump_frames(pid)` debug builtin and a gdb Python helper; panic traces include the heap chain |
| asm offset churn in `sw_process_t` | All new fields go at the end until Phase 5; offset changes come with their `_Static_assert`s |
| Thread-local hazards (#141 class) in carrier and commit code | Thread-locals only through out-of-line accessors in code that runs after a swap |

## 7. Decisions for the maintainer

- **D1.** Approve the architecture: stackless with lazy capture, carriers and
  pinning. Without it, the floor is one page per process (16 KB on macOS) and
  the stock-Linux ceiling stays near 32K processes.
- **D2.** try/catch moves from `setjmp`/`longjmp` to `SW_RAISE` sentinels
  (§3.6). Recommended: yes. The alternative, re-establishing `setjmp` frames
  on resume through hierarchical dispatch, is possible but fragile.
- **D3.** `swc run`/`swc test`:
  - (a) keep the interpreter: dev-only, documented as stackful;
  - (b) run through the compiler with a build cache, keeping the interpreter
    for the REPL and source-defined tools.

  Recommended: (a) now, (b) later. It also ends the interpreter/compiler
  parity bugs (#93, #113…).
- **D4.** Default `SW_MAX_PROCS`: 1,048,576 recommended (address space is
  reserved lazily, about 320 B per slot when touched).
- **D5.** Allocator: our own about 800 lines (recommended; scoped to runtime
  objects, keeps "zero dependencies") or vendored mimalloc.
- **D6.** Claim wording once the Phase 6 gates pass, for example:
  "1,000,000 processes passing messages in about 1–1.5 GB; about 1 KB per
  parked process on Linux and macOS."

## 8. Appendix: reproducing the numbers

```text
# the #161 ring, N = 10,000, 4 schedulers, sampled at 5 s (default `swc build` flags)
bin/swc build bench/memory/ring.sw -o /tmp/ring
python3 bench/memory/measure_smaps.py /tmp/ring N=10000 MODE=busy SW_SCHEDULERS=4
python3 bench/memory/measure_smaps.py /tmp/ring N=10000 MODE=idle SW_SCHEDULERS=4
python3 bench/memory/measure_smaps.py /tmp/ring N=10000 MODE=busy SW_SCHEDULERS=4 MALLOC_ARENA_MAX=1

# stock-Linux process ceiling (2 VMAs per process; vm.max_map_count = 65530)
N=32000 MODE=idle ./ring    # runs
N=33000 MODE=idle ./ring    # SIGSEGV: process_init_arena's -1 is ignored (swarmrt_native.c:2351)

# struct sizes on main @ f9e4929 (x86-64)
sw_process_t 480   sw_gen_exec_t 1560   sw_val_t 72   sw_msg_t 64   sw_value_arena_t 56   sw_scheduler_t 2656
```
