/* =============================================================================
 * JARVIS AI OS — Kernel Linker Script
 * kernel/linker.ld
 *
 * Places kernel at 1 MB physical (0x100000), maps to higher-half
 * virtual address 0xFFFFFFFF80000000 (x86_64 convention).
 * ============================================================================= */

ENTRY(multiboot2_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    /* GRUB Multiboot2 entry code must be low and the header must be early. */
    . = 0x100000;

    .multiboot2 ALIGN(8) : {
        KEEP(*(.multiboot2))
    }

    .boot.text ALIGN(4K) : {
        *(.boot.text)
    }

    .boot.data ALIGN(4K) : {
        *(.boot.data)
    }

    .boot.bss ALIGN(4K) : {
        *(.boot.bss)
    }

    _kernel_phys_base = ALIGN(2M);

    /* Higher-half kernel virtual base */
    . = 0xFFFFFFFF80000000;

    _kernel_start = .;

    /* Executable code */
    .text ALIGN(4K) : AT(ADDR(.text) - 0xFFFFFFFF80000000 + _kernel_phys_base) {
        *(.text)
        *(.text.*)
    }

    /* Read-only data: string literals, vtables */
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - 0xFFFFFFFF80000000 + _kernel_phys_base) {
        *(.rodata)
        *(.rodata.*)
    }

    /* Initialized data */
    .data ALIGN(4K) : AT(ADDR(.data) - 0xFFFFFFFF80000000 + _kernel_phys_base) {
        *(.data)
        *(.data.*)
    }

    /* BSS: zero-init (physical allocator bitmap, etc.) */
    .bss ALIGN(4K) : AT(ADDR(.bss) - 0xFFFFFFFF80000000 + _kernel_phys_base) {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    _kernel_end = .;

    /* Discard debug sections to keep kernel small */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note.*)
        *(.comment)
    }
}
