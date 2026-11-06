BITS 64

; ---------------------------------------------------------------------------
; BareMetal_x86 - the init this project's initramfs runs
;
; Reached by the Linux kernel as PID 1 through rdinit=/init. There is no runtime
; below this code and no libc: the only interface is Linux's own system call ABI,
; which is not the one user/crt0.asm uses, so this file stands alone on purpose.
;
; Its job is to prove the handoff end to end: write a marker where the gate can
; read it, then power the machine off instead of leaving the payload spinning.
; ---------------------------------------------------------------------------

global _start

section .rodata

marker:     db "bmx: initramfs marker", 10
marker_len  equ $ - marker

section .text

_start:
    ; write(1, marker, marker_len). Linux opens /dev/console as file descriptors
    ; 0, 1 and 2 for init before running it, and the kernel command line points
    ; the console at COM1, so this line lands in the serial log.
    mov eax, 1                  ; __NR_write
    mov edi, 1                  ; stdout
    lea rsi, [rel marker]       ; the image is not position independent
    mov edx, marker_len
    syscall

    ; reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, 0)
    ; A clean power off ends the QEMU run by itself, so the gate reads a log that
    ; is complete instead of one cut short by a timeout.
    mov eax, 169                ; __NR_reboot
    mov edi, 0xfee1dead         ; LINUX_REBOOT_MAGIC1
    mov esi, 672274793          ; LINUX_REBOOT_MAGIC2
    mov edx, 0x4321fedc         ; LINUX_REBOOT_CMD_POWER_OFF
    xor r10d, r10d              ; no argument
    syscall

    ; Power off does not return on a machine that can do it. On one that cannot,
    ; idling is better than faulting: a signal here would make PID 1 panic and
    ; bury the marker under a call trace.
.hang:
    pause
    jmp .hang
