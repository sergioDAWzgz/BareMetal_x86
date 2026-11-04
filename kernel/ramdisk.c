#include "ramdisk.h"

#include "console.h"
#include "fat32.h"
#include "memory.h"

#define STAGE_PAGE 4096ULL

static const char *failure = "none";

int ramdisk_stage(const char *name, ramdisk_file_t *out)
{
    fat32_file_t file;
    uint64_t frames;
    uintptr_t phys;
    int32_t bytes;

    failure = "none";
    out->phys = 0;
    out->data = 0;
    out->size = 0;

    if (fat32_open(name, &file) != 0) {
        failure = fat32_error();
        return -1;
    }
    if (file.size == 0) {
        failure = "the file is empty";
        return -1;
    }

    frames = ((uint64_t)file.size + STAGE_PAGE - 1) / STAGE_PAGE;
    phys = pmm_alloc_frames(frames);
    if (phys == 0) {
        failure = "no contiguous run for the staging area";
        return -1;
    }

    /* The driver is told the whole run so a file that grows past the frame count
       it was measured with fails here instead of overflowing into the next
       frame. */
    bytes = fat32_read(&file, PHYS_TO_VIRT(phys), (uint32_t)(frames * STAGE_PAGE));
    if (bytes < 0) {
        failure = fat32_error();
        return -1;
    }

    out->phys = phys;
    out->data = PHYS_TO_VIRT(phys);
    out->size = (uint32_t)bytes;
    return 0;
}

void ramdisk_print(const char *name, const ramdisk_file_t *file)
{
    console_write("ramdisk: staged ");
    console_write(name);
    console_write(" ");
    console_dec(file->size);
    console_write(" bytes at ");
    console_hex64((uint64_t)file->phys);
    console_write("\n");
}

const char *ramdisk_error(void)
{
    return failure;
}
