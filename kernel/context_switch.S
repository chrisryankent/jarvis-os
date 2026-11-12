/* =============================================================================
 * JARVIS AI OS — Kernel Context Switch
 * kernel/context_switch.S
 *
 * void context_switch_asm(uint64_t *save_rsp_here, uint64_t *load_rsp_from);
 *
 * SysV AMD64 calling convention: rdi = save_rsp_here, rsi = load_rsp_from.
 *
 * This is the piece that was missing from the previous C implementation:
 * that version only did "mov %rsp, [from]; mov [to], %rsp" — it swapped the
 * stack pointer but never saved or restored rbp/rbx/r12-r15, so on every
 * switch the *next* thread to run would silently inherit whatever garbage
 * was sitting in those registers, and the thread being switched away from
 * would, whenever it resumed, find its own callee-saved registers
 * clobbered. This version pushes/pops all six callee-saved registers
 * around the stack swap, which is the standard, correct pattern.
 *
 * kthread_create() in ipc_sched_pmm.c builds a brand-new thread's initial
 * stack to look exactly like what this function would have produced if it
 * had "saved" a thread that was about to start at its entry point — i.e.
 * six zeroed register slots followed by the entry function's address as
 * the return address. If you change the push order below, you MUST update
 * that stack layout to match, or new threads will pop garbage into their
 * registers and `ret` to the wrong place.
 *
 * Deliberately does NOT touch CR3 — that decision needs the C-side
 * process_t (to know whether the destination even has its own address
 * space, and to avoid writing 0 into CR3, which would triple-fault real
 * hardware), so it's handled in context_switch() in ipc_sched_pmm.c
 * immediately before this is called.
 * ============================================================================= */

    .section .text
    .global context_switch_asm
    .type context_switch_asm, @function

context_switch_asm:
    /* Save the caller's (outgoing thread's) callee-saved registers. */
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    /* Stash the now-fully-saved stack pointer into *save_rsp_here. */
    mov  %rsp, (%rdi)

    /* Switch to the incoming thread's stack. */
    mov  (%rsi), %rsp

    /* Restore its callee-saved registers (mirrors kthread_create()'s
     * initial frame for a never-before-run thread, or a previous call to
     * this exact function for a thread that's been switched out before). */
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %rbx
    pop  %rbp

    /* Pops the return address left on the incoming stack and jumps to it —
     * either back into whoever last called context_switch() for this
     * thread, or (for a brand new thread) straight into its entry
     * function, per kthread_create()'s frame layout. */
    ret

    .size context_switch_asm, . - context_switch_asm

    .section .note.GNU-stack, "", @progbits

