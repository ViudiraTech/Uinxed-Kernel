/*
 *
 *      fbcon.c
 *      Framebuffer console
 *
 *      2026/5/16 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <libs/gfxs/gfx_proc.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <video/fbcon.h>
#include <video/video.h>

/* Bitmap fonts */
extern uint8_t ascii_font[];

static char      char_buffer[256];      // Char buffer
static uint32_t  char_buffer_index = 0; // Char buffer index
static char     *text_grid         = 0;
static uint32_t *color_grid        = 0;
static uint32_t *dirty_first_col   = 0;
static uint32_t *dirty_last_col    = 0;
static uint8_t   full_redraw_pending;
static uint8_t   redraw_deferred;

static void fbcon_mark_cell_dirty(uint32_t row, uint32_t col)
{
    if (!dirty_first_col || !dirty_last_col || row >= c_height || col >= c_width) return;

    if (dirty_first_col[row] > col) dirty_first_col[row] = col;
    if (dirty_last_col[row] < col) dirty_last_col[row] = col;
}

static void fbcon_clear_row(uint32_t row)
{
    if (!text_grid || !color_grid || row >= c_height) return;

    memset(text_grid + (size_t)row * c_width, ' ', c_width);
    for (uint32_t col = 0; col < c_width; col++) color_grid[(size_t)row * c_width + col] = fore_color;
}

static void fbcon_redraw_row_range(uint32_t row, uint32_t first_col, uint32_t last_col)
{
    if (!text_grid || !color_grid || row >= c_height) return;
    if (first_col >= c_width || last_col >= c_width || first_col > last_col) return;

    for (uint32_t col = first_col; col <= last_col; col++) {
        size_t index = (size_t)row * c_width + col;
        fbcon_draw_char(text_grid[index], col * font_width, row * font_height, color_grid[index]);
    }
}

static void fbcon_flush_dirty_rows(void)
{
    if (!dirty_first_col || !dirty_last_col) return;
    for (uint32_t row = 0; row < c_height; row++) {
        if (dirty_first_col[row] > dirty_last_col[row]) continue;
        fbcon_redraw_row_range(row, dirty_first_col[row], dirty_last_col[row]);
        dirty_first_col[row] = c_width;
        dirty_last_col[row]  = 0;
    }
}

static void fbcon_redraw_screen(void)
{
    if (!text_grid || !color_grid) return;

    for (uint32_t row = 0; row < c_height; row++) {
        fbcon_redraw_row_range(row, 0, c_width ? c_width - 1 : 0);
        if (dirty_first_col && dirty_last_col) {
            dirty_first_col[row] = c_width;
            dirty_last_col[row]  = 0;
        }
    }
}

static void fbcon_clear_uncovered_bottom(void)
{
    uint32_t used_height = c_height * font_height;
    if (used_height < height) video_draw_rect((position_t) {0, used_height}, (position_t) {stride - 1, height - 1}, back_color);
}

static void fbcon_flush_screen_updates(void)
{
    if (redraw_deferred) return;

    if (full_redraw_pending) {
        fbcon_redraw_screen();
        fbcon_clear_uncovered_bottom();
        full_redraw_pending = 0;
        return;
    }

    fbcon_flush_dirty_rows();
}

/* Initialize framebuffer console */
void fbcon_init(void)
{
    /* Bitmap font: 9x16 pixels */
    font_width  = 9;
    font_height = 16;

    cx = cy  = 0;
    c_width  = width / font_width;
    c_height = height / font_height;

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});

    text_grid       = calloc((size_t)c_width * c_height, sizeof(char));
    color_grid      = malloc((size_t)c_width * c_height * sizeof(uint32_t));
    dirty_first_col = malloc((size_t)c_height * sizeof(uint32_t));
    dirty_last_col  = malloc((size_t)c_height * sizeof(uint32_t));
    if (text_grid && color_grid && dirty_first_col && dirty_last_col) {
        for (uint32_t row = 0; row < c_height; row++) {
            fbcon_clear_row(row);
            dirty_first_col[row] = c_width;
            dirty_last_col[row]  = 0;
        }
    }

    char_buffer_index   = 0;
    char_buffer[0]      = '\0';
    full_redraw_pending = 0;
    redraw_deferred     = 0;
}

/* Scroll to a position that units are characters */
void fbcon_move_to(uint32_t c_x, uint32_t c_y)
{
    cx = c_x;
    cy = c_y;
}

