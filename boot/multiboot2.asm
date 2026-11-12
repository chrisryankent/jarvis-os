; Multiboot2 entry path used by GRUB.
;
; GRUB enters here in 32-bit protected mode. This shim builds minimal page
; tables, enables long mode, then jumps to the 64-bit C kernel entry.

BITS 32

%define MULTIBOOT2_MAGIC    0xE85250D6
%define MULTIBOOT2_ARCH     0
%define KERNEL_VIRT_BASE    0xFFFFFFFF80000000

%define PTE_PRESENT         0x001
%define PTE_WRITABLE        0x002
%define PTE_HUGE            0x080

global multiboot2_start
extern jarvis_kernel_main
extern _kernel_phys_base

section .multiboot2 align=8
mb2_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd mb2_header_end - mb2_header_start
    dd -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + (mb2_header_end - mb2_header_start))

    ; Ask GRUB for a linear 32-bit framebuffer so the desktop has a real target.
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32
    align 8, db 0

    ; End tag
    dw 0
    dw 0
    dd 8
mb2_header_end:

section .boot.text align=16
multiboot2_start:
    cli
    mov esp, boot_stack_top

    ; Parse multiboot2 tags to extract framebuffer + memory map info
    ; (must do both before paging touches anything, and before EBX/the
    ; tag list potentially gets reclaimed once the PMM starts handing out
    ; frames over all of low memory).
    call parse_multiboot2_framebuffer
    call parse_multiboot2_memmap

    call setup_paging

    ; Enable PAE.
    mov eax, cr4
    or  eax, (1 << 5) | (1 << 9) | (1 << 10) ; PAE | OSFXSR | OSXMMEXCPT
    mov cr4, eax

    ; Enable SSE: clear CR0.EM and set CR0.MP.
    mov eax, cr0
    and eax, ~(1 << 2)
    or  eax, (1 << 1)
    mov cr0, eax

    ; Enable long mode.
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    lgdt [gdt64_ptr]

    ; Enable paging.
    mov eax, cr0
    or  eax, (1 << 31)
    mov cr0, eax

    jmp 0x08:long_mode_start

parse_multiboot2_framebuffer:
    ; RBX contains pointer to multiboot2 info
    ; This code runs in 32-bit protected mode, so use 32-bit registers
    ; Structure: total_size (4B) | reserved (4B) | tags...
    ; Tags at offset +8: [type (4B) | size (4B) | data...]
    ; Tag types: 0=end, 8=framebuffer

    ; Initialize GOP info at 0x7000 to zero (in case no FB tag is found)
    mov edi, 0x7000
    xor eax, eax
    mov ecx, (24 / 4)           ; sizeof(gop_info_t) / 4 = 6 dwords
    rep stosd

    lea eax, [ebx + 8]          ; First tag at RBX+8 (skip info header)
    mov edi, 0x7000             ; Write GOP info to fixed addr 0x7000
    xor edx, edx                ; EDX = 0 if no framebuffer found

.tag_loop:
    mov ecx, [eax + 0]          ; Load tag type
    cmp ecx, 0
    je .no_framebuffer          ; End of tags

    cmp ecx, 8
    je .found_fb_tag            ; Type 8 = framebuffer tag

    ; Skip to next tag: next = current + ((size + 7) & ~7)
    mov ecx, [eax + 4]          ; Load tag size
    add ecx, 7
    and ecx, ~7
    add eax, ecx
    jmp .tag_loop

.found_fb_tag:
    ; Multiboot2 framebuffer tag (32-bit mode, so read 32-bit chunks):
    ; +8 addr, +16 pitch, +20 width, +24 height, +28 bpp, +29 type.
    mov ecx, [eax + 8]          ; fb_addr low 32-bits
    mov [edi + 0], ecx
    mov ecx, [eax + 12]         ; fb_addr high 32-bits
    mov [edi + 4], ecx

    mov ecx, [eax + 20]         ; width (32-bit)
    mov [edi + 8], ecx

    mov ecx, [eax + 24]         ; height (32-bit)
    mov [edi + 12], ecx

    mov ecx, [eax + 16]         ; pitch (32-bit)
    mov [edi + 16], ecx

    movzx ecx, byte [eax + 28]  ; bits per pixel (8-bit)
    mov [edi + 20], ecx

    mov edx, 1                  ; EDX = 1 if found

.no_framebuffer:
    ret

; ---- Parse the multiboot2 memory map tag (type 6) -------------------------
; Writes a u32 entry count to 0x7100, followed by up to 128 entries of
; {base:u64, length:u64, type:u32} (20 bytes each, packed) starting at
; 0x7104 — this exactly matches kernel.h's `e820_entry_t` layout and
; MAX_E820, so kernel.c's existing memory_init() (which already walks
; g_kstate.e820[]/e820_count and falls back to a conservative 512 MB guess
; if e820_count is 0) can consume it directly with no format translation.
; Multiboot2's own mmap type values (1=available, 2=reserved, 3=ACPI
; reclaimable, 4=NVS, 5=defective) are numerically identical to this
; project's E820_USABLE/E820_RESERVED/E820_ACPI/E820_NVS — type 5 is
; clamped to E820_RESERVED since there's no direct equivalent.
;
; Before this existed, the PMM only ever knew about a single hardcoded
; 512 MB QEMU-sized region — correct by accident on a default QEMU VM,
; wrong (and either wasteful or unsafe) on any real machine or differently
; sized VM, since it never reflected how much RAM actually exists.
parse_multiboot2_memmap:
    push ebx
    push esi
    push edi
    push edx

    mov dword [0x7100], 0       ; default: 0 entries (fallback path handles this)

    lea eax, [ebx + 8]          ; first tag
    xor edi, edi                ; edi = entries written so far

