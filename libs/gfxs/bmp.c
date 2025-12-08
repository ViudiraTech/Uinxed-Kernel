/*
 *
 *      bmp.c
 *      Bitmap image
 *
 *      2025/6/22 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <bmp.h>
#include <video.h>

/* Parse bitmap images and draw them to the screen */
void bmp_analysis(bmp_t *bmp, uint32_t offset_x, uint32_t offset_y, int enable_transparency)
{
    if (bmp->magic != 0x4d42 || bmp->bits_per_pixel != 24) return;

    uint8_t *data      = (uint8_t *)bmp + bmp->bmp_data_offset;
    uint32_t row_bytes = (bmp->frame_width * 3 + 3) & ~3;

    for (uint32_t y_offset = 0; y_offset < bmp->frame_height; ++y_offset) {
        for (uint32_t x_offset = 0; x_offset < bmp->frame_width; ++x_offset) {
            uint32_t pixel_offset = y_offset * row_bytes + x_offset * 3;
            uint8_t  b            = data[pixel_offset + 0];
            uint8_t  g            = data[pixel_offset + 1];
            uint8_t  r            = data[pixel_offset + 2];
            uint32_t color        = (r << 16) | (g << 8) | b;

            if (enable_transparency && color == 0) continue;
            video_draw_pixel(offset_x + x_offset, offset_y + bmp->frame_height - 1 - y_offset, color);
        }
    }
}
