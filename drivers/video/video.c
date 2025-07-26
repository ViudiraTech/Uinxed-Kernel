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
#include "uinxed.h"

extern uint8_t ascii_font[]; // Fonts

uint64_t  width;  // Screen width
uint64_t  height; // Screen height
uint64_t  stride; // Frame buffer line spacing
uint32_t *buffer; // Video Memory (We think BPP is 32. Tf BPP is other value, you have to change it)

uint32_t x, y;              // The current absolute cursor position
uint32_t cx, cy;            // The character position of the current cursor
uint32_t c_width, c_height; // Screen character width and height

uint32_t fore_color; // Foreground color
uint32_t back_color; // Background color

/* Get video information */
video_info_t video_get_info(void)
{
    video_info_t               info;
    struct limine_framebuffer *framebuffer = get_framebuffer();

    info.framebuffer = framebuffer->address;

    info.width      = framebuffer->width;
    info.height     = framebuffer->height;
    info.stride     = framebuffer->pitch / (framebuffer->bpp / 8);
    info.c_width    = info.width / 9;
    info.c_height   = info.height / 16;
    info.cx         = cx;
    info.cy         = cy;
    info.fore_color = fore_color;
    info.back_color = back_color;

    info.bpp              = framebuffer->bpp;
    info.memory_model     = framebuffer->memory_model;
    info.red_mask_size    = framebuffer->red_mask_size;
    info.red_mask_shift   = framebuffer->red_mask_shift;
    info.green_mask_size  = framebuffer->green_mask_size;
    info.green_mask_shift = framebuffer->green_mask_shift;
    info.blue_mask_size   = framebuffer->blue_mask_size;
    info.blue_mask_shift  = framebuffer->blue_mask_shift;
    info.edid_size        = framebuffer->edid_size;
    info.edid             = framebuffer->edid;

    return info;
}

/* Convert color to fb_color */
uint32_t color_to_fb_color(color_t color)
{
    struct limine_framebuffer *fb = get_framebuffer();

    uint32_t fb_red   = COLOR_MASK(color.red, fb->red_mask_size, fb->red_mask_shift);       // Red
    uint32_t fb_green = COLOR_MASK(color.green, fb->green_mask_size, fb->green_mask_shift); // Blue
    uint32_t fb_blue  = COLOR_MASK(color.blue, fb->blue_mask_size, fb->blue_mask_shift);    // Green
    uint32_t fb_color = fb_red | fb_green | fb_blue;
    return fb_color;
}

/* Convert fb_color to color */
color_t fb_color_to_color(uint32_t fb_color)
{
    struct limine_framebuffer *fb = get_framebuffer();

    uint32_t red   = COLOR_UNMASK(fb_color, fb->red_mask_size, fb->red_mask_shift);     // Red
    uint32_t green = COLOR_UNMASK(fb_color, fb->green_mask_size, fb->green_mask_shift); // Blue
    uint32_t blue  = COLOR_UNMASK(fb_color, fb->blue_mask_size, fb->blue_mask_shift);   // Green
    return (color_t) {red, green, blue};
}

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void)
{
    return framebuffer_request.response->framebuffers[0];
}

/* Initialize Video */
void video_init(void)
{
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count < 1) krn_halt();
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    buffer                                 = framebuffer->address;
    width                                  = framebuffer->width;
    height                                 = framebuffer->height;
    stride                                 = framebuffer->pitch / (framebuffer->bpp / 8);

    x = cx = y = cy = 0;
    c_width         = width / 9;
    c_height        = height / 16;

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    video_clear();
}

/* Clear screen */
void video_clear(void)
{
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    x  = 2;
    y  = 0;
    cx = cy = 0;
}

/* Clear screen with color */
void video_clear_color(uint32_t color)
{
    back_color = color;
    for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    x  = 2;
    y  = 0;
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
    uint8_t       *dest;
    const uint8_t *src;
    size_t         count;
    if ((uint32_t)cy >= c_height) {
        dest  = (uint8_t *)buffer;
        src   = (const uint8_t *)(buffer + stride * 16);
        count = (stride * (height - 16) * sizeof(uint32_t)) / 8;
        __asm__ volatile("rep movsq" : "+D"(dest), "+S"(src), "+c"(count)::"memory");

        /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
        /* Fill new line to back color */
        video_draw_rect((position_t) {0, height - 16}, (position_t) {stride, height}, back_color);
        cy = c_height - 1;
    }
}

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    (buffer)[y * stride + x] = color;
}

/* Get a pixel at the specified coordinates on the screen */
uint32_t video_get_pixel(uint32_t x, uint32_t y)
{
    return (buffer)[y * stride + x];
}

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_invoke_area(position_t p0, position_t p1, void (*callback)(position_t p))
{
    position_t p;
    for (p.y = p0.y; p.y <= p1.y; p.y++) {
        for (p.x = p0.x; p.x <= p1.x; p.x++) callback(p);
    }
}

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(position_t p0, position_t p1, uint32_t color)
{
    uint32_t x0 = p0.x;
    uint32_t y0 = p0.y;
    uint32_t x1 = p1.x;
    uint32_t y1 = p1.y;
    for (y = y0; y <= y1; y++) {
        /* Draw horizontal line */
#if defined(__x86_64__) || defined(__i386__)
        uint32_t *line  = buffer + y * stride + x0;
        size_t    count = x1 - x0 + 1;
        __asm__ volatile("rep stosl" : "+D"(line), "+c"(count) : "a"(color) : "memory");
#else
        for (uint32_t x = x0; x <= x1; x++) video_draw_pixel(x, y, color);
#endif
    }
}

/* Draw a character at the specified coordinates on the screen */
void video_draw_char(const char c, uint32_t x, uint32_t y, uint32_t color)
{
    uint8_t *font = ascii_font;
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
void video_put_char(const char c, uint32_t color)
{
    uint32_t x;
    uint32_t y;
    if (c == '\n') {
        cy++;
        cx = 0;
        /* Try scroll (but it will do when next character is printed)
         * video_scroll();
         * cx = 0;
         */
        return;
    } else if (c == '\r') {
        cx = 0;
        return;
    } else if (c == '\t') {
        for (int i = 0; i < 8; i++) {
            /* Expand by video_put_char(' ', color) */
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
    for (; *str; ++str) video_put_char(*str, fore_color);
}

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, uint32_t color)
{
    for (; *str; ++str) video_put_char(*str, color);
}
