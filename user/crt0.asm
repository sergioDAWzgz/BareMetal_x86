BITS 64

; ---------------------------------------------------------------------------
; BareMetal_x86 - user program entry
;
; Reached in ring 3 on the stack the kernel placed for the process. There is no
; runtime below this code: `main` is called directly, its return value becomes
; the process status, and `exit` is what hands control back to the kernel.
; ---------------------------------------------------------------------------

global _start
extern main
extern exit

section .text

_start:
    xor rbp, rbp                ; outermost frame: nothing above it to unwind
    call main
    mov edi, eax                ; the exit status is the value main returned
    call exit
.hang:
    jmp .hang                   ; exit never returns
