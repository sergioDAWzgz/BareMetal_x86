#include "idt.h"
#include "console.h"
#include "gdt.h"
#include "pic.h"

#define IDT_FLAG_PRESENT 0x80
#define IDT_FLAG_INTERRUPT_GATE 0x0E
#define IDT_STUB_COUNT 48
#define IRQ0_VECTOR 32

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_pointer_t;

extern void (*const isr_stub_table[])(void);
extern void divide_error_probe_resume(void);

static idt_entry_t idt[IDT_VECTOR_COUNT];
static idt_pointer_t idt_pointer;
static volatile int expect_fault;

static const char *const exception_names[32] = {
    "#DE divide error",        "#DB debug",              "NMI",
    "#BP breakpoint",          "#OF overflow",           "#BR bound range",
    "#UD invalid opcode",      "#NM device not available", "#DF double fault",
    "coprocessor segment",     "#TS invalid TSS",        "#NP segment not present",
    "#SS stack fault",         "#GP general protection", "#PF page fault",
    "reserved",                "#MF x87 float error",    "#AC alignment check",
    "#MC machine check",       "#XM SIMD float error",   "#VE virtualisation",
    "#CP control protection",  "reserved",               "reserved",
    "reserved",                "reserved",               "reserved",
    "reserved",                "reserved",               "reserved",
    "reserved",                "reserved"
};

static void idt_set_gate(unsigned int vector, void (*handler)(void))
{
    uint64_t address = (uint64_t)(uintptr_t)handler;

    idt[vector].offset_low = (uint16_t)(address & 0xFFFF);
    idt[vector].selector = GDT_SELECTOR_CODE;
    idt[vector].ist = 0;
    idt[vector].type_attributes = IDT_FLAG_PRESENT | IDT_FLAG_INTERRUPT_GATE;
    idt[vector].offset_mid = (uint16_t)((address >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)(address >> 32);
    idt[vector].reserved = 0;
}

void idt_init(void)
{
    unsigned int vector;
    unsigned int byte;

    for (byte = 0; byte < sizeof(idt); byte++) {
        ((uint8_t *)idt)[byte] = 0;
    }

    for (vector = 0; vector < IDT_STUB_COUNT; vector++) {
        idt_set_gate(vector, isr_stub_table[vector]);
    }

    idt_pointer.limit = (uint16_t)(sizeof(idt) - 1);
    idt_pointer.base = (uint64_t)(uintptr_t)&idt[0];
    __asm__ volatile("lidt %0" : : "m"(idt_pointer));
}

void exception_expect_fault(void)
{
    expect_fault = 1;
}

void exception_dispatch(interrupt_frame_t *frame)
{
    if (frame->vector == IRQ0_VECTOR) {
        timer_handle();
        return;
    }

    console_write("\nexc: ");
    if (frame->vector < 32) {
        console_write(exception_names[frame->vector]);
    } else {
        console_write("unexpected interrupt");
    }
    console_write(" vector=");
    console_dec(frame->vector);
    console_write(" error=");
    console_hex64(frame->error);
    console_write(" rip=");
    console_hex64(frame->rip);
    console_write("\n");

    if (expect_fault) {
        expect_fault = 0;
        if (frame->vector == 0) {
            /* The probe divides by zero; resume after the faulting instruction. */
            frame->rip = (uint64_t)(uintptr_t)divide_error_probe_resume;
        }
        return;
    }

    console_write("exc: unhandled, halting\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
