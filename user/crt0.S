/* Program entry: call main(), then exit with its return value. */
    .section .text
    .global _start
_start:
    xor  %ebp, %ebp
    and  $-16, %rsp
    call main
    mov  %eax, %edi
    mov  $60, %eax
    syscall
1:  jmp  1b
    .section .note.GNU-stack, "", @progbits
