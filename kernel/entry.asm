BITS 32
section .text.entry

; ---------------------------------------------------------------------------
; BareMetal_x86 - kernel entry
;
; Reached from stage 2 with flat 32-bit segments. Establishes a stack, zeroes
; .bss and hands control to kmain().
; ---------------------------------------------------------------------------

global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    mov esp, 0x90000
    mov ebp, esp

    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    call kmain

.hang:
    cli
    hlt
    jmp .hang
