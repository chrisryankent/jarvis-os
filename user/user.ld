/* User programs load at 0x8000400000: inside the per-process part of the
 * address space (the kernel owns everything below 0x8000000000). Each
 * segment starts on its own page so it can have its own protection. */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS
{
    . = 0x8000400000;
    .text   : { *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(4096);
    .data   : { *(.data .data.*) } :data
    .bss    : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
