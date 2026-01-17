/*
 *
 *      gfx_truetype.c
 *      TTF graphics processing
 *
 *      2025/10/7 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <heap.h>
#include <limine_module.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <utflib.h>
#include <video.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define NDEBUG

#include <stb_truetype.h>

stbtt_fontinfo font;

/* ttf grayscale mixing */
static uint32_t tty_blend(uint32_t background, uint32_t foreground, uint8_t alpha)
{
    /* Precompute common values */
    uint32_t inv_alpha = 255 - alpha;

    /* Extract and blend channels in one pass */
    uint32_t bg_rb = background & 0xff00ff;
    uint32_t bg_g  = background & 0x00ff00;

    uint32_t fg_rb = foreground & 0xff00ff;
    uint32_t fg_g  = foreground & 0x00ff00;

    /* Blend using integer math - avoid divisions */
    uint32_t blended_rb = ((fg_rb * alpha + bg_rb * inv_alpha) >> 8) & 0xff00ff;
    uint32_t blended_g  = ((fg_g * alpha + bg_g * inv_alpha) >> 8) & 0x00ff00;

    return blended_rb | blended_g;
}

/* Parse ttf data into bitmap */
static uint8_t *ttf_analysis(int *buf, uint32_t *width, uint32_t *height, int size)
{
    if (size <= 0 || !buf || !buf[0]) {
        *width  = 0;
        *height = 0;
        return 0;
    }

    /* Precompute scale once */
    float scale = stbtt_ScaleForPixelHeight(&font, (float)size * 2.0f);
    int   ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    /* Precompute scaled metrics */
    int   scaled_ascent  = (int)roundf((float)ascent * scale);
    int   scaled_descent = (int)roundf((float)descent * scale);
    float scaled_lineGap = (float)lineGap * scale;

    *height = (uint32_t)roundf((float)(scaled_ascent - scaled_descent) + scaled_lineGap);

    /* Precompute character metrics in single pass */
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

    /* Single pass rendering with precomputed positions */
    int x = 0;
    for (int i = 0; buf[i]; ++i) {
        int advanceWidth, leftSideBearing;
        stbtt_GetCodepointHMetrics(&font, buf[i], &advanceWidth, &leftSideBearing);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&font, buf[i], scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int y          = scaled_ascent + c_y1;
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

/* Draw ttf bitmap with antialiasing - optimized inner loop */
static void draw_bitmap(uint8_t *bitmap, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t bitmap_xsize, uint32_t color)
{
    if (!bitmap || !width || !height) return;
    video_info_t fb = video_get_info();
    if (x >= fb.width || y >= fb.height) return;

    /* Precompute bounds */
    uint32_t max_x = (x + width < fb.width) ? width : fb.width - x;
    uint32_t max_y = (y + height < fb.height) ? height : fb.height - y;

    /* Cache framebuffer access */
    uint32_t *framebuffer = fb.framebuffer;
    uint32_t  stride      = fb.stride;

    /* Optimized pixel blending */
    for (uint32_t j = 0; j < max_y; j++) {
        uint32_t fb_row_start     = (y + j) * stride + x;
        uint32_t bitmap_row_start = j * bitmap_xsize;

        for (uint32_t i = 0; i < max_x; i++) {
            uint8_t alpha = bitmap[bitmap_row_start + i];
            if (alpha > 0) {
                uint32_t fb_index = fb_row_start + i;
                if (fb_index < stride * fb.height) framebuffer[fb_index] = tty_blend(framebuffer[fb_index], color, alpha);
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

    rune_t *r = (rune_t *)malloc(sizeof(rune_t) * (str_len + 1));
    if (!r) {
        *width  = 0;
        *height = 0;
        return;
    }

    /* Optimized string parsing */
    int         i         = 0;
    const char *temp_data = data;
    while (*temp_data != '\0' && i < str_len) {
        temp_data += chartorune(&r[i], temp_data);
        i++;
    }
    r[str_len] = 0;

    /* Precompute scale and metrics */
    float scale = stbtt_ScaleForPixelHeight(&font, (float)size * 2.0f);
    int   ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    int scaled_ascent  = (int)roundf((float)ascent * scale);
    int scaled_descent = (int)roundf((float)descent * scale);
    *height            = (uint32_t)roundf((float)(scaled_ascent - scaled_descent) + (float)lineGap * scale);

    /* Single pass width calculation */
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

    rune_t *r = (rune_t *)malloc(sizeof(rune_t) * (str_len + 1));
    if (!r) return;

    /* Optimized string parsing */
    int         i         = 0;
    const char *temp_data = data;
    while (*temp_data != '\0' && i < str_len) {
        temp_data += chartorune(&r[i], temp_data);
        i++;
    }
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
