#ifndef BMX_GDT_H
#define BMX_GDT_H

#include <stdint.h>

/* Slot order is a published contract with isr.asm and usermode.asm, which hold
   no access to this header: a selector is an index into this table, so moving an
   entry changes the number that has to be pushed. */
#define GDT_SELECTOR_CODE 0x08
#define GDT_SELECTOR_DATA 0x10
#define GDT_SELECTOR_USER_CODE 0x18
#define GDT_SELECTOR_USER_DATA 0x20
#define GDT_SELECTOR_TSS 0x28

/* The selectors iretq is given: RPL 3 is what makes the CPU treat the frame as a
   return to ring 3 instead of a same-privilege jump. */
#define GDT_SELECTOR_USER_CODE_RING3 (GDT_SELECTOR_USER_CODE | 3)
#define GDT_SELECTOR_USER_DATA_RING3 (GDT_SELECTOR_USER_DATA | 3)

void gdt_init(void);

/* Sets the stack the CPU switches to when an interrupt arrives from ring 3. The
   hardware reads it from the TSS on every such transition, so it has to be a
   region the kernel owns and it has to be updated whenever that region changes. */
void gdt_set_kernel_stack(uint64_t top);

#endif