/* Screen scrolling operation */
void fbcon_scroll(void)
{
    if ((uint32_t)cx >= c_width) {
        cx = 1;
        cy++;
    }

    if ((uint32_t)cy >= c_height) {
        if (text_grid && color_grid) {
            memmove(text_grid, text_grid + c_width, (size_t)(c_height - 1) * c_width);
            memmove(color_grid, color_grid + c_width, (size_t)(c_height - 1) * c_width * sizeof(uint32_t));
            if (dirty_first_col && dirty_last_col) {
                memmove(dirty_first_col, dirty_first_col + 1, (size_t)(c_height - 1) * sizeof(uint32_t));
                memmove(dirty_last_col, dirty_last_col + 1, (size_t)(c_height - 1) * sizeof(uint32_t));
                dirty_first_col[c_height - 1] = 0;
                dirty_last_col[c_height - 1]  = c_width ? c_width - 1 : 0;
            }
            fbcon_clear_row(c_height - 1);
            full_redraw_pending = 1;
        } else {
            video_draw_rect((position_t) {0, 0}, (position_t) {stride, height}, back_color);
        }
        cy = c_height - 1;
    }
}

/* Draw a character at the specified coordinates on the screen */
void fbcon_draw_char(const char c, uint32_t x, uint32_t y, uint32_t color)
{
    uint8_t *char_font      = ascii_font + (size_t)c * font_height;
    uint32_t char_base_addr = y * stride + x;

    for (uint32_t row = 0; row < font_height; row++) {
        uint32_t *row_buf  = buffer + char_base_addr + row * stride;
        uint8_t   font_row = char_font[row];
        for (uint32_t col = 0; col < font_width; col++) row_buf[col] = (font_row & (0x80 >> col)) ? color : back_color;
    }
}

/* Flush character buffer to screen */
void fbcon_flush_buffer(uint32_t color)
{
    if (char_buffer_index > 0) {
        for (uint32_t i = 0; i < char_buffer_index; i++) {
            char c = char_buffer[i];
            if (text_grid && color_grid && cx + i < c_width && cy < c_height) {
                size_t index      = (size_t)cy * c_width + (cx + i);
                text_grid[index]  = c;
                color_grid[index] = color;
                fbcon_mark_cell_dirty(cy, cx + i);
            } else {
                uint32_t start_x        = cx * font_width;
                uint32_t start_y        = cy * font_height;
                uint32_t base_y_stride  = start_y * stride;
                uint32_t char_x         = start_x + i * font_width;
                uint8_t *char_font      = ascii_font + (size_t)c * font_height;
                uint32_t char_base_addr = base_y_stride + char_x;

                for (uint32_t row = 0; row < font_height; row++) {
                    uint32_t *row_buf  = buffer + char_base_addr + row * stride;
                    uint8_t   font_row = char_font[row];
                    for (uint32_t col = 0; col < font_width; col++) row_buf[col] = (font_row & (0x80 >> col)) ? color : back_color;
                }
            }
        }
        cx += char_buffer_index;
        char_buffer_index = 0;
        char_buffer[0]    = '\0';
    }
    fbcon_flush_screen_updates();
}

/* Print a character at the specified coordinates on the screen */
void fbcon_put_char(const char c, uint32_t color)
{
    if (c == '\n') {
        fbcon_flush_buffer(color);
        cy++;
        cx = 0;
        if (cy >= c_height) fbcon_scroll();
        return;
    }
    if (c == '\r') {
        fbcon_flush_buffer(color);
        cx = 0;
        return;
    }
    if (c == '\t') {
        fbcon_flush_buffer(color);
        cx = (cx + 8) & ~7;
        if (cx >= c_width) {
            cx = 0;
            cy++;
        }
        return;
    }
    if (c == '\b') {
        fbcon_flush_buffer(color);
        if (cx > 0) {
            cx--;
            if (text_grid && color_grid && cy < c_height) {
                size_t index      = (size_t)cy * c_width + cx;
                text_grid[index]  = ' ';
                color_grid[index] = back_color;
                fbcon_mark_cell_dirty(cy, cx);
            }
            fbcon_flush_screen_updates();
        } else if (cy > 0) {
            cy--;
            cx = c_width - 1;
            if (text_grid && color_grid) {
                size_t index      = (size_t)cy * c_width + cx;
                text_grid[index]  = ' ';
                color_grid[index] = back_color;
                fbcon_mark_cell_dirty(cy, cx);
            }
            fbcon_flush_screen_updates();
        }
        return;
    }
    if (cy >= c_height) fbcon_scroll();
    if (char_buffer_index < 256 - 1) char_buffer[char_buffer_index++] = c;
    if (char_buffer_index >= 256 - 1 || cx + char_buffer_index >= c_width) fbcon_flush_buffer(color);
    if (cx >= c_width) {
        cx = 0;
        cy++;
    }
    if (cy >= c_height) fbcon_scroll();
}

/* Print a string at the specified coordinates on the screen */
void fbcon_put_string(const char *str)
{
    redraw_deferred++;
    for (; *str; ++str) fbcon_put_char(*str, fore_color);
    fbcon_flush_buffer(fore_color);
    redraw_deferred--;
    fbcon_flush_screen_updates();
}

/* Print a string with color at the specified coordinates on the screen */
void fbcon_put_string_color(const char *str, uint32_t color)
{
    redraw_deferred++;
    for (; *str; ++str) fbcon_put_char(*str, color);
    fbcon_flush_buffer(color);
    redraw_deferred--;
    fbcon_flush_screen_updates();
}
