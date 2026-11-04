BITS 64

; ---------------------------------------------------------------------------
; BareMetal_x86 - user program entry
;
; Reached in ring 3 on the stack the kernel placed for the process. There is no
; runtime below this code: `main` is called directly and its return value becomes
; the process status the exit path reports.
; ---------------------------------------------------------------------------

global _start
extern main

section .text

_start:
    xor rbp, rbp                ; outermost frame: nothing above it to unwind
    call main
.hang:
    jmp .hang                   ; the exit syscall replaces this wait
