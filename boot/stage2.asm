BITS 16
ORG 0x7E00

; ---------------------------------------------------------------------------
; BareMetal_x86 - stage 2
;
; Runs in real mode, loaded by stage 1 at 0x7E00. Initialises COM1, loads the
; kernel image to 0x10000 through the BIOS extended disk read, enables A20,
; installs a flat GDT and enters 32-bit protected mode.
; ---------------------------------------------------------------------------

STAGE2_SECTORS  equ 8
KERNEL_LBA      equ 1 + STAGE2_SECTORS
KERNEL_SEG      equ 0x1000              ; physical 0x10000
KERNEL_CHUNK    equ 64                  ; sectors per read (32 KiB)
KERNEL_SEG_STEP equ KERNEL_CHUNK * 32   ; 0x0800 paragraphs
BOOT_DRIVE_ADDR equ 0x0500
COM1            equ 0x3F8

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    call serial_init

    mov si, msg_banner
    call puts

    mov si, kdap_low
    mov ah, 0x42
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jc  .disk_failed

    mov si, kdap_high
    mov ah, 0x42
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jc  .disk_failed

    mov si, msg_loaded
    call puts

    in  al, 0x92                    ; fast A20 gate
    or  al, 0x02
    and al, 0xFE
    out 0x92, al

    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

.disk_failed:
    mov si, msg_failed
    call puts
    jmp halt

; ---------------------------------------------------------------------------
serial_init:
    mov dx, COM1 + 1                ; no UART interrupts
    xor al, al
    out dx, al
    mov dx, COM1 + 3                ; DLAB on
    mov al, 0x80
    out dx, al
    mov dx, COM1                    ; divisor 1 = 115200 baud
    mov al, 1
    out dx, al
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3                ; 8N1, DLAB off
    mov al, 0x03
    out dx, al
    mov dx, COM1 + 2                ; FIFO enable and clear
    mov al, 0xC7
    out dx, al
    mov dx, COM1 + 4                ; DTR, RTS, OUT2
    mov al, 0x0B
    out dx, al
    ret

serial_putc:
    push dx
    push ax
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, COM1
    out dx, al
    pop dx
    ret

puts:
    lodsb
    test al, al
    jz  .done
    call serial_putc
    jmp puts
.done:
    ret

halt:
    cli
    hlt
    jmp halt

msg_banner: db 13, 10, "stage2: real mode up, COM1 at 115200 8N1", 13, 10, 0
msg_loaded: db "stage2: kernel loaded at 0x10000, entering protected mode", 13, 10, 0
msg_failed: db "stage2: kernel read failed", 13, 10, 0

align 4
kdap_low:
    db 0x10, 0
    dw KERNEL_CHUNK
    dw 0x0000
    dw KERNEL_SEG
    dq KERNEL_LBA

kdap_high:
    db 0x10, 0
    dw KERNEL_CHUNK
    dw 0x0000
    dw KERNEL_SEG + KERNEL_SEG_STEP
    dq KERNEL_LBA + KERNEL_CHUNK

align 8
gdt_start:
    dq 0
gdt_code:                           ; 0x08: flat 32-bit code, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
gdt_data:                           ; 0x10: flat 32-bit data, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ---------------------------------------------------------------------------
BITS 32

pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov ebp, esp
    jmp KERNEL_SEG * 16

times 4096-($-$$) db 0
