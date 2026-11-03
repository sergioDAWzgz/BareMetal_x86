#include "console.h"
#include "cpu.h"

#define BOOT_DRIVE_ADDR 0x0500
#define KERNEL_LOAD_ADDR 0x10000

static void put_hex_byte(unsigned char value)
{
    static const char digits[] = "0123456789abcdef";

    console_putc(digits[(value >> 4) & 0x0F]);
    console_putc(digits[value & 0x0F]);
}

void kmain(void)
{
    unsigned char boot_drive = *(volatile unsigned char *)BOOT_DRIVE_ADDR;
    char brand[49];

    console_init();

    console_write("\n");
    console_write("BareMetal_x86 - phase 1\n");
    console_write("=======================\n");
    console_write("mode:      32-bit protected mode, flat GDT from stage 2\n");
    console_write("kernel:    loaded at 0x10000 (stage 2)\n");
    console_write("console:   COM1 115200 8N1 + VGA text 80x25\n");

    if (cpu_brand(brand) == 0) {
        console_write("cpu:       ");
        console_write(brand);
        console_write("\n");
    } else {
        console_write("cpu:       brand string unavailable\n");
    }

    console_write("boot disk: 0x");
    put_hex_byte(boot_drive);
    console_write("\n");
    console_write("\nPhase 1 reached the kernel. Halting.\n");
}
