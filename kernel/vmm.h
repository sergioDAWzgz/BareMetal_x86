#ifndef BMX_VMM_H
#define BMX_VMM_H

#include <stdint.h>

/* User space gets the one PML4 slot the loaders never touch.

   Both boot paths point PML4[0] and PML4[256] at the same page directory as the
   kernel window's PDPT entry, so a user bit on any 2 MiB entry there would open
   the identity map, the direct map and the kernel itself to ring 3. PML4[1] is
   empty on both paths, which makes it the only slot where a whole subtree can
   carry U/S = 1 without exposing anything else. */
#define USER_PML4_INDEX 1ULL
#define USER_BASE 0x0000008000000000ULL
/* Sixteen MiB is far more than a user program needs and keeps the number of page
   tables a bounded quantity instead of allocating one per 4 GiB of address. */
#define USER_LIMIT 0x0000008010000000ULL

/* Reads the page map the loader built out of CR3 and installs an empty, all-user
   subtree at PML4[1]. Returns 0 on success; every user mapping call needs it to
   have succeeded first. */
int vmm_init(void);

/* Maps the 4 KiB pages that cover [address, address + size), zeroes each one and
   copies the first source_size bytes of source at address. Every level of the
   new subtree carries U/S = 1. Returns the number of pages mapped, or -1 when
   the range leaves the user window, a page is already mapped or a frame is
   unavailable. */
int64_t vmm_map_user(uint64_t address, uint64_t size, const void *source,
                     uint64_t source_size);

/* Physical address backing a user virtual address, or 0 when it is not mapped. */
uintptr_t vmm_user_phys(uint64_t address);

/* 4 KiB pages mapped in the user window since vmm_init. */
uint64_t vmm_user_pages(void);

/* Counts user-accessible entries in the three subtrees the loaders built. The
   identity map, the direct map and the window the kernel is linked into share
   their page directory, so a user bit anywhere there exposes kernel memory to
   ring 3. Zero is the only acceptable answer. */
uint64_t vmm_audit_supervisor(void);

const char *vmm_error(void);

#endif
