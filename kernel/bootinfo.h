#ifndef BMX_BOOTINFO_H
#define BMX_BOOTINFO_H

#include <stdint.h>

/* Physical block published by both loaders before they jump to the kernel. */
#define BOOT_INFO_ADDR 0x0C00
#define BOOT_INFO_MAGIC 0x424D5833u /* 'BMX3' */

#define BOOT_INFO_SOURCE_BIOS 1
#define BOOT_INFO_SOURCE_UEFI 2

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t source;
    uint64_t framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint32_t bpp;
    uint8_t red_size;
    uint8_t red_pos;
    uint8_t green_size;
    uint8_t green_pos;
    uint8_t blue_size;
    uint8_t blue_pos;
    uint16_t reserved;
    uint64_t rsdp;       /* physical RSDP address, or 0 when the loader has none */
} boot_info_t;

const boot_info_t *bootinfo_get(void);
int bootinfo_valid(void);
const char *bootinfo_source_name(void);
void bootinfo_print(void);

#endif
