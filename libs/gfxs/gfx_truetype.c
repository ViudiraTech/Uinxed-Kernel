/*
 *
 *      gfx_truetype.c
 *      TTF graphics processing
 *
 *      2025/10/7 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "stdint.h"
#include "string.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define NDEBUG

#include "alloc.h"
#include "heap.h"
#include "limine_module.h"
#include "math.h"
#include "stb_truetype.h"
#include "utflib.h"
#include "video.h"

stbtt_fontinfo font;

/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */

/* ttf grayscale mixing */
static uint32_t tty_blend(uint32_t background, uint32_t foreground, uint8_t alpha)
{
    uint8_t bg_r = (background >> 16) & 0xff;
    uint8_t bg_g = (background >> 8) & 0xff;
    uint8_t bg_b = background & 0xff;

    uint8_t fg_r = (foreground >> 16) & 0xff;
    uint8_t fg_g = (foreground >> 8) & 0xff;
    uint8_t fg_b = foreground & 0xff;

    uint8_t blended_r = (uint8_t)((fg_r * alpha + bg_r * (255 - alpha)) / 255);
    uint8_t blended_g = (uint8_t)((fg_g * alpha + bg_g * (255 - alpha)) / 255);
    uint8_t blended_b = (uint8_t)((fg_b * alpha + bg_b * (255 - alpha)) / 255);

    return (blended_r << 16) | (blended_g << 8) | blended_b;
}

/* Parse ttf data into bitmap */
static uint8_t *ttf_analysis(int *buf, uint32_t *width, uint32_t *height, int size)
{
    if (size <= 0 || !buf || !buf[0]) {
        *width  = 0;
        *height = 0;
        return 0;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, (float)size * 2.0f);
    int   ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    ascent  = (int)roundf((float)ascent * scale);
    descent = (int)roundf((float)descent * scale);
    *height = (uint32_t)roundf((float)(ascent - descent) + (float)lineGap * scale);

    int total_width = 0;
    for (int i = 0; buf[i]; ++i) {
        int advanceWidth, leftSideBearing;
        stbtt_GetCodepointHMetrics(&font, buf[i], &advanceWidth, &leftSideBearing);
        total_width += (int)roundf((float)advanceWidth * scale);

        if (buf[i + 1]) {
            int kern = stbtt_GetCodepointKernAdvance(&font, buf[i], buf[i + 1]);
            total_width += (int)roundf((float)kern * scale);
        }
    }
    *width = (uint32_t)total_width;

    if (!*width || !*height) {
        *width  = 0;
        *height = 0;
        return 0;
    }

    size_t bitmap_size = (size_t)(*width) * (size_t)(*height);
    if (!bitmap_size) return 0;

    uint8_t *bitmap = (uint8_t *)calloc(1, bitmap_size);
    if (!bitmap) return 0;

    int x = 0;
    for (int i = 0; buf[i]; ++i) {
        int advanceWidth, leftSideBearing;
        stbtt_GetCodepointHMetrics(&font, buf[i], &advanceWidth, &leftSideBearing);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&font, buf[i], scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int y          = ascent + c_y1;
        int byteOffset = x + (int)((float)leftSideBearing * scale) + (y * (int)(*width));

        if (byteOffset >= 0 && byteOffset < (int)bitmap_size)
            stbtt_MakeCodepointBitmap(&font, bitmap + byteOffset, c_x2 - c_x1, c_y2 - c_y1, (int)(*width), scale, scale, buf[i]);

        x += (int)roundf((float)advanceWidth * scale);
        if (buf[i + 1]) {
            int kern = stbtt_GetCodepointKernAdvance(&font, buf[i], buf[i + 1]);
            x += (int)roundf((float)kern * scale);
        }
    }
    return bitmap;
}

/* Draw ttf bitmap with antialiasing */
static void draw_bitmap(uint8_t *bitmap, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t bitmap_xsize, uint32_t color)
{
    if (!bitmap || !width || !height) return;
    video_info_t fb = video_get_info();

    if (x >= fb.width || y >= fb.height) return;
    uint32_t max_x = (x + width < fb.width) ? width : fb.width - x;
    uint32_t max_y = (y + height < fb.height) ? height : fb.height - y;

    for (uint32_t j = 0; j < max_y; j++) {
        for (uint32_t i = 0; i < max_x; i++) {
            uint8_t alpha = bitmap[j * bitmap_xsize + i];
            if (alpha > 0) {
                uint32_t fb_index = (y + j) * fb.stride + (x + i);
                if (fb_index < fb.stride * fb.height) fb.framebuffer[fb_index] = tty_blend(fb.framebuffer[fb_index], color, alpha);
            }
        }
    }
}

/* Get the length and width of the corresponding word according to the specified font and size */
void get_ttf_dimensions(const char *data, uint32_t size, uint32_t *width, uint32_t *height)
{
    if (!data || !*data || !size) {
        *width  = 0;
        *height = 0;
        return;
    }

    int str_len = (int)utflen(data);
    if (str_len <= 0) {
        *width  = 0;
        *height = 0;
        return;
    }

    Rune *r = (Rune *)malloc(sizeof(Rune) * (str_len + 1));
    if (!r) {
        *width  = 0;
        *height = 0;
        return;
    }

    int   i         = 0;
    char *temp_data = (char *)data;
    while (*temp_data != '\0' && i < str_len) temp_data += chartorune(&(r[i++]), temp_data);
    r[str_len] = 0;

    float scale = stbtt_ScaleForPixelHeight(&font, (float)size * 2.0f);
    int   ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    ascent  = (int)roundf((float)ascent * scale);
    descent = (int)roundf((float)descent * scale);
    *height = (uint32_t)roundf((float)(ascent - descent) + (float)lineGap * scale);

    int x = 0;
    for (int i = 0; r[i]; ++i) {
        int advanceWidth, leftSideBearing;
        stbtt_GetCodepointHMetrics(&font, r[i], &advanceWidth, &leftSideBearing);

        x += (int)roundf((float)advanceWidth * scale);

        if (r[i + 1]) {
            int kern = stbtt_GetCodepointKernAdvance(&font, r[i], r[i + 1]);
            x += (int)roundf((float)kern * scale);
        }
    }
    *width = (uint32_t)x;
    free(r);
}

/* Draw ttf font graphics at the specified position */
void draw_ttf(const char *data, uint32_t x, uint32_t y, uint32_t size, uint32_t color)
{
    if (!data || !*data || !size) return;
    int str_len = (int)utflen(data);

    if (str_len <= 0) return;
    Rune *r = (Rune *)malloc(sizeof(Rune) * (str_len + 1));

    if (!r) return;
    int   i         = 0;
    char *temp_data = (char *)data;

    while (*temp_data != '\0' && i < str_len) temp_data += chartorune(&(r[i++]), temp_data);
    r[str_len] = 0;

    uint32_t width, height;
    uint8_t *bitmap = ttf_analysis(r, &width, &height, (int)size);

    free(r);

    if (bitmap) {
        draw_bitmap(bitmap, x, y, width, height, width, color);
        free(bitmap);
    }
}

/* Initialize ttf font data */
void init_ttf(uint8_t *ttf_buffer)
{
    if (ttf_buffer) stbtt_InitFont(&font, ttf_buffer, 0);
}

/* NOLINTEND(bugprone-easily-swappable-parameters) */
