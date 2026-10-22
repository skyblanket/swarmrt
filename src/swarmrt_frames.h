/*
 * SwarmRT stackless processes: frames and the suspend protocol
 * (docs/design/PROCESS_WEIGHT.md §3.3–3.5, Phase 2c).
 *
 * A stackless process runs on a scheduler's carrier like any other, but when
 * it must wait it doesn't keep the carrier. The waiting primitive records an
 * intent (sw_suspend_*) and returns SW_SUSPEND; every generated frame on the
 * way out saves its locals into a heap frame (sw_frame_new + sw_frame_link)
 * and returns SW_SUSPEND too. The driver (sw_run_stackless) ends the run,
 * and the scheduler commits the intent: re-enqueue for a yield, park on the
 * mailbox for a receive. Resuming calls the innermost frame's resume
 * function, which restores its locals and jumps back to where it suspended.
 *
 * A process suspends this way only while sw_suspend_ok() holds. Everything
 * else (C callbacks into sw code, the interpreter, C-entry processes) blocks
 * the old way and pins its carrier, which is always correct.
 */
#ifndef SWARMRT_FRAMES_H
#define SWARMRT_FRAMES_H

#include <stdint.h>

typedef struct sw_val sw_val_t;
struct sw_process;

/* Sentinels: distinct objects, compared by address, never read as values. */
extern struct sw_val sw_suspend_sentinel;
#define SW_SUSPEND (&sw_suspend_sentinel)
/* An error() raised inside a live try (PROCESS_WEIGHT.md §3.6): every
 * generated call site passes it up to the innermost enclosing catch. */
extern struct sw_val sw_raise_sentinel;
#define SW_RAISE (&sw_raise_sentinel)

typedef struct sw_fn_meta {
    const char *module, *fn;
    int line;
} sw_fn_meta_t;

typedef struct sw_frame {
    struct sw_frame *parent;                 /* caller's frame; NULL = the process root */
    sw_val_t *(*resume)(struct sw_frame *);  /* re-enters the function at pt */
    const sw_fn_meta_t *meta;
    uint32_t pt;                             /* suspend point to resume at */
    uint32_t size;
} sw_frame_t;                                /* generated slots follow */

enum {
    SW_WAIT_NONE = 0,
    SW_WAIT_YIELD,                           /* re-enqueue at the back of the run queue */
    SW_WAIT_RECV,                            /* park until mail arrives or wait_arg (ms) passes */
    SW_WAIT_CALL,                            /* spill: re-issue the parked call on an empty carrier */
};

/* Spill (deep recursion): 1 when a function entry should park its own call
 * and let the driver re-issue it on an empty carrier — the carrier is near
 * its red zone, or SW_SPILL_TORTURE=n says so every nth entry. */
int sw_spill_now(void);

/* Run results reported by sw_run_stackless. */
#define SW_RUN_EXITED    1
#define SW_RUN_SUSPENDED 2

/* Spawn a process whose entry returns a value. Such a process can suspend
 * stacklessly; its entry must be transparent (return its callee's value). */
struct sw_process *sw_spawn_v(sw_val_t *(*entry)(void *), void *arg);
struct sw_process *sw_spawn_v_owned(sw_val_t *(*entry)(void *), void *arg,
                                    struct sw_value_arena *region);

/* 1 when the running process may suspend stacklessly right now. */
int  sw_suspend_ok(void);
/* Record an intent; the caller then returns SW_SUSPEND. */
void sw_suspend_yield(void);
void sw_suspend_recv(uint64_t timeout_ms);   /* UINT64_MAX = no deadline */

/* Frames. sw_frame_new returns a zeroed frame of `size` bytes (header
 * included); sw_frame_link records a capturing frame in the chain being
 * built (innermost first). `resumed` is 1 when the frame came from the heap,
 * in which case it keeps its existing parent. */
sw_frame_t *sw_frame_new(uint32_t size, sw_val_t *(*resume)(sw_frame_t *),
                         const sw_fn_meta_t *meta);
void sw_frame_link(sw_frame_t *f, int resumed);
void sw_frame_free(sw_frame_t *f);
/* Free a frame of a process being torn down (no frame_bytes update). */
void sw_frame_drop(sw_frame_t *f);
/* Frames alive in the runtime: tests check that teardown frees them all. */
long sw_frames_live(void);

/* The value delivered to the frame being resumed (its callee's result). */
sw_val_t *sw_resume_value(void);

/* While above zero, every suspend point takes its old-style path. */
void sw_no_suspend_enter(void);
void sw_no_suspend_exit(void);

#ifndef SW_UNLIKELY
#define SW_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* Run a stackless process until it exits or suspends (called on a carrier). */
int sw_run_stackless(struct sw_process *p);

#endif
