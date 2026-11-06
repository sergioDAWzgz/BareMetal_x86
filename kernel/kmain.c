#include "acpi.h"
#include "ahci.h"
#include "bootinfo.h"
#include "console.h"
#include "cpu.h"
#include "elf.h"
#include "fat32.h"
#include "fb.h"
#include "fbtext.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "lib.h"
#include "linuxboot.h"
#include "mbr.h"
#include "memory.h"
#include "pci.h"
#include "pic.h"
#include "ramdisk.h"
#include "usermode.h"
#include "vmm.h"

#define TIMER_HZ 100
#define TICKS_TO_OBSERVE 10
#define BARS_VISIBLE_TICKS TIMER_HZ
#define KBD_LINE_MAX 96
/* How long the echo loop waits for a keystroke before yielding to the payload.
   The loop demonstrates the 8042 path; it is not the destination of the boot, so
   an unattended board - and the q35 and UEFI gate paths, which inject no
   keystrokes - still has to reach Linux. */
#define KBD_IDLE_TICKS (5 * TIMER_HZ)
/* A serial line that is unconnected or noisy can deliver a byte forever, and
   every byte resets the idle timer. This caps the whole loop so the payload is
   reached even when the input side never goes quiet. */
#define KBD_ECHO_MAX_TICKS (30 * TIMER_HZ)
/* The stack the CPU switches to on every interrupt taken from ring 3. Separate
   from the boot stack so a user process can never walk into kernel frames, and
   from the user stack so a deep user call chain cannot reach kernel data. */
#define KERNEL_STACK_FRAMES 4
#define KERNEL_STACK_SIZE (KERNEL_STACK_FRAMES * 4096ULL)

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

/* Vector 39 is IRQ7, the vector an 8259 reports when it has nothing to deliver.
   Real hardware issues it and QEMU does not, so the gate raises it explicitly:
   the board halted here before the handler learned to tell a spurious interrupt
   from a real one. */
static void irq_self_test(void)
{
    __asm__ volatile("int $39");
    console_write("irq: spurious interrupt self-test passed\n");
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
    uint64_t last_key = timer_ticks_get();
    uint64_t started = timer_ticks_get();

    console_write("kbd: echo loop on the keyboard or COM1, type and press enter, ");
    console_write("esc to leave\n");

    for (;;) {
        int key = keyboard_getchar();

        if (key < 0) {
            key = serial_getchar();
        }
        if (key < 0) {
            if (timer_ticks_get() - started >= KBD_ECHO_MAX_TICKS) {
                console_write("\nkbd: echo loop time limit, leaving\n");
                return;
            }
            if (timer_ticks_get() - last_key >= KBD_IDLE_TICKS) {
                console_write("\nkbd: idle, leaving the echo loop\n");
                return;
            }
            __asm__ volatile("hlt");
            continue;
        }

        last_key = timer_ticks_get();

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
    ramdisk_file_t staged;
    elf_image_t image;
    uintptr_t kernel_stack;
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

    if (ramdisk_stage("USER.ELF", &staged) != 0) {
        console_write("ramdisk: ");
        console_write(ramdisk_error());
        console_write("\n");
        return;
    }
    ramdisk_print("USER.ELF", &staged);

    /* The user window is a subtree of the live page map, so the loader can only
       place pages once that subtree exists. */
    if (vmm_init() != 0) {
        console_write("vmm: ");
        console_write(vmm_error());
        console_write("\n");
        return;
    }
    if (elf_load(staged.data, staged.size, &image) != 0) {
        console_write("elf: ");
        console_write(elf_error());
        console_write("\n");
        return;
    }

    /* The user subtree is the only one allowed to carry U/S = 1, so the check is
       reported either way: zero is a pass, anything else names the count. */
    console_write("vmm: supervisor entries ");
    console_dec(vmm_audit_supervisor());
    console_write(" above the user window\n");

    console_write("elf: USER.ELF entry ");
    console_hex64(image.entry);
    console_write(" segments ");
    console_dec(image.segments);
    console_write(" pages ");
    console_dec(image.pages);
    console_write("\n");

    /* Every interrupt taken from ring 3 lands on this region, so it has to exist
       and be registered before the drop. It is not user mapped: the CPU reaches
       it through the TSS, and the program has no address for it. */
    kernel_stack = pmm_alloc_frames(KERNEL_STACK_FRAMES);
    if (kernel_stack == 0) {
        console_write("user: no contiguous run for the kernel stack\n");
        return;
    }
    memset(PHYS_TO_VIRT(kernel_stack), 0, KERNEL_STACK_SIZE);
    gdt_set_kernel_stack((uint64_t)(uintptr_t)PHYS_TO_VIRT(kernel_stack) +
                         KERNEL_STACK_SIZE);

    /* The stack is ordinary user memory with nothing in it: the program writes
       to it as soon as it calls a function, which is the first thing it does. */
    if (vmm_map_user(USER_STACK_BASE, USER_STACK_SIZE, 0, 0) < 0) {
        console_write("user: ");
        console_write(vmm_error());
        console_write("\n");
        return;
    }

    console_write("user: entering ring 3 at ");
    console_hex64(image.entry);
    console_write(" with stack ");
    console_hex64(USER_STACK_BASE + USER_STACK_SIZE);
    console_write("\n");

    usermode_run(image.entry, USER_STACK_BASE + USER_STACK_SIZE,
                 GDT_SELECTOR_USER_CODE_RING3, GDT_SELECTOR_USER_DATA_RING3);

    console_write("user: returned to ring 0\n");
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
    console_write("idt:       48 vectors and the syscall gate installed\n");

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

    irq_self_test();

    fault_demo();

    if (serial_self_test() != 0) {
        console_write("serial: loopback self-test failed\n");
    } else {
        console_write("serial: loopback self-test passed\n");
    }

    keyboard_start();

    console_write("\nPhase 3 complete.\n");

    /* Pure and disk-free on purpose: a broken partition table has to be named
       here, not discovered later as a volume that cannot be mounted. */
    if (mbr_self_test() != 0) {
        console_write("mbr: partition table self-test failed: ");
        console_write(mbr_error());
        console_write("\n");
    } else {
        console_write("mbr: partition table self-test passed\n");
    }

    phase4();
    console_write("Phase 4 complete.\n");

    /* The echo loop runs first and yields after a few seconds without a key, so
       the keyboard path stays proven on the one gate path that can inject
       keystrokes without holding the payload for every other path. */
    keyboard_echo_loop();

    if (linux_payload_boot() != 0) {
        console_write("linux: ");
        console_write(linux_payload_error());
        console_write("\n");
    }

    console_write("bmx: halting\n");
    interrupts_disable();
    for (;;) {
        __asm__ volatile("hlt");
    }
}
