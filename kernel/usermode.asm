BITS 64

; ---------------------------------------------------------------------------
; BareMetal_x86 - ring 3 entry and return
;
; usermode_run builds the frame iretq consumes to drop to ring 3. The kernel
; stack pointer at the call site is saved because the return path cannot be
; rebuilt from anything the CPU keeps: the ring 3 frames are abandoned on
; purpose, so the exit syscall has to jump back to the saved point.
; ---------------------------------------------------------------------------

section .text

global usermode_run
global usermode_resume

; rdi = entry point, rsi = user stack top,
; rdx = user code selector, rcx = user data selector
usermode_run:
    ; The interrupt gate clears IF on the way in, and this path returns with ret
    ; instead of iretq, so the flags at the call site are what brings them back:
    ; without this the kernel would resume with interrupts disabled.
    pushfq
    pop qword [rel kernel_flags]

    mov [rel kernel_rsp], rsp       ; the return address of the C call sits here

    ; The frame iretq pops, pushed in reverse order: ss is consumed last.
    push rcx                        ; ss
    push rsi                        ; rsp
    push qword 0x202                ; rflags: reserved bit 1 plus IF
    push rdx                        ; cs
    push rdi                        ; rip

    ; The data segment registers still hold kernel selectors. Ring 3 would fault
    ; on the first one it loaded, and leaving them stale would also make the
    ; interrupt entry push a block the kernel does not expect.
    mov rax, rcx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    iretq

usermode_resume:
    ; The kernel data selector from gdt.h. An interrupt from ring 3 arrives with
    ; ss already nulled by the CPU and ds/es/fs/gs still holding the user
    ; selector, so all of them are reloaded before any C code runs again.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [rel kernel_rsp]
    ; Interrupts come back only here, exactly as they were at the call site.
    push qword [rel kernel_flags]
    popfq
    ret

section .bss
align 8
kernel_rsp:
    resq 1
kernel_flags:
    resq 1
