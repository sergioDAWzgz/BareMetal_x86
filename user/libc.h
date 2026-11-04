#ifndef BMX_USER_LIBC_H
#define BMX_USER_LIBC_H

#include <stdint.h>

/* The user side of the syscall interface. Everything here is implemented over
   int 0x80: the program never touches a port, a page table or an interrupt
   controller, and it has no way to reach the kernel except this gate. */

void write(const char *text);
void puts(const char *text);

/* Ends the process. The kernel resumes its own caller; the program's frames are
   abandoned, which is why this never returns. */
void exit(int status);

uint64_t strlen(const char *text);
void *memset(void *destination, int value, uint64_t count);
void *memcpy(void *destination, const void *source, uint64_t count);

#endif
