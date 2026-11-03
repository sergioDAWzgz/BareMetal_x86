BITS 16
ORG 0x7E00

; ---------------------------------------------------------------------------
; BareMetal_x86 - stage 2
;
; Runs in real mode, loaded by stage 1 at 0x7E00. It
;   1. initialises COM1 and reports progress,
;   2. collects the BIOS E820 memory map for the kernel,
;   3. loads the kernel image to physical 0x10000,
;   4. enables A20, installs a GDT and enters 32-bit protected mode,
;   5. builds 4-level page tables that map the first GiB both identity and at
;      the higher half base 0xFFFFFFFF80000000,
;   6. enables long mode and jumps to the kernel virtual entry.
; ---------------------------------------------------------------------------

STAGE2_SECTORS  equ 16
KERNEL_LBA      equ 1 + STAGE2_SECTORS
KERNEL_PHYS     equ 0x10000
KERNEL_VIRT     equ 0xFFFFFFFF80010000
KERNEL_SEG      equ 0x1000              ; physical 0x10000
KERNEL_CHUNK    equ 64                  ; sectors per read (32 KiB)
KERNEL_SEG_STEP equ KERNEL_CHUNK * 32   ; 0x0800 paragraphs

BOOT_DRIVE_ADDR equ 0x0500
E820_COUNT_ADDR equ 0x0504
E820_BUF_ADDR   equ 0x0508
E820_MAX        equ 32
E820_ENTRY_SIZE equ 24

COM1            equ 0x3F8

PML4_ADDR       equ 0x70000
PDPT_ADDR       equ 0x71000
PD_ADDR         equ 0x72000

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

    call collect_memory_map
    mov si, msg_memory
    call puts

    call check_long_mode

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
    jmp 0x18:pm_entry

.disk_failed:
    mov si, msg_failed
    call puts
    jmp halt

; ---------------------------------------------------------------------------
; collect_memory_map: fills E820_COUNT_ADDR and the entry buffer at
; E820_BUF_ADDR. Leaves the count at zero when the BIOS has no E820 support.
; ---------------------------------------------------------------------------
collect_memory_map:
    xor ebx, ebx                    ; E820 continuation starts at zero
    xor bp, bp
.loop:
    movzx eax, bp
    mov ecx, E820_ENTRY_SIZE
    mul ecx
    mov edi, E820_BUF_ADDR
    add edi, eax

    mov eax, 0xE820
    mov edx, 0x534D4150             ; 'SMAP'
    mov ecx, E820_ENTRY_SIZE
    int 0x15
    jc  .done
    cmp eax, 0x534D4150
    jne .done

    inc bp
    test ebx, ebx
    jz  .done
    cmp bp, E820_MAX
    jb  .loop

.done:
    movzx eax, bp
    mov [E820_COUNT_ADDR], eax
    ret

; ---------------------------------------------------------------------------
; check_long_mode: halts with a readable message when the CPU cannot run 64-bit
; code, instead of triple faulting later.
; ---------------------------------------------------------------------------
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb  .unsupported

    mov eax, 0x80000001
    cpuid
    test edx, 0x20000000            ; EDX bit 29 = long mode
    jz  .unsupported
    ret

.unsupported:
    mov si, msg_no_long_mode
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
msg_memory: db "stage2: BIOS memory map collected", 13, 10, 0
msg_loaded: db "stage2: kernel loaded at 0x10000", 13, 10, 0
msg_no_long_mode: db "stage2: this CPU does not support long mode, halting", 13, 10, 0
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
gdt_code64:                         ; 0x08: 64-bit code, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00
gdt_data:                           ; 0x10: flat data, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_code32:                         ; 0x18: flat 32-bit code, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
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

    ; --- zero the page tables ---
    mov edi, PML4_ADDR
    mov ecx, (3 * 4096) / 4
    xor eax, eax
    cld
    rep stosd

    mov eax, PD_ADDR + 0x3          ; present | writable
    mov [PDPT_ADDR], eax            ; PDPT[0]   -> PD
    mov [PDPT_ADDR + 510*8], eax    ; PDPT[510] -> PD

    mov eax, PDPT_ADDR + 0x3
    mov [PML4_ADDR], eax            ; PML4[0]   -> PDPT (identity)
    mov [PML4_ADDR + 511*8], eax    ; PML4[511] -> PDPT (higher half)

    ; --- PD: 512 x 2 MiB pages covering the first GiB ---
    mov edi, PD_ADDR
    mov eax, 0x83                   ; present | writable | page size
    mov ecx, 512
.fill_pd:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    ; --- long mode bring-up ---
    mov eax, cr4
    or  eax, 0x20                   ; CR4.PAE
    mov cr4, eax

    mov eax, PML4_ADDR
    mov cr3, eax

    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or  eax, 0x100                  ; EFER.LME
    wrmsr

    mov eax, cr0
    or  eax, 0x80000000             ; CR0.PG
    mov cr0, eax

    jmp 0x08:long_entry             ; 64-bit code segment

; ---------------------------------------------------------------------------
BITS 64

long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rax, KERNEL_VIRT
    jmp rax

times 8192-($-$$) db 0
