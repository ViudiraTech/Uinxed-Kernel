/*
 *
 *		video.h
 *		Basic Video
 *
 *		2024/9/16 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_VIDEO_H_
#define INCLUDE_VIDEO_H_

#include "limine.h"
#include "stdint.h"

/* Initialize Video */
void video_init(void);

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void);

/* Clear screen */
void video_clear(void);

/* Clear screen with color */
void video_clear_color(int color);

/* Screen scrolling operation */
void video_scroll(void);

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color);

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(int x0, int y0, int x1, int y1, int color);

/* Draw a character at the specified coordinates on the screen */
void video_draw_char(char c, int32_t x, int32_t y, int color);

/* Print a character at the specified coordinates on the screen */
void video_put_char(char c, int color);

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str);

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, int color);

#endif // INCLUDE_VIDEO_H_
