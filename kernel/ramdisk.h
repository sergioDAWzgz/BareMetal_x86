#ifndef BMX_RAMDISK_H
#define BMX_RAMDISK_H

#include <stdint.h>

typedef struct {
    uintptr_t phys;         /* first physical frame of the staged copy */
    void *data;             /* kernel virtual address of the staged copy */
    uint32_t size;          /* bytes staged */
} ramdisk_file_t;

/* Copies a file from the mounted FAT32 volume into a contiguous run of frames.
   Contiguity is what lets the loader treat the image as one block through the
   direct map instead of chasing a scatter list. Returns 0 on success. */
int ramdisk_stage(const char *name, ramdisk_file_t *out);

void ramdisk_print(const char *name, const ramdisk_file_t *file);

const char *ramdisk_error(void);

#endif
