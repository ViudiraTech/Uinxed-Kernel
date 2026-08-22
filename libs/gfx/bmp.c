/*
 *
 *      bmp.c
 *      Bitmap image
 *
 *      2025/6/22 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/fbdev/video.h>
#include <libs/gfx/bmp.h>

/* Parse bitmap images and draw them to the screen */
void bmp_analysis(bmp_t *bmp, uint32_t offset_x, uint32_t offset_y, int enable_transparency)
{
    if (bmp->magic != 0x4d42 || bmp->bits_per_pixel != 24) return;
    if (bmp->frame_width == 0 || bmp->frame_height == 0 || bmp->frame_width > 10000 || bmp->frame_height > 10000) return;
    if (bmp->bmp_data_offset >= bmp->file_size) return;
    uint32_t w3;

    if (__builtin_mul_overflow(bmp->frame_width, (uint32_t)3, &w3)) return;
    uint32_t row_bytes;

    if (__builtin_add_overflow(w3, (uint32_t)3, &row_bytes)) return;
    row_bytes &= ~3u;

    if (row_bytes == 0) return;
    uint64_t total;

    if (__builtin_mul_overflow((uint64_t)row_bytes, bmp->frame_height, &total)) return;
    if (bmp->bmp_data_offset + total > bmp->file_size) return;
    uint8_t *data = (uint8_t *)bmp + bmp->bmp_data_offset;

    for (uint32_t y_offset = 0; y_offset < bmp->frame_height; ++y_offset) {
        for (uint32_t x_offset = 0; x_offset < bmp->frame_width; ++x_offset) {
            uint32_t x3;
            if (__builtin_mul_overflow(x_offset, (uint32_t)3, &x3)) return;

            uint32_t pixel_offset;
            if (__builtin_mul_overflow(y_offset, row_bytes, &pixel_offset)) return;
            if (__builtin_add_overflow(pixel_offset, x3, &pixel_offset)) return;
            if (pixel_offset + 2 >= bmp->file_size) return;

            uint8_t  b     = data[pixel_offset + 0];
            uint8_t  g     = data[pixel_offset + 1];
            uint8_t  r     = data[pixel_offset + 2];
            uint32_t color = (r << 16) | (g << 8) | b;
            if (enable_transparency && color == 0) continue;

            video_draw_pixel(offset_x + x_offset, offset_y + bmp->frame_height - 1 - y_offset, color);
        }
    }
}
