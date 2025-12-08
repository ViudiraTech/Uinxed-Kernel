/*
 *
 *      video.c
 *      Basic video
 *
 *      2024/9/16 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <cpuid.h>
#include <gfx_proc.h>
#include <limine.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <uinxed.h>
#include <video.h>

#if TTF_CONSOLE
#    include <gfx_truetype.h> // TTF fonts
#else
extern uint8_t ascii_font[]; // Bitmap fonts
#endif

uint64_t  width;  // Screen width
uint64_t  height; // Screen height
uint64_t  stride; // Frame buffer line spacing
uint32_t *buffer; // Video Memory (We think BPP is 32. If BPP is other value, you have to change it)

uint32_t x, y;              // The current absolute cursor position
uint32_t cx, cy;            // The character position of the current cursor
uint32_t c_width, c_height; // Screen character width and height

uint32_t fore_color; // Foreground color
uint32_t back_color; // Background color

uint32_t font_width;  // Font width
uint32_t font_height; // Font height

static char     char_buffer[256];      // Char buffer
static uint32_t char_buffer_index = 0; // Char buffer index

/* Get video information */
video_info_t video_get_info(void)
{
    video_info_t               info;
    struct limine_framebuffer *framebuffer = get_framebuffer();

    info.framebuffer = framebuffer->address;

    info.width      = framebuffer->width;
    info.height     = framebuffer->height;
    info.stride     = framebuffer->pitch / (framebuffer->bpp / 8);
    info.c_width    = info.width / font_width;
    info.c_height   = info.height / font_height;
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

#if TTF_CONSOLE
    get_ttf_dimensions("A", CONSOLE_FONT_SIZE, &font_width, &font_height);
#else
    font_width  = 9;
    font_height = 16;
#endif

    x = cx = y = cy = 0;
    c_width         = width / font_width;
    c_height        = height / font_height;

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});

    char_buffer_index = 0;
    char_buffer[0]    = '\0';

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

    char_buffer_index = 0;
    char_buffer[0]    = '\0';
}

/* Clear screen with color */
void video_clear_color(uint32_t color)
{
    back_color = color;
    for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    x  = 2;
    y  = 0;
    cx = cy = 0;

    char_buffer_index = 0;
    char_buffer[0]    = '\0';
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
    for (p.y = p0.y; p.y <= p1.y; p.y++)
        for (p.x = p0.x; p.x <= p1.x; p.x++) callback(p);
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
#if TTF_CONSOLE
    draw_ttf((char[]) {c, '\0'}, x, y, CONSOLE_FONT_SIZE, color);
#else
    uint8_t *char_font      = ascii_font + (size_t)c * font_height;
    uint32_t char_base_addr = y * stride + x;

    for (uint32_t row = 0; row < font_height; row++) {
        uint32_t *row_buf  = buffer + char_base_addr + row * stride;
        uint8_t   font_row = char_font[row];
        for (uint32_t col = 0; col < font_width; col++) row_buf[col] = (font_row & (0x80 >> col)) ? color : back_color;
    }
#endif
}

/* Flush character buffer to screen */
void video_flush_buffer(uint32_t color)
{
#if TTF_CONSOLE
    if (char_buffer_index > 0) {
        char_buffer[char_buffer_index] = '\0';

        draw_ttf(char_buffer, cx * font_width, cy * font_height, CONSOLE_FONT_SIZE, color);
        cx += char_buffer_index;

        char_buffer_index = 0;
        char_buffer[0]    = '\0';
    }
#else
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
#endif
}

/* Print a character at the specified coordinates on the screen */
void video_put_char(const char c, uint32_t color)
{
    if (c == '\n') {
        video_flush_buffer(color);
        cy++;
        cx = 0;
        return;
    }
    if (c == '\r') {
        video_flush_buffer(color);
        cx = 0;
        return;
    }
    if (c == '\t') {
        video_flush_buffer(color);
        cx = (cx + 8) & ~7;
        if (cx >= c_width) {
            cx = 0;
            cy++;
        }
        return;
    }
    if (c == '\b') {
        video_flush_buffer(color);
        if (cx > 0) {
            cx--;
#if TTF_CONSOLE
            draw_ttf(" ", cx * font_width, cy * font_height, CONSOLE_FONT_SIZE, back_color);
#else
            video_draw_char(' ', cx * font_width, cy * font_height, back_color);
#endif
        } else if (cy > 0) {
            cy--;
            cx = c_width - 1;
#if TTF_CONSOLE
            draw_ttf(" ", cx * font_width, cy * font_height, CONSOLE_FONT_SIZE, back_color);
#else
            video_draw_char(' ', cx * font_width, cy * font_height, back_color);
#endif
        }
        return;
    }
    if (char_buffer_index < 256 - 1) char_buffer[char_buffer_index++] = c;
    if (char_buffer_index >= 256 - 1 || cx + char_buffer_index >= c_width) video_flush_buffer(color);
    if (cx >= c_width) {
        cx = 0;
        cy++;
    }
    if (cy >= c_height) video_scroll();
}

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str)
{
    for (; *str; ++str) video_put_char(*str, fore_color);
    video_flush_buffer(fore_color);
}

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, uint32_t color)
{
    for (; *str; ++str) video_put_char(*str, color);
    video_flush_buffer(color);
}
