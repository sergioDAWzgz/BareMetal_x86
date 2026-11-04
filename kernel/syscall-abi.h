#ifndef BMX_SYSCALL_ABI_H
#define BMX_SYSCALL_ABI_H

/* The call numbers the kernel and the user library both have to agree on. This
   header is the only shared definition: the kernel includes it to decode the
   register block, the user library includes it to raise the gate, and neither
   hardcodes a number the other one owns. */
#define SYSCALL_WRITE 1     /* rdi = buffer, rsi = size, returns bytes written */
#define SYSCALL_EXIT 2      /* rdi = status, never returns */

#endif
