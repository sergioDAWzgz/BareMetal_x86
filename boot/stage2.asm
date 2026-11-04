BITS 16
ORG 0x7E00

; ---------------------------------------------------------------------------
; BareMetal_x86 - stage 2 (BIOS path)
;
; Runs in real mode, loaded by stage 1 at 0x7E00. It
;   1. initialises COM1 and reports progress,
;   2. collects the BIOS E820 memory map for the kernel,
;   3. sets a VBE linear framebuffer mode and publishes the boot info block,
;   4. loads the kernel image to physical 0x10000,
;   5. enables A20, installs a GDT and enters 32-bit protected mode,
;   6. builds 4-level page tables with an identity map, a direct map at
;      0xFFFF800000000000 and the kernel at 0xFFFFFFFF80000000,
;   7. enables long mode and jumps to the kernel virtual entry.
; ---------------------------------------------------------------------------

STAGE2_SECTORS  equ 16
KERNEL_LBA      equ 1 + STAGE2_SECTORS
KERNEL_VIRT     equ 0xFFFFFFFF80010000
KERNEL_SEG      equ 0x1000              ; physical 0x10000
KERNEL_CHUNK    equ 64                  ; sectors per read (32 KiB)
KERNEL_SEG_STEP equ KERNEL_CHUNK * 32   ; 0x0800 paragraphs

BOOT_DRIVE_ADDR equ 0x0500
E820_COUNT_ADDR equ 0x0504
E820_BUF_ADDR   equ 0x0508
E820_MAX        equ 64
E820_ENTRY_SIZE equ 24

BOOT_INFO_ADDR  equ 0x0C00
BOOT_INFO_MAGIC equ 0x424D5833          ; 'BMX3'
BOOT_SOURCE_BIOS equ 1

VBE_INFO_ADDR   equ 0x3000              ; ModeInfoBlock scratch
VBE_CTRL_ADDR   equ 0x2000              ; VbeInfoBlock scratch
VBE_MODES_ADDR  equ 0x2400              ; flattened mode list
VBE_MAX_MODES   equ 512
VBE_LINEAR      equ 0x4000              ; request the linear framebuffer
VBE_WANT_W      equ 1024
VBE_WANT_H      equ 768
VBE_WANT_BPP    equ 32

COM1            equ 0x3F8

PT_BASE         equ 0x70000             ; PML4
PDPT_DIRECT     equ 0x71000             ; direct map / identity PDPT
PDPT_KERNEL     equ 0x72000             ; kernel higher half PDPT
PD_BASE         equ 0x73000             ; four page directories, 4 GiB

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; The stack grows down from the load address, so it sits below this code and
    ; does not overwrite the copy the BIOS gave us at 0x7C00.
    mov sp, 0x7C00
    sti

    call serial_init

    mov si, msg_banner
    call puts

    call collect_memory_map
    mov si, msg_memory
    call puts

    call check_long_mode
    call setup_video_mode

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
    ; CR0.PE: the next far jump needs protected mode already enabled, and the GDT
    ; loaded above is the only descriptor table in effect from here on.
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
; setup_video_mode: publishes the boot info block and, when VBE is available,
; switches to a linear framebuffer mode.
; ---------------------------------------------------------------------------
setup_video_mode:
    mov dword [BOOT_INFO_ADDR + 0x00], BOOT_INFO_MAGIC
    mov dword [BOOT_INFO_ADDR + 0x04], BOOT_SOURCE_BIOS
    mov dword [BOOT_INFO_ADDR + 0x08], 0
    mov dword [BOOT_INFO_ADDR + 0x0C], 0
    mov dword [BOOT_INFO_ADDR + 0x10], 0
    mov dword [BOOT_INFO_ADDR + 0x14], 0
    mov dword [BOOT_INFO_ADDR + 0x18], 0
    mov dword [BOOT_INFO_ADDR + 0x1C], 0
    mov dword [BOOT_INFO_ADDR + 0x20], 0
    mov dword [BOOT_INFO_ADDR + 0x24], 0
    mov dword [BOOT_INFO_ADDR + 0x28], 0
    mov dword [BOOT_INFO_ADDR + 0x2C], 0

    ; --- ask the controller for its mode list ---
    ; 'VBE2' requests the 2.0 information block; with the 1.x 'VESA' signature
    ; the dword at offset 0x28 is not defined as PhysBasePtr.
    mov dword [VBE_CTRL_ADDR], 0x32454256   ; 'VBE2'
    mov ax, 0x4F00
    mov di, VBE_CTRL_ADDR
    int 0x10
    cmp ax, 0x004F
    jne .no_vbe

    ; PhysBasePtr is only defined by the 2.0 layout, so refuse an older block.
    cmp word [VBE_CTRL_ADDR + 0x04], 0x0200
    jb  .no_vbe

    ; Flatten the far mode list into low memory so a BIOS that clobbers the
    ; segment registers cannot break the scan.
    mov ax, [VBE_CTRL_ADDR + 0x10]
    mov fs, ax
    mov si, [VBE_CTRL_ADDR + 0x0E]
    mov di, VBE_MODES_ADDR
    mov cx, VBE_MAX_MODES - 1
