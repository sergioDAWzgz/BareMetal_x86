#include "pic.h"
#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43
#define PIT_BASE_FREQUENCY 1193182u

static volatile uint64_t timer_ticks;

void pic_init(void)
{
    outb(PIC1_COMMAND, 0x11); /* ICW1: initialise, expect ICW4 */
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();
    outb(PIC1_DATA, 0x20);    /* ICW2: master vectors at 0x20 */
    io_wait();
    outb(PIC2_DATA, 0x28);    /* ICW2: slave vectors at 0x28 */
    io_wait();
    outb(PIC1_DATA, 0x04);    /* ICW3: slave on IRQ2 */
    io_wait();
    outb(PIC2_DATA, 0x02);    /* ICW3: slave cascade identity */
    io_wait();
    outb(PIC1_DATA, 0x01);    /* ICW4: 8086 mode */
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, 0xFE);    /* unmask IRQ0 only */
    outb(PIC2_DATA, 0xFF);
}

void pit_init(uint32_t hz)
{
    uint32_t divisor = PIT_BASE_FREQUENCY / hz;

    outb(PIT_COMMAND, 0x36);  /* channel 0, lo/hi byte, mode 3 */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_COMMAND, 0x20);
    }
    outb(PIC1_COMMAND, 0x20);
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t line = (uint8_t)(irq < 8 ? irq : irq - 8);

    outb(port, (uint8_t)(inb(port) & ~(1u << line)));
}

void pic_mask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t line = (uint8_t)(irq < 8 ? irq : irq - 8);

    outb(port, (uint8_t)(inb(port) | (1u << line)));
}

/* Reads the master in-service register through OCW3. The 8259 raises IRQ7 and
   sets no in-service bit when it has nothing to deliver, which is exactly how a
   spurious interrupt is told apart from a real one. */
uint8_t pic_isr(void)
{
    outb(PIC1_COMMAND, 0x0B);  /* OCW3: select the in-service register */
    return inb(PIC1_COMMAND);
}

void timer_handle(void)
{
    timer_ticks++;
    pic_eoi(0);
}

uint64_t timer_ticks_get(void)
{
    return timer_ticks;
}
