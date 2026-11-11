#ifndef BMX_CONSOLE_H
#define BMX_CONSOLE_H

#include <stdint.h>

void console_init(void);
void console_putc(char c);
void console_write(const char *text);
void console_hex64(uint64_t value);
void console_hex_width(uint64_t value, unsigned int digits);
void console_dec(uint64_t value);

/* Reads one byte from COM1 without blocking, or -1 when the receive buffer is
   empty. A terminal sends CR for Enter and DEL for Backspace, so those are
   translated to the codes the PS/2 map already produces. */
int serial_getchar(void);

/* Writes and reads back through the UART's loopback path, including the CR/DEL
   translation, so the gate proves the receive side before a cable is attached.
   Returns 0 when the three bytes came back as expected. */
int serial_self_test(void);

#endif