.copy_modes:
    mov ax, [fs:si]
    mov [di], ax
    cmp ax, 0xFFFF
    je  .modes_copied
    add si, 2
    add di, 2
    loop .copy_modes
.modes_copied:
    xor ax, ax
    mov fs, ax

    ; --- pick the first 1024x768x32 mode in the list ---
    mov word [vbe_index], 0
.scan:
    mov bx, [vbe_index]
    cmp bx, VBE_MAX_MODES - 1       ; a list without its terminator must not run on
    jae .no_vbe
    add bx, bx                      ; the flattened list holds words, so index * 2
    mov cx, [VBE_MODES_ADDR + bx]
    cmp cx, 0xFFFF
    je  .no_vbe

    mov ax, 0x4F01
    mov di, VBE_INFO_ADDR
    int 0x10
    cmp ax, 0x004F
    jne .next_mode
    cmp word [VBE_INFO_ADDR + 0x12], VBE_WANT_W
    jne .next_mode
    cmp word [VBE_INFO_ADDR + 0x14], VBE_WANT_H
    jne .next_mode
    cmp byte [VBE_INFO_ADDR + 0x19], VBE_WANT_BPP
    jne .next_mode
    jmp .mode_found

.next_mode:
    inc word [vbe_index]
    jmp .scan

.mode_found:
    mov [vbe_chosen], cx
    mov ax, 0x4F02                  ; set mode with the linear framebuffer
    mov bx, cx
    or  bx, VBE_LINEAR
    int 0x10
    cmp ax, 0x004F
    jne .no_vbe

    mov ax, 0x4F01                  ; re-read the mode info after the switch
    mov cx, [vbe_chosen]
    mov di, VBE_INFO_ADDR
    int 0x10
    cmp ax, 0x004F
    jne .no_vbe

    movzx eax, word [VBE_INFO_ADDR + 0x10]  ; bytes per scanline
    mov [BOOT_INFO_ADDR + 0x18], eax
    movzx eax, word [VBE_INFO_ADDR + 0x12]  ; x resolution
    mov [BOOT_INFO_ADDR + 0x10], eax
    movzx eax, word [VBE_INFO_ADDR + 0x14]  ; y resolution
    mov [BOOT_INFO_ADDR + 0x14], eax
    movzx eax, byte [VBE_INFO_ADDR + 0x19]  ; bits per pixel
    mov [BOOT_INFO_ADDR + 0x1C], eax

    mov eax, [VBE_INFO_ADDR + 0x28]         ; physical framebuffer base
    mov [BOOT_INFO_ADDR + 0x08], eax
    mov dword [BOOT_INFO_ADDR + 0x0C], 0

    mov al, [VBE_INFO_ADDR + 0x1F]          ; red mask size
    mov [BOOT_INFO_ADDR + 0x20], al
    mov al, [VBE_INFO_ADDR + 0x20]          ; red field position
    mov [BOOT_INFO_ADDR + 0x21], al
    mov al, [VBE_INFO_ADDR + 0x21]          ; green mask size
    mov [BOOT_INFO_ADDR + 0x22], al
    mov al, [VBE_INFO_ADDR + 0x22]          ; green field position
    mov [BOOT_INFO_ADDR + 0x23], al
    mov al, [VBE_INFO_ADDR + 0x23]          ; blue mask size
    mov [BOOT_INFO_ADDR + 0x24], al
    mov al, [VBE_INFO_ADDR + 0x24]          ; blue field position
    mov [BOOT_INFO_ADDR + 0x25], al

    mov si, msg_video
    call puts
    ret

