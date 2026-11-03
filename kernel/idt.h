#ifndef BMX_IDT_H
#define BMX_IDT_H

#include <stdint.h>

#define IDT_VECTOR_COUNT 256

typedef struct {
    uint64_t vector;
    uint64_t error;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

void idt_init(void);
void exception_dispatch(interrupt_frame_t *frame);

/* Makes the next CPU exception resume instead of halting, so a fault can be
   demonstrated without losing the machine. */
void exception_expect_fault(void);

#endif
