#ifndef BMX_MEMORY_H
#define BMX_MEMORY_H

#include <stdint.h>

/* Both loaders build a direct map of the first 4 GiB at this base, so physical
   address X is also reachable as a kernel virtual address. The kernel itself is
   linked in a separate 2 MiB window at KERNEL_BASE. */
#define PHYS_BASE 0xFFFF800000000000ULL
#define PHYS_TO_VIRT(address) ((void *)(uintptr_t)((uint64_t)(address) + PHYS_BASE))

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
void pmm_free_frame(uintptr_t frame);
uint64_t pmm_free_frames(void);

#endif
