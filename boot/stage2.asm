; =============================================================================
; JARVIS OS — Stage 2 Bootloader
; Switches CPU from real mode → protected mode → long mode (64-bit)
; then calls the C kernel entry point: jarvis_kernel_main()
; =============================================================================
BITS 16
ORG  0x0000             ; we're loaded at segment 0x0800, offset 0x0000

KERNEL_LOAD_ADDR EQU 0x100000  ; 1 MB mark — kernel physical address

stage2_start:
    cli
    ; Enable A20 line (keyboard controller method)
    call enable_a20

    ; Load 32-bit GDT
    lgdt [gdtr32]

    ; Enter protected mode
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:pm_entry

BITS 32
pm_entry:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x90000

    ; Load the 64-bit kernel from disk into KERNEL_LOAD_ADDR
    ; (in a real impl, this calls a disk read routine or UEFI LoadImage)
    call load_kernel_image

    ; Set up page tables for long mode
    call setup_paging

    ; Load 64-bit GDT
    lgdt [gdtr64]

    ; Enable long mode (EFER.LME)
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, (1 << 8)
    wrmsr

    ; Activate paging + protected mode
    mov  eax, cr0
    or   eax, (1 << 31)
    mov  cr0, eax

    jmp  0x18:lm_entry

BITS 64
lm_entry:
    ; Update segment regs for long mode
    mov  ax, 0x20
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    xor  ax, ax
    mov  fs, ax
    mov  gs, ax

    ; Call the loaded kernel image. Flat binary output cannot carry extern refs.
    mov  rax, KERNEL_LOAD_ADDR
    call rax
    hlt

; ---- Enable A20 via keyboard controller ------------------------------------
BITS 32
enable_a20:
    in   al, 0x92
    or   al, 2
    out  0x92, al
    ret

; ---- Minimal page table: identity-map first 2 GB --------------------------
setup_paging:
    ; Clear PML4 table at 0x1000
    mov  edi, 0x1000
    xor  eax, eax
    mov  ecx, 4096
    rep  stosd

    ; PML4[0] → PDPT at 0x2000
    mov  dword [0x1000], 0x2003
    ; PDPT[0] → PD at 0x3000
    mov  dword [0x2000], 0x3003
    ; PD: 512 × 2 MB pages (identity map)
    mov  edi, 0x3000
    mov  eax, 0x83          ; present | writable | 2MB page
    mov  ecx, 512
.pd_loop:
    mov  [edi], eax
    add  eax, 0x200000
    add  edi, 8
    loop .pd_loop

    ; Load CR3
    mov  eax, 0x1000
    mov  cr3, eax

    ; Enable PAE
    mov  eax, cr4
    or   eax, (1 << 5)
    mov  cr4, eax
    ret

; ---- Stub: in production this loads ELF kernel from disk -------------------
load_kernel_image:
    ; TODO: INT 13h or UEFI file protocol to read kernel.elf
    ret

; ---- GDT (32-bit) ----------------------------------------------------------
ALIGN 8
gdt32:
    dq 0x0000000000000000  ; null
    dq 0x00CF9A000000FFFF  ; code 32-bit
    dq 0x00CF92000000FFFF  ; data 32-bit
gdtr32:
    dw $ - gdt32 - 1
    dd gdt32

; ---- GDT (64-bit) ----------------------------------------------------------
ALIGN 8
gdt64:
    dq 0x0000000000000000  ; null
    dq 0x00CF9A000000FFFF  ; code 32-bit compat
    dq 0x00CF92000000FFFF  ; data 32-bit compat
    dq 0x00AF9A000000FFFF  ; code 64-bit
    dq 0x00AF92000000FFFF  ; data 64-bit
gdtr64:
    dw $ - gdt64 - 1
    dq gdt64
