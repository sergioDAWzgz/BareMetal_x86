#ifndef BMX_IDT_H
#define BMX_IDT_H

#include <stdint.h>

#define IDT_VECTOR_COUNT 256
#define IDT_SYSCALL_VECTOR 0x80

/* The block isr.asm leaves on the kernel stack, in the order the pushes place it
   in memory: the last register pushed sits at the lowest address. Anything a
   handler writes here is what iretq restores, so a syscall returns its result in
   `rax` simply by writing that field. */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} registers_t;

void idt_init(void);

/* Routes every interrupt the stubs forward. Vector 0x80 is the user gate;
   everything else is a CPU exception or a hardware IRQ. */
void interrupt_dispatch(registers_t *registers);

void exception_dispatch(registers_t *registers);

/* Makes the next CPU exception resume instead of halting, so a fault can be
   demonstrated without losing the machine. */
void exception_expect_fault(void);

#endif
