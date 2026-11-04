#include "acpi.h"
#include "ahci.h"
#include "bootinfo.h"
#include "console.h"
#include "cpu.h"
#include "fat32.h"
#include "fb.h"
#include "fbtext.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "memory.h"
#include "pci.h"
#include "pic.h"

#define TIMER_HZ 100
#define TICKS_TO_OBSERVE 10
#define BARS_VISIBLE_TICKS TIMER_HZ
#define KBD_LINE_MAX 96

extern void divide_error_probe(void);

static void wait_ticks(uint64_t count)
{
    uint64_t start = timer_ticks_get();

    while (timer_ticks_get() - start < count) {
        __asm__ volatile("hlt");
    }
}

static void fault_demo(void)
{
    console_write("exc: triggering breakpoint\n");
    exception_expect_fault();
    __asm__ volatile("int3");
    console_write("exc: resumed after breakpoint\n");

    console_write("exc: triggering divide error\n");
    exception_expect_fault();
    divide_error_probe();
    console_write("exc: resumed after divide error\n");
}

static void pmm_demo(void)
{
    uintptr_t first = pmm_alloc_frame();
    uintptr_t second = pmm_alloc_frame();
    uintptr_t third = pmm_alloc_frame();

    console_write("pmm: allocated ");
    console_hex64(first);
    console_write(" ");
    console_hex64(second);
    console_write(" ");
    console_hex64(third);
    console_write("\n");

    pmm_free_frame(second);
    pmm_free_frame(third);

    console_write("pmm: free frames ");
    console_dec(pmm_free_frames());
    console_write("\n");
}

static void video_demo(void)
{
    if (fb_init() != 0) {
        console_write("fb: no linear framebuffer available\n");
        return;
    }

    console_write("fb: ");
    console_dec(fb_width());
    console_write("x");
    console_dec(fb_height());
    console_write(" bpp=32 pitch=");
    console_dec(fb_pitch());
    console_write(" phys=");
    console_hex64(fb_physical());
    console_write("\n");

    if (fb_self_test() != 0) {
        console_write("fb: pixel readback failed\n");
        return;
    }

    console_write("fb: pixel readback self-test passed\n");

    fb_draw_bars();
    console_write("fb: colour bars drawn\n");
    wait_ticks(BARS_VISIBLE_TICKS);

    if (fbtext_init() != 0) {
        console_write("fb: text console did not start\n");
        return;
    }

    console_write("fb: text console ");
    console_dec(fbtext_cols());
    console_write("x");
    console_dec(fbtext_rows());
    console_write(" cells\n");

    fbtext_write("BareMetal_x86\n");
    fbtext_write("phase 3b - framebuffer console\n\n");
}

static void keyboard_start(void)
{
    if (keyboard_self_test() != 0) {
        console_write("kbd: scancode translation self-test failed\n");
    } else {
        console_write("kbd: scancode translation self-test passed\n");
    }

    if (keyboard_init() != 0) {
        console_write("kbd: the 8042 controller rejected the init sequence\n");
        return;
    }

    console_write("kbd: controller ready, IRQ1 unmasked\n");
}

static void keyboard_echo_loop(void)
{
    char line[KBD_LINE_MAX];
    unsigned int length = 0;

    console_write("kbd: echo loop, type and press enter, esc to leave\n");

    for (;;) {
        int key = keyboard_getchar();

        if (key < 0) {
            __asm__ volatile("hlt");
            continue;
        }

        if (key == KEY_ESCAPE) {
            console_write("\nkbd: escape received, leaving the echo loop\n");
            return;
        }

        if (key == KEY_ENTER) {
            line[length] = '\0';
            console_write("\nkbd: typed \"");
            console_write(line);
            console_write("\"\n");
            length = 0;
            continue;
        }

        if (key == KEY_BACKSPACE) {
            if (length > 0) {
                length--;
                console_write("\b \b");
            }
            continue;
        }

        if (length + 1 < sizeof(line)) {
            line[length] = (char)key;
            length++;
            console_putc((char)key);
        }
    }
}

/* Phase 4: the storage path from the SATA port to a user process. Each step
   fails loudly and stops the chain, because a later step would only read a
   buffer the earlier one never filled. */
static void phase4(void)
{
    uint8_t sector[AHCI_SECTOR_SIZE];
    uint8_t content[AHCI_SECTOR_SIZE];
    fat32_file_t file;
    int32_t size;

    if (ahci_init() != 0) {
        console_write("ahci: ");
        console_write(ahci_error());
        console_write("\n");
        return;
    }
    ahci_print();

    if (ahci_read(0, 0, 1, sector) != 0) {
        console_write("ahci: ");
        console_write(ahci_error());
        console_write("\n");
        return;
    }
    console_write("ahci: read 512 bytes from port ");
    console_dec(ahci_port_number(0));
    console_write(" lba 0\n");

    if (fat32_mount() != 0) {
        console_write("fat32: ");
        console_write(fat32_error());
        console_write("\n");
        return;
    }
    fat32_print();

    if (fat32_open("HELLO.TXT", &file) != 0) {
        console_write("fat32: ");
        console_write(fat32_error());
        console_write("\n");
        return;
    }
    if (file.size >= sizeof(content)) {
        console_write("fat32: HELLO.TXT does not fit the staging buffer\n");
        return;
    }

    size = fat32_read(&file, content, sizeof(content));
    if (size < 0) {
        console_write("fat32: ");
        console_write(fat32_error());
        console_write("\n");
        return;
    }
    content[size] = '\0';

    console_write("fat32: HELLO.TXT ");
    console_dec((uint64_t)size);
    console_write(" bytes\n");
    console_write("fat32: content ");
    console_write((const char *)content);
}

void kmain(void)
{
    char brand[49];

    console_init();

    console_write("\n");
    console_write("BareMetal_x86 - phase 3\n");
    console_write("=======================\n");
    console_write("mode:      64-bit long mode (IA-32e)\n");
    console_write("paging:    4-level, identity + direct map + kernel window\n");

    if (cpu_brand(brand) == 0) {
        console_write("cpu:       ");
        console_write(brand);
        console_write("\n");
    } else {
        console_write("cpu:       brand string unavailable\n");
    }

    gdt_init();
    console_write("gdt:       kernel GDT and TSS installed\n");

    idt_init();
    console_write("idt:       48 vectors installed\n");

    pic_init();
    pit_init(TIMER_HZ);
    console_write("pic:       remapped to 0x20/0x28, pit ");
    console_dec(TIMER_HZ);
    console_write(" Hz\n");

    interrupts_enable();

    memmap_print();

    pmm_init();
    pmm_demo();

    bootinfo_print();
    video_demo();

    if (acpi_init() != 0) {
        console_write("acpi: no rsdp found\n");
    } else {
        acpi_print();
    }

    pci_enumerate();
    pci_print();

    wait_ticks(TICKS_TO_OBSERVE);
    console_write("pit:       ");
    console_dec(timer_ticks_get());
    console_write(" ticks observed\n");

    fault_demo();

    keyboard_start();

    console_write("\nPhase 3 complete.\n");

    phase4();
    console_write("Phase 4 complete.\n");

    keyboard_echo_loop();

    console_write("bmx: halting\n");
    interrupts_disable();
    for (;;) {
        __asm__ volatile("hlt");
    }
}
