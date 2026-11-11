#include "console.h"
#include "fbtext.h"
#include "io.h"

#define COM1_PORT 0x3F8

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR 0x0A

/* VGA text buffer seen through the direct map. */
static volatile uint16_t *const vga_text = (volatile uint16_t *)0xFFFF8000000B8000ULL;

static unsigned int vga_row;
static unsigned int vga_col;

static void serial_init(void)
{
    outb(COM1_PORT + 1, 0x00); /* no UART interrupts */
    outb(COM1_PORT + 3, 0x80); /* DLAB on */
    outb(COM1_PORT + 0, 0x01); /* divisor 1 = 115200 baud */
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03); /* 8N1, DLAB off */
    outb(COM1_PORT + 2, 0xC7); /* FIFO enable and clear */
    outb(COM1_PORT + 4, 0x0B); /* DTR, RTS, OUT2 */
}

static void serial_putc(char c)
{
    while ((inb(COM1_PORT + 5) & 0x20) == 0) {
    }
    outb(COM1_PORT, (uint8_t)c);
}

#define UART_LSR_DATA_READY 0x01

int serial_getchar(void)
{
    uint8_t status = inb(COM1_PORT + 5);
    uint8_t value;

    /* An absent or disabled UART leaves the bus floating at 0xFF, where the
       data-ready bit is set forever. Treating that as input would hand every
       caller a phantom byte for as long as it kept polling. */
    if (status == 0xFF) {
        return -1;
    }
    if ((status & UART_LSR_DATA_READY) == 0) {
        return -1;
    }
    value = inb(COM1_PORT);

    /* Terminals are line oriented the way a VT100 was: CR for Enter and DEL for
       Backspace. The receive path speaks the codes the PS/2 map produces, so the
       transport translates instead of teaching every caller two encodings. */
    if (value == '\r') {
        return '\n';
    }
    if (value == 0x7F) {
        return '\b';
    }
    return (int)value;
}

/* Waits for one received byte, then reads it through serial_getchar. Returns the
   byte, or -1 if nothing arrived before the spin budget ran out. */
static int serial_wait_getchar(void)
{
    unsigned int spin;

    for (spin = 0; spin < 100000; spin++) {
        int value = serial_getchar();

        if (value >= 0) {
            return value;
        }
    }
    return -1;
}

int serial_self_test(void)
{
    uint8_t mcr;
    unsigned int spin;
    int ok = 1;

    /* Discard anything a terminal sent before the test, so the loopback bytes
       are the only ones in the receive FIFO. The loop is bounded because an
       absent UART reports data-ready forever. */
    for (spin = 0; spin < 64; spin++) {
        if ((inb(COM1_PORT + 5) & UART_LSR_DATA_READY) == 0) {
            break;
        }
        (void)inb(COM1_PORT);
    }

    mcr = inb(COM1_PORT + 4);
    outb(COM1_PORT + 4, (uint8_t)(mcr | 0x10)); /* loopback: TX feeds RX */

    outb(COM1_PORT, 'B');
    if (serial_wait_getchar() != 'B') {
        ok = 0;
    }
    outb(COM1_PORT, '\r');
    if (serial_wait_getchar() != '\n') {
        ok = 0;
    }
    outb(COM1_PORT, 0x7F);
    if (serial_wait_getchar() != '\b') {
        ok = 0;
    }

    outb(COM1_PORT + 4, mcr);
    return ok ? 0 : -1;
}

static uint16_t vga_cell(char c)
{
    return (uint16_t)((uint16_t)VGA_ATTR << 8) | (uint8_t)c;
}

static void vga_clear(void)
{
    unsigned int i;

    for (i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga_text[i] = vga_cell(' ');
    }
    vga_row = 0;
    vga_col = 0;
}

static void vga_scroll(void)
{
    unsigned int i;

    for (i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++) {
        vga_text[i] = vga_text[i + VGA_COLS];
    }
    for (i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++) {
        vga_text[i] = vga_cell(' ');
    }
    vga_row = VGA_ROWS - 1;
}

static void vga_putc(char c)
{
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else {
        vga_text[vga_row * VGA_COLS + vga_col] = vga_cell(c);
        vga_col++;
        if (vga_col >= VGA_COLS) {
            vga_col = 0;
            vga_row++;
        }
    }

    if (vga_row >= VGA_ROWS) {
        vga_scroll();
    }
}

void console_init(void)
{
    serial_init();
    vga_clear();
}

void console_putc(char c)
{
    serial_putc(c);
    vga_putc(c);
    fbtext_putc(c);
}

void console_write(const char *text)
{
    while (*text != '\0') {
        console_putc(*text);
        text++;
    }
}

void console_hex_width(uint64_t value, unsigned int digits)
{
    static const char table[] = "0123456789abcdef";
    unsigned int shift;

    for (shift = digits; shift > 0; shift--) {
        unsigned int nibble = (unsigned int)((value >> ((shift - 1) * 4)) & 0x0F);

        console_putc(table[nibble]);
    }
}

void console_hex64(uint64_t value)
{
    console_write("0x");
    console_hex_width(value, 16);
}

void console_dec(uint64_t value)
{
    char buffer[21];
    unsigned int index = 0;

    if (value == 0) {
        console_putc('0');
        return;
    }
    while (value > 0) {
        buffer[index++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (index > 0) {
        console_putc(buffer[--index]);
    }
}
