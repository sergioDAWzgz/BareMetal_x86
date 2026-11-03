BITS 64
section .text.entry

; ---------------------------------------------------------------------------
; BareMetal_x86 - kernel entry (long mode)
;
; Reached from stage 2 with the higher half mapped and 64-bit flat segments.
; Sets the kernel stack, zeroes .bss and calls kmain().
; ---------------------------------------------------------------------------

global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    mov rsp, stack_top
    xor rbp, rbp

    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor al, al
    cld
    rep stosb

    call kmain

.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resq 2048                   ; 16 KiB kernel stack
stack_top:
