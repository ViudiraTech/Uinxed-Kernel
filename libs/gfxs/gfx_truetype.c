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
    float scale = stbtt_ScaleForPixelHeight(&font, (float)size * 2.0f);
    int   ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    ascent  = (int)roundf((float)ascent * scale);
    descent = (int)roundf((float)descent * scale);
    *height = (uint32_t)roundf((float)(ascent - descent) + (float)lineGap * scale);

    /* Calculate total width first */
    int total_width = 0;
    for (int i = 0; buf[i] != 0; ++i) {
        int advanceWidth, leftSideBearing;
        stbtt_GetCodepointHMetrics(&font, buf[i], &advanceWidth, &leftSideBearing);
        total_width += (int)roundf((float)advanceWidth * scale);

        if (buf[i + 1] != 0) {
            int kern = stbtt_GetCodepointKernAdvance(&font, buf[i], buf[i + 1]);
            total_width += (int)roundf((float)kern * scale);
        }
    }
    *width = total_width;

    /* Dynamically allocate bitmap based on calculated dimensions */
    uint8_t *bitmap = (uint8_t *)calloc(1, (size_t)(*width) * (size_t)(*height));
    if (!bitmap) { return 0; }

    int x = 0;
    for (int i = 0; buf[i] != 0; ++i) {
        int advanceWidth, leftSideBearing;
        stbtt_GetCodepointHMetrics(&font, buf[i], &advanceWidth, &leftSideBearing);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&font, buf[i], scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int y          = ascent + c_y1;
        int byteOffset = x + (int)((float)leftSideBearing * scale) + (y * (int)(*width));
        stbtt_MakeCodepointBitmap(&font, bitmap + byteOffset, c_x2 - c_x1, c_y2 - c_y1, (int)(*width), scale, scale, buf[i]);

        x += (int)roundf((float)advanceWidth * scale);
        if (buf[i + 1] != 0) {
            int kern = stbtt_GetCodepointKernAdvance(&font, buf[i], buf[i + 1]);
            x += (int)roundf((float)kern * scale);
        }
    }
    return bitmap;
}

/* Draw ttf bitmap with antialiasing */
static void draw_bitmap(uint8_t *bitmap, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t bitmap_xsize, uint32_t color)
{
    video_info_t fb = video_get_info();

    for (int i = 0; i < (int)width; i++) {
        for (int j = 0; j < (int)height; j++) {
            uint8_t alpha = bitmap[j * bitmap_xsize + i];
            if (alpha > 0) { /* Only blend if pixel is not transparent */
                fb.framebuffer[(y + j) * fb.stride + (x + i)] = tty_blend(fb.framebuffer[(y + j) * fb.stride + (x + i)], color, alpha);
            }
        }
    }
}

/* Get the length and width of the corresponding word according to the specified font and size */
void get_ttf_dimensions(const char *data, uint32_t size, uint32_t *width, uint32_t *height)
{
    int  i = 0;
    Rune r[utflen(data) + 1];
    r[utflen(data)] = 0;
    char *temp_data = (char *)data;

    while (*temp_data != '\0') temp_data += chartorune(&(r[i++]), temp_data);

    float scale = stbtt_ScaleForPixelHeight(&font, (float)size * 2.0f);
    int   ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    ascent  = (int)roundf((float)ascent * scale);
    descent = (int)roundf((float)descent * scale);
    *height = (uint32_t)roundf((float)(ascent - descent) + (float)lineGap * scale);

    int x = 0;
    for (int i = 0; r[i] != 0; ++i) {
        int advanceWidth, leftSideBearing;
        stbtt_GetCodepointHMetrics(&font, r[i], &advanceWidth, &leftSideBearing);

        x += (int)roundf((float)advanceWidth * scale);

        if (r[i + 1] != 0) {
            int kern = stbtt_GetCodepointKernAdvance(&font, r[i], r[i + 1]);
            x += (int)roundf((float)kern * scale);
        }
    }
    *width = x;
}

/* Draw ttf font graphics at the specified position */
void draw_ttf(const char *data, uint32_t x, uint32_t y, uint32_t size, uint32_t color)
{
    int      i = 0;
    uint32_t width, height;

    Rune r[utflen(data) + 1];
    r[utflen(data)] = 0;

    char *temp_data = (char *)data;
    while (*temp_data != '\0') temp_data += chartorune(&(r[i++]), temp_data);

    uint8_t *bitmap = ttf_analysis(r, &width, &height, (int)size);
    if (bitmap) {
        draw_bitmap(bitmap, x, y, width, height, width, color);
        free(bitmap);
    }
}

/* Initialize ttf font data */
void init_ttf(uint8_t *ttf_buffer)
{
    stbtt_InitFont(&font, ttf_buffer, 0);
}

/* NOLINTEND(bugprone-easily-swappable-parameters) */
