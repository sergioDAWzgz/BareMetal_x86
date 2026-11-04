#ifndef BMX_USERMODE_H
#define BMX_USERMODE_H

#include <stdint.h>

/* Drops to ring 3 at `entry` with `stack` as the initial stack pointer. Returns
   only when the program exits through the exit syscall, because that call is what
   resumes the kernel caller. The selectors are passed in rather than repeated in
   assembly, so the GDT layout has exactly one owner. */
void usermode_run(uint64_t entry, uint64_t stack, uint64_t code, uint64_t data);

/* Called from the syscall handler: restores the kernel stack saved by
   usermode_run and returns to whoever called it. */
void usermode_resume(void);

#endif
