/* =============================================================================
 * JARVIS AI OS - syscall entry placeholder
 *
 * syscall.c exports syscall_init(), but the current boot path never calls it:
 * the OS is still running entirely in ring 0 and has no user-mode stack/TSS/GS
 * setup for a real SYSCALL/SYSRET transition yet.
 *
 * The root Makefile links a syscall_entry.o because syscall.c references the
 * symbol from syscall_init(). Provide a loud, non-returning target so the kernel
 * can link today, while any accidental attempt to install and execute the fast
 * syscall path halts immediately instead of returning through an invalid stack.
 * ============================================================================= */

    .section .text
    .global syscall_entry_stub
    .type syscall_entry_stub, @function

syscall_entry_stub:
    cli
1:
    hlt
    jmp 1b

    .size syscall_entry_stub, . - syscall_entry_stub

    .section .note.GNU-stack, "", @progbits
