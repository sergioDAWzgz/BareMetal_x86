#include "fb.h"
#include "bootinfo.h"
#include "console.h"
#include "memory.h"

#define FB_MAX_WIDTH 4096
#define FB_MAX_HEIGHT 4096
#define FB_TEST_SIZE 8

static volatile uint8_t *fb_base;
static uint32_t fb_w;
static uint32_t fb_h;
static uint32_t fb_stride;
static uint32_t fb_bpp;
static uint64_t fb_phys;
static fb_format_t fb_fmt;
static int fb_ok;

static uint32_t pack_channel(uint32_t value, uint8_t size, uint8_t position)
{
    if (size == 0 || size > 8) {
        return 0;
    }
    return ((value >> (8 - size)) & ((1u << size) - 1u)) << position;
}

static uint32_t fb_pack(uint32_t rgb)
{
    uint32_t red = (rgb >> 16) & 0xFF;
    uint32_t green = (rgb >> 8) & 0xFF;
    uint32_t blue = rgb & 0xFF;

    return pack_channel(red, fb_fmt.red_size, fb_fmt.red_pos) |
           pack_channel(green, fb_fmt.green_size, fb_fmt.green_pos) |
           pack_channel(blue, fb_fmt.blue_size, fb_fmt.blue_pos);
}

int fb_init(void)
{
    const boot_info_t *info = bootinfo_get();

    fb_ok = 0;

    if (!bootinfo_valid()) {
        return -1;
    }
    if (info->framebuffer == 0 || info->width == 0 || info->height == 0) {
        return -1;
    }
    if (info->bpp != 32) {
        return -1;
    }
    if (info->width > FB_MAX_WIDTH || info->height > FB_MAX_HEIGHT) {
        return -1;
    }
    if (info->pitch < info->width * 4) {
        return -1;
    }

    fb_base = (volatile uint8_t *)PHYS_TO_VIRT(info->framebuffer);
    fb_w = info->width;
    fb_h = info->height;
    fb_stride = info->pitch;
    fb_bpp = info->bpp;
    fb_phys = info->framebuffer;

    fb_fmt.red_size = info->red_size;
    fb_fmt.red_pos = info->red_pos;
    fb_fmt.green_size = info->green_size;
    fb_fmt.green_pos = info->green_pos;
    fb_fmt.blue_size = info->blue_size;
    fb_fmt.blue_pos = info->blue_pos;

    if (fb_fmt.red_size == 0 || fb_fmt.green_size == 0 || fb_fmt.blue_size == 0) {
        return -1;
    }

    fb_ok = 1;
    return 0;
}

int fb_ready(void)
{
    return fb_ok;
}

uint32_t fb_width(void)
{
    return fb_w;
}

uint32_t fb_height(void)
{
    return fb_h;
}

uint32_t fb_pitch(void)
{
    return fb_stride;
}

uint64_t fb_physical(void)
{
    return fb_phys;
}

const fb_format_t *fb_format(void)
{
    return &fb_fmt;
}

volatile uint8_t *fb_buffer(void)
{
    return fb_base;
}

uint32_t fb_pack_rgb(uint32_t rgb)
{
    return fb_pack(rgb);
}

static volatile uint32_t *fb_slot(uint32_t x, uint32_t y)
{
    return (volatile uint32_t *)(fb_base + (uint64_t)y * fb_stride + (uint64_t)x * 4);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (!fb_ok || x >= fb_w || y >= fb_h) {
        return;
    }
    *fb_slot(x, y) = fb_pack(rgb);
}

uint32_t fb_read_pixel(uint32_t x, uint32_t y)
{
    if (!fb_ok || x >= fb_w || y >= fb_h) {
        return 0;
    }
    return *fb_slot(x, y);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rgb)
{
    uint32_t row;
    uint32_t column;
    uint32_t value;

    if (!fb_ok) {
        return;
    }

    value = fb_pack(rgb);
    for (row = y; row < y + height && row < fb_h; row++) {
        for (column = x; column < x + width && column < fb_w; column++) {
            *fb_slot(column, row) = value;
        }
    }
}

void fb_clear(uint32_t rgb)
{
    fb_fill_rect(0, 0, fb_w, fb_h, rgb);
}

void fb_scroll_up(uint32_t pixels)
{
    uint32_t row;
    uint64_t shift;

    if (!fb_ok || pixels == 0 || pixels >= fb_h) {
        return;
    }

    shift = (uint64_t)pixels * fb_stride;
    for (row = 0; row + pixels < fb_h; row++) {
        volatile uint8_t *dst = fb_base + (uint64_t)row * fb_stride;
        const volatile uint8_t *src = dst + shift;
        uint32_t column;

        for (column = 0; column < fb_stride; column++) {
            dst[column] = src[column];
        }
    }
}

/* Writes, reads back and compares every primary plus black and white, which
   proves the framebuffer is reachable and writable through the direct map. */
int fb_self_test(void)
{
    static const uint32_t colors[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000 };
    unsigned int index;

    if (!fb_ok) {
        return -1;
    }

    for (index = 0; index < sizeof(colors) / sizeof(colors[0]); index++) {
        uint32_t expected = fb_pack(colors[index]);

        fb_fill_rect(0, 0, FB_TEST_SIZE, FB_TEST_SIZE, colors[index]);
        if (fb_read_pixel(0, 0) != expected) {
            return -1;
        }
        if (fb_read_pixel(FB_TEST_SIZE - 1, FB_TEST_SIZE - 1) != expected) {
            return -1;
        }
    }

    fb_clear(0x000000);
    return 0;
}

void fb_draw_bars(void)
{
    static const uint32_t bars[] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
        0xFF00FF, 0xFF0000, 0x0000FF, 0x000000
    };
    unsigned int count = (unsigned int)(sizeof(bars) / sizeof(bars[0]));
    uint32_t bar_width = fb_w / count;
    unsigned int index;

    if (!fb_ok) {
        return;
    }

    for (index = 0; index < count; index++) {
        fb_fill_rect(index * bar_width, 0, bar_width, fb_h, bars[index]);
    }
}
