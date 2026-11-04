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
