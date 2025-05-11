/*
 *
 *      video.c
 *      Basic Video
 *
 *      2024/9/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "video.h"
#include "common.h"
#include "limine.h"
#include "stddef.h"
#include "stdint.h"
#include "string.h"

__attribute__((used, section(".limine_requests"))) volatile struct limine_framebuffer_request framebuffer_request = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

extern uint8_t ascfont[]; // Fonts

uint64_t width;   // Screen length
uint64_t height;  // Screen width
uint64_t stride;  // Frame buffer line spacing
uint32_t *buffer; // Video Memory

uint32_t x, y;              // The current absolute cursor position
uint32_t cx, cy;            // The character position of the current cursor
uint32_t c_width, c_height; // Screen character width and height

uint32_t fore_color; // Foreground color
uint32_t back_color; // Background color

/* Get video information */
VideoInfo video_get_info()
{
    VideoInfo info;
    info.width      = width;
    info.height     = height;
    info.stride     = stride;
    info.c_width    = c_width;
    info.c_height   = c_height;
    info.cx         = cx;
    info.cy         = cy;
    info.fore_color = fore_color;
    info.back_color = back_color;
    return info;
}

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void)
{
    return framebuffer_request.response->framebuffers[0];
}

/* Initialize Video */
void video_init(void)
{
    if (framebuffer_request.response == 0 || framebuffer_request.response->framebuffer_count < 1) krn_halt();
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    buffer                                 = framebuffer->address;
    width                                  = framebuffer->width;
    height                                 = framebuffer->height;
    stride                                 = framebuffer->pitch / 4;

    x = cx = y = cy = 0;
    c_width         = width / 9;
    c_height        = height / 16;

    fore_color = 0xffaaaaaa;
    back_color = 0xff000000;
    video_clear();
}

/* Clear screen */
void video_clear(void)
{
    for (uint32_t i = 0; i < (width * height); i++) buffer[i] = 0xff000000;
    back_color = 0xff000000;
    x          = 2;
    y          = 0;
    cx = cy = 0;
}

/* Clear screen with color */
void video_clear_color(int color)
{
    for (uint32_t i = 0; i < (width * height); i++) buffer[i] = color;
    back_color = color;
    x          = 2;
    y          = 0;
    cx = cy = 0;
}

/* Scroll the screen to the specified coordinates */
void video_move_to(uint32_t c_x, uint32_t c_y)
{
    cx = c_x;
    cy = c_y;
}

/* Screen scrolling operation */
void video_scroll(void)
{
    if ((uint32_t)cx >= c_width) {
        cx = 1;
        cy++;
    } else {
        cx++;
    }
    uint8_t *dest;
    const uint8_t *src;
    size_t count;
    if ((uint32_t)cy >= c_height) {
        dest  = (uint8_t *)buffer;
        src   = (const uint8_t *)(buffer + stride * 16);
        count = (width * (height - 16) * sizeof(uint32_t)) / 8;
        __asm__ volatile("rep movsq" : "+D"(dest), "+S"(src), "+c"(count)::"memory");

        // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
        memset(buffer + (height - 16) * stride, (int32_t)back_color, 16 * stride * sizeof(uint32_t));
        cy = c_height - 1;
    }
}

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    (buffer)[y * width + x] = color;
}

/* Get a pixel at the specified coordinates on the screen */
uint32_t video_get_pixel(uint32_t x, uint32_t y)
{
    return (buffer)[y * width + x];
}

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_invoke_area(position p0, position p1, void (*callback)(position p))
{
    position p;
    for (p.y = p0.y; y <= p1.y; p.y++) {
        for (p.x = p0.x; x <= p1.x; p.x++) callback(p);
    }
}

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(position p0, position p1, int color)
{
    uint32_t x0 = p0.x;
    uint32_t y0 = p0.y;
    uint32_t x1 = p1.x;
    uint32_t y1 = p1.y;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) video_draw_pixel(x, y, color);
    }
}

/* Draw a character at the specified coordinates on the screen */
void video_draw_char(const char c, uint32_t x, uint32_t y, int color)
{
    uint8_t *font = ascfont;
    font += (size_t)c * 16;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 9; j++) {
            if (font[i] & (0x80 >> j)) {
                video_draw_pixel(x + j, y + i, color);
            } else {
                video_draw_pixel(x + j, y + i, back_color);
            }
        }
    }
}

/* Print a character at the specified coordinates on the screen */
void video_put_char(const char c, int color)
{
    uint32_t x;
    uint32_t y;
    if (c == '\n') {
        video_scroll();
        cx = 0;
        cy++;
        return;
    } else if (c == '\r') {
        cx = 0;
        return;
    } else if (c == '\t') {
        for (int i = 0; i < 8; i++) {
            // Expand by video_put_char(' ', color)
            video_scroll();
            x = (cx - 1) * 9;
            y = cy * 16;
            video_draw_char(c, x, y, color);
        }
        return;
    } else if (c == '\b' && cx > 0) { // Do not fill, just move cursor
        if ((long long)cx - 1 < 0) {
            cx = c_width - 1;
            if (cy != 0) cy -= 1;
            if (cy == 0) cx = 0, cy = 0;
        } else {
            cx--;
        }
        return;
    }
    video_scroll();
    x = (cx - 1) * 9;
    y = cy * 16;
    video_draw_char(c, x, y, color);
}

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str)
{
    for (; *str; ++str) video_put_char(*str, (int)fore_color);
}

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, int color)
{
    for (; *str; ++str) video_put_char(*str, color);
}
