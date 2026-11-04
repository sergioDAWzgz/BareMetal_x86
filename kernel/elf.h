#ifndef BMX_ELF_H
#define BMX_ELF_H

#include <stdint.h>

typedef struct {
    uint64_t entry;         /* link address of the entry point */
    uint32_t segments;      /* PT_LOAD segments that were loaded */
    uint64_t pages;         /* 4 KiB pages those segments occupy */
} elf_image_t;

/* Loads an ELF64 executable built for x86-64 from an in-memory image. Every
   PT_LOAD segment is copied into freshly allocated user pages at its link
   address, so the caller must have run vmm_init first. The whole image, every
   segment and the entry point must lie inside the user window. */
int elf_load(const void *image, uint32_t size, elf_image_t *out);

const char *elf_error(void);

#endif
