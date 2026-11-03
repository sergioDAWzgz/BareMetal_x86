#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "memory.h"
#include "pic.h"

#define TIMER_HZ 100
#define TICKS_TO_OBSERVE 50

static void wait_ticks(uint64_t count)
{
    uint64_t start = timer_ticks_get();

    while (timer_ticks_get() - start < count) {
        __asm__ volatile("hlt");
    }
}

extern void divide_error_probe(void);

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

void kmain(void)
{
    char brand[49];

    console_init();

    console_write("\n");
    console_write("BareMetal_x86 - phase 2\n");
    console_write("=======================\n");
    console_write("mode:      64-bit long mode (IA-32e)\n");
    console_write("paging:    4-level, 2 MiB pages, identity + higher half\n");

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

    memmap_print();

    pmm_init();
    pmm_demo();

    interrupts_enable();
    wait_ticks(TICKS_TO_OBSERVE);
    console_write("pit:       ");
    console_dec(timer_ticks_get());
    console_write(" ticks observed\n");

    fault_demo();

    console_write("\nPhase 2 complete. Halting.\n");
    interrupts_disable();
    for (;;) {
        __asm__ volatile("hlt");
    }
}