.no_vbe:
    mov si, msg_no_video
    call puts
    ret

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
msg_video: db "stage2: VBE linear framebuffer active", 13, 10, 0
msg_no_video: db "stage2: VBE unavailable, continuing without framebuffer", 13, 10, 0
msg_loaded: db "stage2: kernel loaded at 0x10000", 13, 10, 0
msg_failed: db "stage2: kernel read failed", 13, 10, 0
msg_no_long_mode: db "stage2: this CPU does not support long mode, halting", 13, 10, 0

align 2
vbe_index:  dw 0
vbe_chosen: dw 0

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
    ; A fresh stack in low memory: the real-mode stack at 0x7C00 is below the
    ; 1 MiB region the kernel and its page tables are about to use.
    mov esp, 0x90000
    mov ebp, esp

    ; --- zero the page tables: PML4, two PDPTs and four PDs ---
    mov edi, PT_BASE
    mov ecx, (7 * 4096) / 4
    xor eax, eax
    cld
    rep stosd

    ; PML4: identity, direct map and kernel
    mov eax, PDPT_DIRECT + 0x3
    mov [PT_BASE], eax                      ; PML4[0]   -> identity
    mov [PT_BASE + 256*8], eax              ; PML4[256] -> direct map 0xFFFF800000000000
    mov eax, PDPT_KERNEL + 0x3
    mov [PT_BASE + 511*8], eax              ; PML4[511] -> kernel 0xFFFFFFFF80000000

    ; PDPT_DIRECT[0..3] -> PD0..PD3
    mov eax, PD_BASE + 0x3
    xor ecx, ecx
.fill_pdpt:
    mov [PDPT_DIRECT + ecx*8], eax
    add eax, 4096
    inc ecx
    cmp ecx, 4
    jb  .fill_pdpt

    ; PDPT_KERNEL[510] -> PD0, so 0xFFFFFFFF80000000 maps physical 0
    mov eax, PD_BASE + 0x3
    mov [PDPT_KERNEL + 510*8], eax

    ; PD0..PD3: 2048 x 2 MiB pages covering the first 4 GiB
    mov edi, PD_BASE
    mov eax, 0x83                           ; present | writable | page size
    mov ecx, 2048
.fill_pd:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    ; --- long mode bring-up ---
    ; CR4.PAE must be set before EFER.LME, because the CPU refuses to enter long
    ; mode with the physical address extension disabled.
    mov eax, cr4
    or  eax, 0x20                           ; CR4.PAE
    mov cr4, eax

    mov eax, PT_BASE
    ; CR3 points at the PML4 just built, so the identity and direct maps are live
    ; as soon as CR0.PG is set below.
    mov cr3, eax

    mov ecx, 0xC0000080                     ; IA32_EFER
    rdmsr
    ; EFER.LME only arms long mode: the mode itself is entered by the far jump
    ; below, once paging is on, which is the order the architecture requires.
    or  eax, 0x100                          ; EFER.LME
    wrmsr

    mov eax, cr0
    ; CR0.PG last of the three: with PAE and LME already set this turns paging on
    ; in long mode, and the next instruction fetch happens through the tables.
    or  eax, 0x80000000                     ; CR0.PG
    mov cr0, eax

    jmp 0x08:long_entry                     ; 64-bit code segment

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
