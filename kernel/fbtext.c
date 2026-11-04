#include "fbtext.h"
#include "fb.h"
#include "font8x8.h"

/* Bounds for the in-memory cursor state. The real size comes from the mode. */
#define FB_TEXT_MAX_COLS 192
#define FB_TEXT_MAX_ROWS 160

static volatile uint8_t *text_base;
static uint32_t text_pitch;
static uint32_t text_cols;
static uint32_t text_rows;
static uint32_t cursor_col;
static uint32_t cursor_row;
static uint32_t color_fg;
static uint32_t color_bg;
static int text_ready;

static void draw_glyph(uint32_t column, uint32_t row, unsigned char character)
{
    const uint8_t *glyph;
    uint32_t line;

    if (character < FONT_FIRST_ASCII || character > FONT_LAST_ASCII) {
        character = '?';
    }
    glyph = font8x8_basic[character - FONT_FIRST_ASCII];

    for (line = 0; line < FONT_GLYPH_HEIGHT; line++) {
        volatile uint32_t *cell = (volatile uint32_t *)(text_base +
            (uint64_t)(row * FONT_GLYPH_HEIGHT + line) * text_pitch +
            (uint64_t)column * FONT_GLYPH_WIDTH * 4);
        uint8_t bits = glyph[line];
        uint32_t pixel;

        for (pixel = 0; pixel < FONT_GLYPH_WIDTH; pixel++) {
            cell[pixel] = (bits & (1u << pixel)) != 0 ? color_fg : color_bg;
        }
    }
}

static void scroll_one_line(void)
{
    fb_scroll_up(FONT_GLYPH_HEIGHT);
    fb_fill_rect(0, (text_rows - 1) * FONT_GLYPH_HEIGHT,
                 fb_width(), FONT_GLYPH_HEIGHT, FB_TEXT_BG);
    cursor_row = text_rows - 1;
}

int fbtext_init(void)
{
    if (!fb_ready()) {
        return -1;
    }
    if (fb_width() < FONT_GLYPH_WIDTH || fb_height() < FONT_GLYPH_HEIGHT) {
        return -1;
    }

    text_base = fb_buffer();
    text_pitch = fb_pitch();
    text_cols = fb_width() / FONT_GLYPH_WIDTH;
    text_rows = fb_height() / FONT_GLYPH_HEIGHT;

    if (text_cols > FB_TEXT_MAX_COLS) {
        text_cols = FB_TEXT_MAX_COLS;
    }
    if (text_rows > FB_TEXT_MAX_ROWS) {
        text_rows = FB_TEXT_MAX_ROWS;
    }

    color_fg = fb_pack_rgb(FB_TEXT_FG);
    color_bg = fb_pack_rgb(FB_TEXT_BG);

    cursor_col = 0;
    cursor_row = 0;
    text_ready = 1;

    fbtext_clear();
    return 0;
}

int fbtext_ready(void)
{
    return text_ready;
}

uint32_t fbtext_cols(void)
{
    return text_cols;
}

uint32_t fbtext_rows(void)
{
    return text_rows;
}

void fbtext_clear(void)
{
    if (!text_ready) {
        return;
    }
    fb_fill_rect(0, 0, text_cols * FONT_GLYPH_WIDTH,
                 text_rows * FONT_GLYPH_HEIGHT, FB_TEXT_BG);
    cursor_col = 0;
    cursor_row = 0;
}

void fbtext_putc(char c)
{
    unsigned char character = (unsigned char)c;

    if (!text_ready) {
        return;
    }

    if (character == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (character == '\r') {
        cursor_col = 0;
    } else if (character == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            draw_glyph(cursor_col, cursor_row, ' ');
        }
    } else if (character == '\t') {
        do {
            draw_glyph(cursor_col, cursor_row, ' ');
            cursor_col++;
            if (cursor_col >= text_cols) {
                cursor_col = 0;
                cursor_row++;
            }
        } while ((cursor_col % 4) != 0);
    } else if (character < FONT_FIRST_ASCII || character > FONT_LAST_ASCII) {
        return;
    } else {
        draw_glyph(cursor_col, cursor_row, character);
        cursor_col++;
        if (cursor_col >= text_cols) {
            cursor_col = 0;
            cursor_row++;
        }
    }

    while (cursor_row >= text_rows) {
        scroll_one_line();
    }
}

void fbtext_write(const char *text)
{
    while (*text != '\0') {
        fbtext_putc(*text);
        text++;
    }
}
