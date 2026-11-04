#ifndef BMX_FBTEXT_H
#define BMX_FBTEXT_H

#include <stdint.h>

/* A character cell console drawn straight onto the linear framebuffer with the
   bundled 8x8 font. Every entry point is a no-op until fbtext_init succeeds. */
#define FB_TEXT_FG 0x00C0C0C0u
#define FB_TEXT_BG 0x00000000u

int fbtext_init(void);
int fbtext_ready(void);
void fbtext_putc(char c);
void fbtext_write(const char *text);
void fbtext_clear(void);
uint32_t fbtext_cols(void);
uint32_t fbtext_rows(void);

#endif
