/*
 * SwarmRT Assembly Context Switching
 *
 * True preemptive user-space threading
 * Target: ~100ns context switch
 *
 * Architecture support:
 * - x86_64 (System V AMD64 ABI)
 * - ARM64 (AArch64)
 *
 * Struct offsets (verified with offsetof):
 *   offsetof(sw_process_t, ctx)   = 0x70
 *   offsetof(sw_process_t, entry) = 0xC0 (x86_64) / 0xF0 (ARM64)
 *   offsetof(sw_process_t, arg)   = 0xC8 (x86_64) / 0xF8 (ARM64)
 */

#ifdef __APPLE__
#define CNAME(name) _##name
#else
#define CNAME(name) name
#endif

/* ============================================================================
 * x86_64 Implementation
 * ============================================================================ */
#ifdef __x86_64__

.text
.globl CNAME(sw_context_swap)
.globl CNAME(sw_process_trampoline)
.globl CNAME(sw_rdtsc)

.align 16

/*
 * void sw_context_swap(sw_process_t *from, sw_process_t *to)
 *
 * Save current context to 'from', restore from 'to', jump to 'to'
 *
 * Register usage:
 *   %rdi = from process
 *   %rsi = to process
 *
 * sw_context_t layout (offsets from ctx start):
 *   0x00: rbx
 *   0x08: rbp
 *   0x10: r12
 *   0x18: r13
 *   0x20: r14
 *   0x28: r15
 *   0x30: rsp
 *   0x38: rip
 *   0x40: stack_limit
 *   0x48: stack_base
 */

#define CTX_OFFSET   0x70   /* offsetof(sw_process_t, ctx) */
#define ENTRY_OFFSET 0xC0   /* offsetof(sw_process_t, entry) */
#define ARG_OFFSET   0xC8   /* offsetof(sw_process_t, arg) */

CNAME(sw_context_swap):
    /* Save current context to 'from' */
    movq %rbx, CTX_OFFSET+0x00(%rdi)
    movq %rbp, CTX_OFFSET+0x08(%rdi)
    movq %r12, CTX_OFFSET+0x10(%rdi)
    movq %r13, CTX_OFFSET+0x18(%rdi)
    movq %r14, CTX_OFFSET+0x20(%rdi)
    movq %r15, CTX_OFFSET+0x28(%rdi)

    /* Save stack pointer */
    movq %rsp, CTX_OFFSET+0x30(%rdi)

    /* Save return address as rip */
    movq (%rsp), %rax
    movq %rax, CTX_OFFSET+0x38(%rdi)

    /* Restore 'to' context */
    movq CTX_OFFSET+0x00(%rsi), %rbx
    movq CTX_OFFSET+0x08(%rsi), %rbp
    movq CTX_OFFSET+0x10(%rsi), %r12
    movq CTX_OFFSET+0x18(%rsi), %r13
    movq CTX_OFFSET+0x20(%rsi), %r14
    movq CTX_OFFSET+0x28(%rsi), %r15

    /* Restore stack pointer */
    movq CTX_OFFSET+0x30(%rsi), %rsp

    /* Push return address and ret */
    movq CTX_OFFSET+0x38(%rsi), %rax
    pushq %rax

    ret

/*
 * sw_process_trampoline - Initial entry point for new processes
 *
 * When a context switch targets a newly spawned process for the first
 * time, execution lands here. The process pointer was stored in r12
 * during spawn (process_init sets ctx.r12 = process pointer).
 *
 * We load entry and arg from the process struct and call entry(arg).
 * When entry returns, we spin (scheduler handles cleanup).
 */
CNAME(sw_process_trampoline):
    /* r12 = process pointer (set during spawn via ctx.r12) */
    movq ARG_OFFSET(%r12), %rdi     /* arg → first argument */
    movq ENTRY_OFFSET(%r12), %rax   /* entry function pointer */
    callq *%rax                      /* call entry(arg) */

    /* entry() returned — context switch back to scheduler */
    movq %r12, %rdi                  /* proc pointer as arg1 */
    callq CNAME(sw_process_done)

    /* Should not reach here */
