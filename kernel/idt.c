#include "idt.h"
#include "console.h"
#include "gdt.h"
#include "keyboard.h"
#include "pic.h"
#include "syscall.h"

#define IDT_FLAG_PRESENT 0x80
#define IDT_FLAG_INTERRUPT_GATE 0x0E
/* Two bits of the type/attributes byte; ring 3 may raise the vector only when
   the gate itself is DPL 3. */
#define IDT_FLAG_DPL_SHIFT 5
#define IDT_STUB_COUNT 48
#define IRQ0_VECTOR 32
#define IRQ1_VECTOR 33
#define IRQ_SPURIOUS 7

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
extern void isr_syscall(void);
extern void divide_error_probe_resume(void);

static idt_entry_t idt[IDT_VECTOR_COUNT];
static idt_pointer_t idt_pointer;
static volatile int expect_fault;
static int spurious_seen;

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

static void idt_set_gate(unsigned int vector, void (*handler)(void),
                         unsigned int dpl)
{
    uint64_t address = (uint64_t)(uintptr_t)handler;

    idt[vector].offset_low = (uint16_t)(address & 0xFFFF);
    idt[vector].selector = GDT_SELECTOR_CODE;
    idt[vector].ist = 0;
    idt[vector].type_attributes =
        IDT_FLAG_PRESENT | IDT_FLAG_INTERRUPT_GATE | (dpl << IDT_FLAG_DPL_SHIFT);
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
        idt_set_gate(vector, isr_stub_table[vector], 0);
    }

    /* The one gate ring 3 is allowed to raise. It stays an interrupt gate, so
       the handler runs with interrupts disabled and cannot be interleaved with
       an IRQ that would touch the same console. */
    idt_set_gate(IDT_SYSCALL_VECTOR, isr_syscall, 3);

    idt_pointer.limit = (uint16_t)(sizeof(idt) - 1);
    idt_pointer.base = (uint64_t)(uintptr_t)&idt[0];
    __asm__ volatile("lidt %0" : : "m"(idt_pointer));
}

void interrupt_dispatch(registers_t *registers)
{
    if (registers->vector == IDT_SYSCALL_VECTOR) {
        syscall_dispatch(registers);
        return;
    }
    exception_dispatch(registers);
}

void exception_expect_fault(void)
{
    expect_fault = 1;
}

/* Hardware IRQs with no driver are not fatal. The 8259 reports an interrupt it
   cannot identify as vector 39 (IRQ7) and sets no in-service bit; that one gets
   no EOI, because acknowledging it would close an interrupt that is still
   pending on another line. Any other unclaimed line is masked before its EOI, so
   a level-triggered device cannot fire again the moment the EOI lands. */
static void unhandled_irq(unsigned int vector)
{
    unsigned int irq = vector - IRQ0_VECTOR;

    if (irq == IRQ_SPURIOUS && (pic_isr() & (1u << IRQ_SPURIOUS)) == 0) {
        if (!spurious_seen) {
            spurious_seen = 1;
            console_write("irq: spurious IRQ7 ignored\n");
        }
        return;
    }

    pic_mask((uint8_t)irq);
    pic_eoi((uint8_t)irq);
    console_write("irq: masked unhandled irq ");
    console_dec(irq);
    console_write("\n");
}

void exception_dispatch(registers_t *frame)
{
    unsigned int vector;

    if (frame->vector >= IRQ0_VECTOR) {
        if (frame->vector == IRQ0_VECTOR) {
            timer_handle();
            return;
        }
        if (frame->vector == IRQ1_VECTOR) {
            keyboard_handle();
            return;
        }
        unhandled_irq((unsigned int)frame->vector);
        return;
    }

    vector = (unsigned int)frame->vector;
    console_write("\nexc: ");
    console_write(exception_names[vector]);
    console_write(" vector=");
    console_dec(vector);
    console_write(" error=");
    console_hex64(frame->error);
    console_write(" rip=");
    console_hex64(frame->rip);
    console_write("\n");

    if (expect_fault) {
        expect_fault = 0;
        if (vector == 0) {
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
