/* =============================================================================
 * JARVIS AI OS — privilege transitions (kernel/syscall_entry.S)
 *
 *   syscall_entry_stub   user mode -> kernel mode, via the SYSCALL instruction
 *   usermode_enter       kernel mode -> user mode, for a new program
 *   gdt_flush            load the GDT (kernel + user segments) and the TSS
 *
 * SYSCALL does not switch stacks, so the stub moves to the calling process's
 * kernel stack itself. syscall_kernel_rsp is kept pointing at the top of the
 * current process's kernel stack by the scheduler (single CPU, so one global
 * is enough; the user stack pointer is pushed immediately, before interrupts
 * are re-enabled, so a process switch during the call cannot lose it).
 *
 * Register convention (same as Linux x86-64):
 *   rax = call number; rdi, rsi, rdx, r10, r8, r9 = arguments; rax = result.
 *   rcx and r11 are overwritten by the CPU (return address and flags).
 * ============================================================================= */
    .section .data
    .global syscall_kernel_rsp
syscall_kernel_rsp: .quad 0
syscall_user_rsp:   .quad 0

    .section .text
    .global syscall_entry_stub
    .type syscall_entry_stub, @function
syscall_entry_stub:
    /* Interrupts are off here (FMASK clears IF). */
    mov  %rsp, syscall_user_rsp(%rip)
    mov  syscall_kernel_rsp(%rip), %rsp
    push syscall_user_rsp(%rip)     /* user stack pointer   */
    push %rcx                       /* user return address  */
    push %r11                       /* user flags           */
    push %rdi                       /* As on Linux, a system call changes only */
    push %rsi                       /* rax, rcx and r11: compiled programs     */
    push %rdx                       /* rely on every other register surviving. */
    push %r10
    push %r8
    push %r9
    push %r9                        /* 7th C argument (a6); keeps rsp 16-aligned */

    /* syscall_handler(nr, a1, a2, a3, a4, a5, a6) */
    mov  %r8,  %r9
    mov  %r10, %r8
    mov  %rdx, %rcx
    mov  %rsi, %rdx
    mov  %rdi, %rsi
    mov  %rax, %rdi

    sti                             /* system calls are preemptible */
    call syscall_handler
    cli

    add  $8, %rsp
    pop  %r9
    pop  %r8
    pop  %r10
    pop  %rdx
    pop  %rsi
    pop  %rdi
    pop  %r11
    pop  %rcx
    pop  %rsp
    sysretq                         /* back to user mode */
    .size syscall_entry_stub, . - syscall_entry_stub

/* void usermode_enter(uint64_t entry, uint64_t user_stack);  never returns */
    .global usermode_enter
    .type usermode_enter, @function
usermode_enter:
    cli
    mov  $0x1B, %ax                 /* user data segment, privilege level 3 */
    mov  %ax, %ds
    mov  %ax, %es
    push $0x1B                      /* ss     */
    push %rsi                       /* rsp    */
    push $0x202                     /* rflags: interrupts on */
    push $0x23                      /* cs: user code, privilege level 3 */
    push %rdi                       /* rip    */
    xor  %eax, %eax
    xor  %ebx, %ebx
    xor  %ecx, %ecx
    xor  %edx, %edx
    xor  %esi, %esi
    xor  %edi, %edi
    xor  %ebp, %ebp
    xor  %r8d, %r8d
    xor  %r9d, %r9d
    xor  %r10d, %r10d
    xor  %r11d, %r11d
    xor  %r12d, %r12d
    xor  %r13d, %r13d
    xor  %r14d, %r14d
    xor  %r15d, %r15d
    iretq
    .size usermode_enter, . - usermode_enter

/* void gdt_flush(void *gdt_pointer); */
    .global gdt_flush
    .type gdt_flush, @function
gdt_flush:
    lgdt (%rdi)
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  %ax, %fs
    mov  %ax, %gs
    pushq $0x08
    lea  1f(%rip), %rax
    push %rax
    lretq                           /* reload cs */
1:
    mov  $0x28, %ax
    ltr  %ax                        /* task state segment */
    ret
    .size gdt_flush, . - gdt_flush

    .section .note.GNU-stack, "", @progbits