0:
    pause
    jmp 0b

/*
 * uint64_t sw_rdtsc(void)
 * Read timestamp counter for benchmarking
 */
CNAME(sw_rdtsc):
    rdtsc
    shlq $32, %rdx
    orq %rdx, %rax
    ret

#endif /* __x86_64__ */

/* ============================================================================
 * ARM64 Implementation
 * ============================================================================ */
#ifdef __aarch64__

.text
.globl CNAME(sw_context_swap)
.globl CNAME(sw_process_trampoline)
.globl CNAME(sw_rdtsc)

.align 4

/*
 * void sw_context_swap(sw_process_t *from, sw_process_t *to)
 *
 * x0 = from process
 * x1 = to process
 *
 * sw_context_t layout (offsets from ctx start):
 *   0x00: x19, x20
 *   0x10: x21, x22
 *   0x20: x23, x24
 *   0x30: x25, x26
 *   0x40: x27, x28
 *   0x50: x29 (fp), x30 (lr)
 *   0x60: sp
 *   0x68: pc (where to resume)
 *   0x70: stack_limit
 *   0x78: stack_base
 */

#define CTX_OFFSET   0x70    /* offsetof(sw_process_t, ctx) */
#define ENTRY_OFFSET 0xF0    /* offsetof(sw_process_t, entry) */
#define ARG_OFFSET   0xF8    /* offsetof(sw_process_t, arg) */

CNAME(sw_context_swap):
    /* Save callee-saved registers to 'from' */
    add x9, x0, #CTX_OFFSET

    stp x19, x20, [x9, #0x00]
    stp x21, x22, [x9, #0x10]
    stp x23, x24, [x9, #0x20]
    stp x25, x26, [x9, #0x30]
    stp x27, x28, [x9, #0x40]
    stp x29, x30, [x9, #0x50]

    /* Save stack pointer */
    mov x10, sp
    str x10, [x9, #0x60]

    /* Save where to resume (this function's return address) */
    adr x10, 1f
    str x10, [x9, #0x68]

    /* Restore 'to' context */
    add x9, x1, #CTX_OFFSET

    ldp x19, x20, [x9, #0x00]
    ldp x21, x22, [x9, #0x10]
    ldp x23, x24, [x9, #0x20]
    ldp x25, x26, [x9, #0x30]
    ldp x27, x28, [x9, #0x40]
    ldp x29, x30, [x9, #0x50]

    /* Restore stack pointer */
    ldr x10, [x9, #0x60]
    mov sp, x10

    /* Jump to saved PC */
    ldr x10, [x9, #0x68]
    br x10

1:  /* Resume point after context switch */
    ret

/*
 * sw_process_trampoline - Initial entry point for new processes (ARM64)
 *
 * When a context switch targets a newly spawned process for the first
 * time, execution lands here. The process pointer was stored in x19
 * during spawn (process_init sets ctx.x19 = process pointer).
 *
 * We load entry and arg from the process struct and call entry(arg).
 * When entry returns, we spin (scheduler handles cleanup).
 */
CNAME(sw_process_trampoline):
    /* x19 = process pointer (set during spawn via ctx.x19) */
    ldr x0, [x19, #ARG_OFFSET]     /* arg → first argument (x0) */
    ldr x1, [x19, #ENTRY_OFFSET]   /* entry function pointer */
    blr x1                           /* call entry(arg) */

    /* entry() returned — context switch back to scheduler */
    mov x0, x19                      /* proc pointer as arg1 */
    bl CNAME(sw_process_done)

    /* Should not reach here */
0:
    wfe
    b 0b

/* Read CNTPCT (virtual timer count) for ARM64 */
CNAME(sw_rdtsc):
    mrs x0, cntvct_el0
    ret

#endif /* __aarch64__ */
