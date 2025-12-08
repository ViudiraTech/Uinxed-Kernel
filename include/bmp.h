/*
 *
 *      bmp.h
 *      Bitmap image header file
 *
 *      2025/6/22 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BMP_H_
#define INCLUDE_BMP_H_

#include <stdint.h>

typedef struct {
        uint16_t magic;
        uint32_t file_size;
        uint32_t reserved;
        uint32_t bmp_data_offset;
        uint32_t bmp_info_size;
        uint32_t frame_width;
        uint32_t frame_height;
        uint16_t reserved_value;
        uint16_t bits_per_pixel;
        uint32_t compression_mode;
        uint32_t frame_size;
        uint32_t horizontal_resolution;
        uint32_t vertical_resolution;
        uint32_t used_color_count;
        uint32_t important_color_count;
} __attribute__((packed)) bmp_t;

/* Parse bitmap images and draw them to the screen */
void bmp_analysis(bmp_t *bmp, uint32_t offset_x, uint32_t offset_y, int enable_transparency);

#endif // INCLUDE_BMP_H_
