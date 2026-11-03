#include "bootinfo.h"
#include "console.h"
#include "memory.h"

static boot_info_t cached;
static int cached_state; /* 0 = not read yet, 1 = valid, -1 = missing */

static void bootinfo_load(void)
{
    const volatile boot_info_t *raw =
        (const volatile boot_info_t *)PHYS_TO_VIRT(BOOT_INFO_ADDR);
    volatile uint8_t *dst = (volatile uint8_t *)&cached;
    const volatile uint8_t *src = (const volatile uint8_t *)raw;
    unsigned int i;

    for (i = 0; i < sizeof(cached); i++) {
        dst[i] = src[i];
    }

    if (cached.magic == BOOT_INFO_MAGIC) {
        cached_state = 1;
    } else {
        cached_state = -1;
    }
}

const boot_info_t *bootinfo_get(void)
{
    if (cached_state == 0) {
        bootinfo_load();
    }
    return &cached;
}

int bootinfo_valid(void)
{
    (void)bootinfo_get();
    return cached_state == 1;
}

const char *bootinfo_source_name(void)
{
    const boot_info_t *info = bootinfo_get();

    switch (info->source) {
    case BOOT_INFO_SOURCE_BIOS:
        return "bios";
    case BOOT_INFO_SOURCE_UEFI:
        return "uefi";
    default:
        return "unknown";
    }
}

void bootinfo_print(void)
{
    const boot_info_t *info = bootinfo_get();

    if (!bootinfo_valid()) {
        console_write("boot: no boot info block published\n");
        return;
    }

    console_write("boot: source=");
    console_write(bootinfo_source_name());
    console_write(" framebuffer=");
    console_hex64(info->framebuffer);
    console_write(" mode=");
    console_dec(info->width);
    console_write("x");
    console_dec(info->height);
    console_write("x");
    console_dec(info->bpp);
    console_write(" pitch=");
    console_dec(info->pitch);
    console_write(" masks=");
    console_dec(info->red_size);
    console_write("/");
    console_dec(info->green_size);
    console_write("/");
    console_dec(info->blue_size);
    console_write("\n");
}
