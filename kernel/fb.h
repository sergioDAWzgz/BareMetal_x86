#ifndef BMX_FB_H
#define BMX_FB_H

#include <stdint.h>

typedef struct {
    uint8_t red_size;
    uint8_t red_pos;
    uint8_t green_size;
    uint8_t green_pos;
    uint8_t blue_size;
    uint8_t blue_pos;
} fb_format_t;

int fb_init(void);
int fb_ready(void);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_pitch(void);
uint64_t fb_physical(void);
const fb_format_t *fb_format(void);

/* Raw linear framebuffer seen through the direct map, and the packer for a
   0xRRGGBB colour. Byte-level consumers (the text console) use these instead
   of going through the bounds-checked pixel helpers. */
volatile uint8_t *fb_buffer(void);
uint32_t fb_pack_rgb(uint32_t rgb);

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);
uint32_t fb_read_pixel(uint32_t x, uint32_t y);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rgb);
void fb_clear(uint32_t rgb);
void fb_draw_bars(void);
int fb_self_test(void);

/* Shifts the whole framebuffer up by `pixels` scanlines. The rows uncovered at
   the bottom are left as they were, so the caller paints its own background. */
void fb_scroll_up(uint32_t pixels);

#endif
