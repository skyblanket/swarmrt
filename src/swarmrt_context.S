/*
 * SwarmRT-BEAM Assembly Context Switching
 *
 * Fast user-space context switching for x86_64 and ARM64
 * Based on BEAM's process_main context switching
 *
 * Performance target: ~100-200ns (faster than BEAM's ~300ns)
 */

#ifdef __APPLE__
#define CNAME(name) _##name
#else
#define CNAME(name) name
#endif

#ifdef __x86_64__
/* ============================================================================
 * x86_64 Context Switch
 *
 * Callee-saved registers (must preserve):
 *   rbx, rbp, r12-r15
 * Plus: rsp (stack pointer), rip (return address)
 *
 * Calling convention:
 *   rdi = from process (sw_beam_process_t*)
 *   rsi = to process (sw_beam_process_t*)
 * ============================================================================ */

.text
.globl CNAME(sw_beam_context_swap_asm)
.align 16

CNAME(sw_beam_context_swap_asm):
    /* Get pointer to env buffer from process struct */
    leaq 256(%rdi), %r8        /* &from->env (env is at offset ~256) */

    /* Save callee-saved registers */
    movq %rbx, 0(%r8)
    movq %rbp, 8(%r8)
    movq %r12, 16(%r8)
    movq %r13, 24(%r8)
    movq %r14, 32(%r8)
    movq %r15, 40(%r8)

    /* Save stack pointer */
    movq %rsp, 48(%r8)

    /* Save return address (rip) */
    movq (%rsp), %rax
    movq %rax, 56(%r8)

    /* Now restore 'to' process context */
    leaq 256(%rsi), %r8        /* &to->env */

    /* Restore callee-saved registers */
    movq 0(%r8), %rbx
    movq 8(%r8), %rbp
    movq 16(%r8), %r12
    movq 24(%r8), %r13
    movq 32(%r8), %r14
    movq 40(%r8), %r15

    /* Restore stack pointer */
    movq 48(%r8), %rsp

    /* Push return address for ret */
    movq 56(%r8), %rax
    pushq %rax

    /* Return to 'to' process */
    ret

/* ============================================================================
 * Get current time stamp counter for benchmarking
 * ============================================================================ */
.globl CNAME(sw_beam_rdtsc)
.align 16

CNAME(sw_beam_rdtsc):
    rdtsc                      /* Returns TSC in edx:eax */
    shlq $32, %rdx
    orq %rdx, %rax
    ret

#elif defined(__aarch64__)
/* ============================================================================
 * ARM64 Context Switch
 *
 * Callee-saved registers: x19-x28, fp (x29), lr (x30), sp
 * ============================================================================ */

.text
.globl CNAME(sw_beam_context_swap_asm)
.align 4

CNAME(sw_beam_context_swap_asm):
    /* x0 = from process, x1 = to process */

    /* Get pointer to env buffer */
    add x9, x0, #256           /* &from->env */

    /* Save callee-saved registers */
    stp x19, x20, [x9, #0]
    stp x21, x22, [x9, #16]
    stp x23, x24, [x9, #32]
    stp x25, x26, [x9, #48]
    stp x27, x28, [x9, #64]
    stp x29, x30, [x9, #80]    /* fp, lr */
    mov x10, sp
    str x10, [x9, #96]         /* sp */

    /* Save where we want to resume */
    adr x10, 1f
    str x10, [x9, #104]        /* pc */

    /* Now restore 'to' process */
    add x9, x1, #256           /* &to->env */

    /* Restore callee-saved registers */
    ldp x19, x20, [x9, #0]
    ldp x21, x22, [x9, #16]
    ldp x23, x24, [x9, #32]
    ldp x25, x26, [x9, #48]
    ldp x27, x28, [x9, #64]
    ldp x29, x30, [x9, #80]    /* fp, lr */
    ldr x10, [x9, #96]
    mov sp, x10                /* sp */

    /* Jump to saved PC or lr */
    ldr x10, [x9, #104]
    br x10

1:  /* Label for saving resume point */
    ret

/* Get CNTPCT (virtual timer) for ARM64 */
.globl CNAME(sw_beam_read_cntpct)
.align 4

CNAME(sw_beam_read_cntpct):
    mrs x0, cntvct_el0
    ret

#endif
