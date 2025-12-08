/*
 *
 *      gfx_truetype.h
 *      TTF graphics processing header files
 *
 *      2025/10/7 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_GFX_TRUETYPE_H_
#define INCLUDE_GFX_TRUETYPE_H_

#include <stdint.h>

/* Get the length and width of the corresponding word according to the specified font and size */
void get_ttf_dimensions(const char *data, uint32_t size, uint32_t *width, uint32_t *height);

/* Draw ttf font graphics at the specified position */
void draw_ttf(const char *data, uint32_t x, uint32_t y, uint32_t size, uint32_t color);

/* Initialize ttf font data */
void init_ttf(uint8_t *ttf_buffer);

#endif // INCLUDE_GFX_TRUETYPE_H_