.scan_tags:
    mov ecx, [eax + 0]          ; tag type
    cmp ecx, 0
    je .mm_done                 ; end tag reached, no memmap tag present
    cmp ecx, 6
    je .mm_found
    mov ecx, [eax + 4]          ; tag size
    add ecx, 7
    and ecx, ~7
    add eax, ecx
    jmp .scan_tags

.mm_found:
    mov ebx, [eax + 8]          ; entry_size (bytes per mmap entry, usually 24)
    mov edx, [eax + 4]          ; tag total size
    lea edx, [eax + edx]        ; edx = one-past-last-byte of this tag
    lea esi, [eax + 16]         ; esi -> first mmap entry

.mm_entry_loop:
    cmp esi, edx
    jae .mm_done
    cmp edi, 128                ; MAX_E820
    jae .mm_done

    mov eax, edi
    imul eax, eax, 20
    add eax, 0x7104              ; eax = dest slot for this entry

    mov ecx, [esi + 0]           ; base low
    mov [eax + 0], ecx
    mov ecx, [esi + 4]           ; base high
    mov [eax + 4], ecx
    mov ecx, [esi + 8]           ; length low
    mov [eax + 8], ecx
    mov ecx, [esi + 12]          ; length high
    mov [eax + 12], ecx

    mov ecx, [esi + 16]          ; multiboot2 region type
    cmp ecx, 4
    jbe .mm_type_ok
    mov ecx, 2                   ; clamp unknown types (e.g. 5=defective) to RESERVED
.mm_type_ok:
    mov [eax + 16], ecx

    inc edi
    add esi, ebx                 ; advance by entry_size
    jmp .mm_entry_loop

.mm_done:
    mov dword [0x7100], edi      ; final entry count
    pop edx
    pop edi
    pop esi
    pop ebx
    ret

setup_paging:
    ; Clear all boot page-table memory.
    mov edi, pml4_table
    xor eax, eax
    mov ecx, (page_tables_end - pml4_table) / 4
    rep stosd

    ; PML4[0] maps the low identity range for the transition code.
    mov eax, pdpt_low
    or  eax, PTE_PRESENT | PTE_WRITABLE
    mov [pml4_table + 0 * 8], eax

    ; PML4[511] maps the higher-half kernel base.
    mov eax, pdpt_high
    or  eax, PTE_PRESENT | PTE_WRITABLE
    mov [pml4_table + 511 * 8], eax

    ; Identity map 0..4 GiB via PDPT[0..3] so high MMIO framebuffers work.
    mov eax, pd_low_0
    or  eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdpt_low + 0 * 8], eax
    mov eax, pd_low_1
    or  eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdpt_low + 1 * 8], eax
    mov eax, pd_low_2
    or  eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdpt_low + 2 * 8], eax
    mov eax, pd_low_3
    or  eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdpt_low + 3 * 8], eax

    ; Map 0xFFFFFFFF80000000..+1 GiB to physical 0..1 GiB.
    mov eax, pd_high
    or  eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdpt_high + 510 * 8], eax

    ; Identity-map low memory with 2 MiB pages.
    xor eax, eax
    mov edi, pd_low_0
    call map_identity_pd
    mov edi, pd_low_1
    call map_identity_pd
    mov edi, pd_low_2
    call map_identity_pd
    mov edi, pd_low_3
    call map_identity_pd

    ; Map higher-half kernel virtual memory to its physical load address.
    mov eax, _kernel_phys_base
    mov edi, pd_high
    mov ecx, 512
.map_high_pd:
    mov edx, eax
    or  edx, PTE_PRESENT | PTE_WRITABLE | PTE_HUGE
    mov [edi], edx
    mov [edi + 4], dword 0
    add eax, 0x200000
    add edi, 8
    loop .map_high_pd

    mov eax, pml4_table
    mov cr3, eax
    ret

map_identity_pd:
    mov ecx, 512
.map_identity_pd_loop:
    mov edx, eax
    or  edx, PTE_PRESENT | PTE_WRITABLE | PTE_HUGE
    mov [edi], edx
    mov [edi + 4], dword 0
    add eax, 0x200000
    add edi, 8
    loop .map_identity_pd_loop
    ret

BITS 64
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    mov rax, jarvis_kernel_main
    call rax

.halt:
    hlt
    jmp .halt

section .boot.data align=8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF
gdt64_ptr:
    dw gdt64_ptr - gdt64 - 1
    dq gdt64

section .boot.bss nobits align=4096
pml4_table:
    resb 4096
pdpt_low:
    resb 4096
pdpt_high:
    resb 4096
pd_low_0:
    resb 4096
pd_low_1:
    resb 4096
pd_low_2:
    resb 4096
pd_low_3:
    resb 4096
pd_high:
    resb 4096
page_tables_end:

section .boot.bss nobits align=16
boot_stack:
    resb 16384
boot_stack_top:
