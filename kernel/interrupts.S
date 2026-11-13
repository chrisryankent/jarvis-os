/* =============================================================================
 * JARVIS AI OS — kernel/interrupts.S
 *
 * Real x86_64 interrupt entry stubs. Without this file, idt_init()/
 * idt_set_handler()/pic_remap()/pit_set_frequency() in platform.c were all
 * empty stubs — there was no IDT, so the CPU had nowhere to dispatch
 * interrupts to, and irq0_timer_handler() (which drives ai_scheduler_tick(),
 * which is what actually runs ai_brain_thread/speech_stt_thread/etc.) was
 * never invoked by real hardware.
 *
 * Design: generate one tiny stub per vector (0-255) that pushes the vector
 * number (and a dummy error code for vectors the CPU doesn't push one for),
 * then jumps to a single common handler. The common handler saves every
 * general-purpose register, calls interrupt_dispatch(vector, error_code) in
 * platform.c, restores registers, and iretq's back. This is the standard
 * pattern used by most teaching/hobby kernels (xv6, JamesM's tutorial,
 * OSDev's Bare Bones) — mechanical and low-risk because every stub is
 * identical in shape; the only real risk in this kind of code is a wrong
 * IDT *descriptor* (segment selector / gate type byte), which is built in
 * C in platform.c and reviewed separately.
 *
 * Written using GNU as's `.intel_syntax noprefix` + `.altmacro` rather than
 * NASM, so kernel/ stays on a single assembler (as, matching
 * context_switch.S) instead of adding nasm as a second build dependency
 * just for this directory — nasm is already used for boot/, which is a
 * separate part of the project.
 *
 * CPU exception vectors that push their own error code onto the stack:
 *   8 (#DF), 10 (#TS), 11 (#NP), 12 (#SS), 13 (#GP), 14 (#PF), 17 (#AC),
 *   21 (#CP), 29 (#VC), 30 (#SX). Every other vector gets a dummy 0 pushed
 *   so the common handler can always pop the same shape off the stack.
 * ============================================================================= */

.intel_syntax noprefix
.altmacro

.extern interrupt_dispatch
.global isr_stub_table
.global isr_common_stub

.section .text
.align 16

/* ---- Common handler --------------------------------------------------------
 * Stack on entry (low to high address, after the per-vector stub pushed
 * vector_num then error_code is already below it from either the CPU or
 * our own dummy push): [vector_num][error_code][RIP][CS][RFLAGS][RSP][SS]
 * (the last 5 are what the CPU itself pushed on the interrupt). */
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    /* 15 GP registers pushed above. vector_num sits at rsp+15*8 (pushed
     * last by the per-vector stub, so closest to rsp); error_code sits
     * right above it at rsp+16*8. */
    mov rdi, [rsp + 15*8]    /* arg1 = vector_num   */
    mov rsi, [rsp + 16*8]    /* arg2 = error_code   */
    mov rdx, rsp             /* arg3 = saved frame: 15 registers, vector,
                                error code, then rip/cs/rflags/rsp/ss */

    mov rax, ds
    push rax
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    call interrupt_dispatch

    pop rax
    mov ds, ax
    mov es, ax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16               /* discard vector_num + error_code */
    iretq

/* ---- Macros to generate the 256 per-vector entry stubs -------------------- */
.macro ISR_NOERR num
isr_stub_\num:
    push 0          /* dummy error code */
    push \num        /* vector number    */
    jmp isr_common_stub
.endm

.macro ISR_ERR num
isr_stub_\num:
    push \num        /* vector number (CPU already pushed the error code) */
    jmp isr_common_stub
.endm

/* Vectors with a CPU-pushed error code */
ISR_ERR 8
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_ERR 17
ISR_ERR 21
ISR_ERR 29
ISR_ERR 30

/* Every other vector (0-255 minus the 10 above) gets the no-error-code form. */
.set i, 0
.rept 256
  .if (i != 8) && (i != 10) && (i != 11) && (i != 12) && (i != 13) && (i != 14) && (i != 17) && (i != 21) && (i != 29) && (i != 30)
    ISR_NOERR %i
  .endif
  .set i, i+1
.endr

/* ---- Table of all 256 stub addresses, read by idt_init() in platform.c --- */
.macro EMIT_PTR num
    .quad isr_stub_\num
.endm

.section .data
.align 8
isr_stub_table:
.set i, 0
.rept 256
    EMIT_PTR %i
  .set i, i+1
.endr

.section .note.GNU-stack, "", @progbits
