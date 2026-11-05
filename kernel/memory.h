#ifndef BMX_MEMORY_H
#define BMX_MEMORY_H

#include <stdint.h>

/* Both loaders build a direct map of the first 4 GiB at this base, so physical
   address X is also reachable as a kernel virtual address. The kernel itself is
   linked in a separate 2 MiB window at KERNEL_BASE. */
#define PHYS_BASE 0xFFFF800000000000ULL
#define PHYS_TO_VIRT(address) ((void *)(uintptr_t)((uint64_t)(address) + PHYS_BASE))
/* The inverse of PHYS_TO_VIRT, for the rare case where a kernel pointer into the
   direct map has to be reported as the physical address firmware gave us. */
#define VIRT_TO_PHYS(address) ((uint64_t)(uintptr_t)(address) - PHYS_BASE)

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_extended;
} e820_entry_t;

uint32_t memmap_count(void);
const e820_entry_t *memmap_entries(void);
void memmap_print(void);

void pmm_init(void);
uintptr_t pmm_alloc_frame(void);

/* Allocates count physically contiguous frames and returns the physical address
   of the first one, or 0 when no run that long is free. DMA needs contiguity:
   a PRDT and the direct map both describe memory by physical address, so frames
   handed out one at a time cannot be spliced back together. */
uintptr_t pmm_alloc_frames(uint64_t count);

void pmm_free_frame(uintptr_t frame);
uint64_t pmm_free_frames(void);

/* The highest physical address the frame allocator manages, exclusive. A caller
   searching for a window to reserve uses it to tell "this range is taken" from
   "this range is past the end of the map": only the first one is worth retrying
   at a higher address. */
uint64_t pmm_limit(void);

/* Takes every frame that overlaps [base, base + size) out of the free set before
   anything is copied there. Returns 0 when the whole range is now reserved, -1
   when a frame inside it had already been handed out: overwriting memory some
   other part of the kernel owns is worse than refusing to start the payload, so
   the caller gets the chance to stop instead. A range that leaves the managed
   window is refused for the same reason. */
int pmm_reserve_range(uint64_t base, uint64_t size);

#endif
