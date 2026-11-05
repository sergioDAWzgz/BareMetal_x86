; Transfers the machine to the Linux 64-bit boot protocol entry point.
;
; Documentation/arch/x86/boot.rst states the contract this file exists to keep:
; at entry the CPU is in 64-bit mode with paging on, interrupts are masked, a GDT
; is loaded whose 0x10 selector is a flat 64-bit code segment and whose 0x18
; selector is a flat data segment, and %rsi holds the physical address of the
; struct boot_params. Linux reloads CS from its own GDT within a few instructions
; and zeroes every data segment, but it starts on the descriptor the loader left
; behind, so that descriptor has to be a valid long-mode one.
BITS 64

section .text
global linux_jump

; void linux_jump(uint64_t entry, uint64_t boot_params_phys);
;   rdi = physical entry point, rsi = physical address of struct boot_params
linux_jump:
    cli                             ; from here on nothing of ours may run again
    lgdt [linux_gdt_pointer]        ; the kernel's own GDT, with 0x10 and 0x18 flat

    ; A far return is the only way to load CS in long mode: an immediate far jump
    ; is not encodable here, and the selector has to come from the new GDT or the
    ; CPU keeps indexing the old one.
    push 0x10                       ; __BOOT_CS: 64-bit code, ring 0, 4 GiB flat
    lea rax, [rel .reloaded]
    push rax
    retfq

.reloaded:
    mov ax, 0x18                    ; __BOOT_DS: data, ring 0, 4 GiB flat
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax

    ; %rsi already carries boot_params: it is the second argument, and nothing
    ; above touched it. %rdi carries the entry point.
    jmp rdi                         ; never returns

section .rodata
align 16
linux_gdt:
    dq 0x0000000000000000           ; null descriptor
    dq 0x0000000000000000           ; unused, the slot the CPU's first GDT has
    dq 0x00AF9A000000FFFF           ; 0x10: present, ring 0, execute/read, L = 1
    dq 0x00CF92000000FFFF           ; 0x18: present, ring 0, read/write, 4 GiB flat
linux_gdt_end:

linux_gdt_pointer:
    dw linux_gdt_end - linux_gdt - 1
    dq linux_gdt
