#ifndef BMX_CONSOLE_H
#define BMX_CONSOLE_H

#include <stdint.h>

void console_init(void);
void console_putc(char c);
void console_write(const char *text);
void console_hex64(uint64_t value);
void console_hex_width(uint64_t value, unsigned int digits);
void console_dec(uint64_t value);

#endif
