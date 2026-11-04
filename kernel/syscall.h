#ifndef BMX_SYSCALL_H
#define BMX_SYSCALL_H

#include "idt.h"

/* Handles the int 0x80 gate. The register block is the live interrupt frame, so
   writing `rax` here is what the program receives as the return value, and the
   exit call ends the process instead of returning to it. */
void syscall_dispatch(registers_t *registers);

#endif
