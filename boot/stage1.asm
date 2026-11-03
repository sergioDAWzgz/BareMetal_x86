BITS 16
ORG 0x7C00

; ---------------------------------------------------------------------------
; BareMetal_x86 - stage 1 (MBR)
;
; Loads stage 2 (LBA 1) through the BIOS extended disk read and jumps to it.
; Leaves the boot drive number at 0x0500 for stage 2.
; ---------------------------------------------------------------------------

STAGE2_LBA      equ 1
STAGE2_SECTORS  equ 8               ; 4 KiB, must match stage2 padding
STAGE2_SEG      equ 0x07E0          ; physical 0x7E00
BOOT_DRIVE_ADDR equ 0x0500

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [BOOT_DRIVE_ADDR], dl

    mov ah, 0x41                    ; EDD presence check
    mov bx, 0x55AA
    int 0x13
    jc  .failed
    cmp bx, 0xAA55
    jne .failed

    mov si, dap
    mov ah, 0x42
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jc  .failed

    mov dl, [BOOT_DRIVE_ADDR]
    jmp STAGE2_SEG:0

.failed:
    mov si, msg_failed
    call print
    jmp halt

; ---------------------------------------------------------------------------
print:
    lodsb
    test al, al
    jz  .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

halt:
    cli
    hlt
    jmp halt

msg_failed: db "stage1: disk read failed", 0

align 4
dap:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw 0x0000
    dw STAGE2_SEG
    dq STAGE2_LBA

times 510-($-$$) db 0
dw 0xAA55
