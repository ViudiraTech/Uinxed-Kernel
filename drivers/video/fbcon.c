/*
 *
 *      fbcon.c
 *      Framebuffer console
 *
 *      2026/5/16 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <fbcon.h>
#include <heap.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <video.h>
#include <gfx_proc.h>

/* Bitmap fonts */
extern uint8_t ascii_font[];

static char     char_buffer[256];      // Char buffer
static uint32_t char_buffer_index = 0; // Char buffer index
static char    *text_grid         = 0;
static uint32_t *color_grid       = 0;

static void fbcon_clear_row(uint32_t row)
{
    if (!text_grid || !color_grid || row >= c_height) return;

    memset(text_grid + (size_t)row * c_width, ' ', c_width);
    for (uint32_t col = 0; col < c_width; col++) color_grid[(size_t)row * c_width + col] = fore_color;
}

static void fbcon_redraw_row(uint32_t row)
{
    if (!text_grid || !color_grid || row >= c_height) return;

    for (uint32_t col = 0; col < c_width; col++) {
        size_t index = (size_t)row * c_width + col;
        fbcon_draw_char(text_grid[index], col * font_width, row * font_height, color_grid[index]);
    }
}

static void fbcon_redraw_all(void)
{
    if (!text_grid || !color_grid) return;
    for (uint32_t row = 0; row < c_height; row++) fbcon_redraw_row(row);
}

/* Initialize framebuffer console */
void fbcon_init(void)
{
    /* Bitmap font: 9x16 pixels */
    font_width  = 9;
    font_height = 16;

    cx = cy = 0;
    c_width  = width / font_width;
    c_height = height / font_height;

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});

    text_grid  = calloc((size_t)c_width * c_height, sizeof(char));
    color_grid = malloc((size_t)c_width * c_height * sizeof(uint32_t));
    if (text_grid && color_grid) {
        for (uint32_t row = 0; row < c_height; row++) fbcon_clear_row(row);
    }

    char_buffer_index = 0;
    char_buffer[0]    = '\0';
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
            fbcon_clear_row(c_height - 1);
            fbcon_redraw_all();
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
        uint32_t start_x       = cx * font_width;
        uint32_t start_y       = cy * font_height;
        uint32_t base_y_stride = start_y * stride;

        for (uint32_t i = 0; i < char_buffer_index; i++) {
            char     c              = char_buffer[i];
            uint32_t char_x         = start_x + i * font_width;
            uint8_t *char_font      = ascii_font + (size_t)c * font_height;
            uint32_t char_base_addr = base_y_stride + char_x;

            for (uint32_t row = 0; row < font_height; row++) {
                uint32_t *row_buf  = buffer + char_base_addr + row * stride;
                uint8_t   font_row = char_font[row];
                for (uint32_t col = 0; col < font_width; col++) row_buf[col] = (font_row & (0x80 >> col)) ? color : back_color;
            }

            if (text_grid && color_grid && cx + i < c_width && cy < c_height) {
                size_t index      = (size_t)cy * c_width + (cx + i);
                text_grid[index]  = c;
                color_grid[index] = color;
            }
        }
        cx += char_buffer_index;
        char_buffer_index = 0;
        char_buffer[0]    = '\0';
    }
}

/* Print a character at the specified coordinates on the screen */
void fbcon_put_char(const char c, uint32_t color)
{
    if (c == '\n') {
        fbcon_flush_buffer(color);
        cy++;
        cx = 0;
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
            }
            fbcon_draw_char(' ', cx * font_width, cy * font_height, back_color);
        } else if (cy > 0) {
            cy--;
            cx = c_width - 1;
            if (text_grid && color_grid) {
                size_t index      = (size_t)cy * c_width + cx;
                text_grid[index]  = ' ';
                color_grid[index] = back_color;
            }
            fbcon_draw_char(' ', cx * font_width, cy * font_height, back_color);
        }
        return;
    }
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
    for (; *str; ++str) fbcon_put_char(*str, fore_color);
    fbcon_flush_buffer(fore_color);
}

/* Print a string with color at the specified coordinates on the screen */
void fbcon_put_string_color(const char *str, uint32_t color)
{
    for (; *str; ++str) fbcon_put_char(*str, color);
    fbcon_flush_buffer(color);
}
