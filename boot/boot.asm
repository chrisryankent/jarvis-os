; =============================================================================
; JARVIS OS BOOTLOADER — Stage 1
; Assembled with NASM. Loaded by UEFI/BIOS at 0x7C00.
; Hands off to stage2 which loads the C kernel.
; =============================================================================
BITS 16
ORG  0x7C00

BOOT_SEG    EQU 0x07C0
STACK_TOP   EQU 0x9000
STAGE2_SEG  EQU 0x0800
STAGE2_OFF  EQU 0x0000
STAGE2_LBA  EQU 1          ; LBA sector where stage2 starts
STAGE2_SECS EQU 8          ; number of sectors for stage2

start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, STACK_TOP
    sti

    ; Save boot drive
    mov  [boot_drive], dl

    ; Print JARVIS banner
    mov  si, msg_boot
    call print_str

    ; Load stage2 via LBA (disk extension INT 13h)
    call load_stage2
    jc   disk_error

    ; Far jump into stage2 (16-bit protected mode setup there)
    jmp  STAGE2_SEG:STAGE2_OFF

disk_error:
    mov  si, msg_err
    call print_str
    hlt

; ---- Load stage2 with INT 13h extended read --------------------------------
load_stage2:
    mov  ah, 0x42
    mov  dl, [boot_drive]
    mov  si, dap
    int  0x13
    ret

; ---- Print null-terminated string (SI -> string) ---------------------------
print_str:
    lodsb
    test al, al
    jz   .done
    mov  ah, 0x0E
    int  0x10
    jmp  print_str
.done:
    ret

; ---- Disk Address Packet for LBA read --------------------------------------
dap:
    db  0x10            ; size of DAP
    db  0x00            ; reserved
    dw  STAGE2_SECS     ; sectors to read
    dw  STAGE2_OFF      ; dest offset
    dw  STAGE2_SEG      ; dest segment
    dq  STAGE2_LBA      ; start LBA

boot_drive: db 0

msg_boot: db "JARVIS OS Bootloader v0.1", 0x0D, 0x0A, 0
msg_err:  db "DISK READ ERROR", 0x0D, 0x0A, 0

times 510-($-$$) db 0
dw 0xAA55               ; boot signature
