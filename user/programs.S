/* Embeds the built user programs in the kernel image; the kernel copies
 * them into /jarvis/bin at boot (kernel/usermode.c). */
    .section .rodata
    .macro PROGRAM name
    .global user_prog_\name\()_start
    .global user_prog_\name\()_end
user_prog_\name\()_start:
    .incbin "user/\name\().elf"
user_prog_\name\()_end:
    .endm

    PROGRAM hello
    PROGRAM counter
    PROGRAM kernelpoke
    PROGRAM privileged
    .section .note.GNU-stack, "", @progbits
