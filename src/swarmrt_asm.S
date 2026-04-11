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
 * x86_64 Implementation (System V AMD64 ABI — macOS + Linux)
 * ============================================================================ */
#if defined(__x86_64__) && !defined(_WIN32)

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

    /* Save stack pointer (return address is already at [rsp] from call) */
    movq %rsp, CTX_OFFSET+0x30(%rdi)

    /* Restore 'to' context */
    movq CTX_OFFSET+0x00(%rsi), %rbx
    movq CTX_OFFSET+0x08(%rsi), %rbp
    movq CTX_OFFSET+0x10(%rsi), %r12
    movq CTX_OFFSET+0x18(%rsi), %r13
    movq CTX_OFFSET+0x20(%rsi), %r14
    movq CTX_OFFSET+0x28(%rsi), %r15

    /* Restore stack pointer and return.
     * For resuming processes: [rsp] has the saved return address from `call`.
     * For new processes: [rsp] was pre-initialized with trampoline address. */
    movq CTX_OFFSET+0x30(%rsi), %rsp

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

#endif /* __x86_64__ && !_WIN32 */

/* ============================================================================
 * x86_64 Windows Implementation (Microsoft x64 ABI)
 *
 * Key differences from System V:
 * - Arguments in rcx, rdx (not rdi, rsi)
 * - Callee-saved: rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15
 * - 32-byte shadow space required before calls
 * - No leading underscore (same as Linux)
 *
 * We use MinGW/GAS syntax (.S file) — MASM would need a separate .asm.
 * MinGW-w64 and clang-cl both support .S with GAS syntax.
 *
 * sw_context_t layout for Windows x64 (offsets from ctx start):
 *   0x00: rbx
 *   0x08: rbp
 *   0x10: rdi       (callee-saved on Windows, not on SysV)
 *   0x18: rsi       (callee-saved on Windows, not on SysV)
 *   0x20: r12
 *   0x28: r13
 *   0x30: r14
 *   0x38: r15
 *   0x40: rsp
 *   0x48: rip
 *   0x50: xmm6-xmm15 (10 * 16 = 160 bytes)
 *   0xF0: stack_limit
 *   0xF8: stack_base
 *
 * NOTE: The context struct is larger on Windows due to xmm registers.
 * process_init must account for this when setting up new process contexts.
 * ============================================================================ */
#if defined(__x86_64__) && defined(_WIN32)

.text
.globl sw_context_swap
.globl sw_process_trampoline
.globl sw_rdtsc

.align 16

#define CTX_OFFSET   0x70
#define ENTRY_OFFSET 0xC0
#define ARG_OFFSET   0xC8

sw_context_swap:
    /* rcx = from process, rdx = to process (Windows x64 ABI) */

    /* Save callee-saved GPRs to 'from' */
    movq %rbx, CTX_OFFSET+0x00(%rcx)
    movq %rbp, CTX_OFFSET+0x08(%rcx)
    movq %rdi, CTX_OFFSET+0x10(%rcx)
    movq %rsi, CTX_OFFSET+0x18(%rcx)
    movq %r12, CTX_OFFSET+0x20(%rcx)
    movq %r13, CTX_OFFSET+0x28(%rcx)
    movq %r14, CTX_OFFSET+0x30(%rcx)
    movq %r15, CTX_OFFSET+0x38(%rcx)

    /* Save stack pointer */
    movq %rsp, CTX_OFFSET+0x40(%rcx)

    /* Save XMM6-XMM15 (callee-saved on Windows) */
    movaps %xmm6,  CTX_OFFSET+0x50(%rcx)
    movaps %xmm7,  CTX_OFFSET+0x60(%rcx)
    movaps %xmm8,  CTX_OFFSET+0x70(%rcx)
    movaps %xmm9,  CTX_OFFSET+0x80(%rcx)
    movaps %xmm10, CTX_OFFSET+0x90(%rcx)
    movaps %xmm11, CTX_OFFSET+0xA0(%rcx)
    movaps %xmm12, CTX_OFFSET+0xB0(%rcx)
    movaps %xmm13, CTX_OFFSET+0xC0(%rcx)
    movaps %xmm14, CTX_OFFSET+0xD0(%rcx)
    movaps %xmm15, CTX_OFFSET+0xE0(%rcx)

    /* Restore 'to' context */
    movq CTX_OFFSET+0x00(%rdx), %rbx
    movq CTX_OFFSET+0x08(%rdx), %rbp
    movq CTX_OFFSET+0x10(%rdx), %rdi
    movq CTX_OFFSET+0x18(%rdx), %rsi
    movq CTX_OFFSET+0x20(%rdx), %r12
    movq CTX_OFFSET+0x28(%rdx), %r13
    movq CTX_OFFSET+0x30(%rdx), %r14
    movq CTX_OFFSET+0x38(%rdx), %r15

    /* Restore XMM6-XMM15 */
    movaps CTX_OFFSET+0x50(%rdx), %xmm6
    movaps CTX_OFFSET+0x60(%rdx), %xmm7
    movaps CTX_OFFSET+0x70(%rdx), %xmm8
    movaps CTX_OFFSET+0x80(%rdx), %xmm9
    movaps CTX_OFFSET+0x90(%rdx), %xmm10
    movaps CTX_OFFSET+0xA0(%rdx), %xmm11
    movaps CTX_OFFSET+0xB0(%rdx), %xmm12
    movaps CTX_OFFSET+0xC0(%rdx), %xmm13
    movaps CTX_OFFSET+0xD0(%rdx), %xmm14
    movaps CTX_OFFSET+0xE0(%rdx), %xmm15

    /* Restore stack pointer and return */
    movq CTX_OFFSET+0x40(%rdx), %rsp
    ret

sw_process_trampoline:
    /* r12 = process pointer (set during spawn) */
    movq ARG_OFFSET(%r12), %rcx     /* arg → first argument (rcx on Windows) */
    movq ENTRY_OFFSET(%r12), %rax   /* entry function pointer */

    /* Allocate 32-byte shadow space before call (Windows ABI requirement) */
    subq $32, %rsp
    callq *%rax
    addq $32, %rsp

    /* entry() returned */
    movq %r12, %rcx                  /* proc pointer as arg1 */
    subq $32, %rsp
    callq sw_process_done
    addq $32, %rsp

0:
    pause
    jmp 0b

sw_rdtsc:
    rdtsc
    shlq $32, %rdx
    orq %rdx, %rax
    ret

#endif /* __x86_64__ && _WIN32 */

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
