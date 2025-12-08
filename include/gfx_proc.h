/*
 *
 *      gfx_proc.h
 *      Graphics processing header file
 *
 *      2025/7/28 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_GFX_PROC_H_
#define INCLUDE_GFX_PROC_H_

#include <video.h>

#define COLOR_SIZE_MASK(size)               ((1 << (size)) - 1)
#define COLOR_MASK(color, size, shift)      (((color) & COLOR_SIZE_MASK(size)) << (shift))
#define COLOR_UNMASK(fb_color, size, shift) (((fb_color) >> (shift)) & COLOR_SIZE_MASK(size))

/* Convert color to fb_color */
uint32_t color_to_fb_color(color_t color);

/* Convert fb_color to color */
color_t fb_color_to_color(uint32_t fb_color);

#endif // INCLUDE_GFX_PROC_H_
