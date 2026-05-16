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
#include <cpuid.h>
#include <fbcon.h>
#include <stddef.h>
#include <stdint.h>
#include <video.h>
#include <gfx_proc.h>

/* Bitmap fonts */
extern uint8_t ascii_font[];

static char     char_buffer[256];      // Char buffer
static uint32_t char_buffer_index = 0; // Char buffer index

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
        uint8_t       *dest  = (uint8_t *)buffer;
        const uint8_t *src   = (const uint8_t *)(buffer + stride * font_height);
        size_t         count = stride * (height - font_height) * sizeof(uint32_t);

#if CPU_FEATURE_SSE
        if (cpu_support_sse()) {
            size_t blocks = count / 64;
            size_t remain = count % 64;

            __asm__ volatile("1:\n\t"
                             "movdqu (%[src]), %%xmm0\n\t"
                             "movdqu 16(%[src]), %%xmm1\n\t"
                             "movdqu 32(%[src]), %%xmm2\n\t"
                             "movdqu 48(%[src]), %%xmm3\n\t"
                             "movdqu %%xmm0, (%[dest])\n\t"
                             "movdqu %%xmm1, 16(%[dest])\n\t"
                             "movdqu %%xmm2, 32(%[dest])\n\t"
                             "movdqu %%xmm3, 48(%[dest])\n\t"
                             "add $64, %[src]\n\t"
                             "add $64, %[dest]\n\t"
                             "dec %[blocks]\n\t"
                             "jnz 1b\n\t"
                             : [src] "+r"(src), [dest] "+r"(dest), [blocks] "+r"(blocks)
                             :
                             : "xmm0", "xmm1", "xmm2", "xmm3", "memory");

            for (size_t i = 0; i < remain; i++) *dest++ = *src++;
        } else {
            count /= 8;
            __asm__ volatile("rep movsq" : "+D"(dest), "+S"(src), "+c"(count)::"memory");
        }
#else
        count /= 8;
        __asm__ volatile("rep movsq" : "+D"(dest), "+S"(src), "+c"(count)::"memory");
#endif
        video_draw_rect((position_t) {0, height - font_height}, (position_t) {stride, height}, back_color);
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
            fbcon_draw_char(' ', cx * font_width, cy * font_height, back_color);
        } else if (cy > 0) {
            cy--;
            cx = c_width - 1;
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
