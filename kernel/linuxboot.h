#ifndef BMX_LINUXBOOT_H
#define BMX_LINUXBOOT_H

#include <stdint.h>

/* Loads LINUX.BIN from the mounted FAT32 volume into the memory window its own
   setup header asks for, builds the zero page Linux expects and transfers the
   machine to it.

   A successful handoff never returns: the Linux entry point takes over the CPU,
   the page tables and the interrupt flag, and the kernel image this function
   lives in is no longer addressable. The return value only describes the failure
   paths, which print and then leave the caller free to decide what to do. */
int linux_payload_boot(void);

/* Reason for the most recent failure. */
const char *linux_payload_error(void);

#endif
