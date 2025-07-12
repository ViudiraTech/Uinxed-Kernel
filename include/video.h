/*
 *
 *      video.h
 *      Basic Video
 *
 *      2024/9/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_VIDEO_H_
#define INCLUDE_VIDEO_H_

#include "limine.h"
#include "stdint.h"

typedef struct position {
        uint32_t x;
        uint32_t y;
} position;

typedef struct VideoInfo {
        uint32_t cx, cy;            // The character position of the current cursor
        uint32_t c_width, c_height; // Screen character width and height
        uint64_t width;             // Screen length
        uint64_t height;            // Screen width
        uint64_t stride;            // Frame buffer line spacing
        uint32_t fore_color;        // Foreground color
        uint32_t back_color;        // Background color
} VideoInfo;

/* Get video information */
VideoInfo video_get_info(void);

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void);

/* Initialize Video */
void video_init(void);

/* Clear screen */
void video_clear(void);

/* Clear screen with color */
void video_clear_color(uint32_t color);

/* Scroll to a position that units are characters */
void video_move_to(uint32_t cx, uint32_t cy);

/* Screen scrolling operation */
void video_scroll(void);

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color);

/* Get a pixel at the specified coordinates on the screen */
uint32_t video_get_pixel(uint32_t x, uint32_t y);

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_invoke_area(position p0, position p1, void (*callback)(position p));

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(position p0, position p1, uint32_t color);

/* Draw a character at the specified coordinates on the screen */
void video_draw_char(const char c, uint32_t x, uint32_t y, uint32_t color);

/* Print a character at the specified coordinates on the screen */
void video_put_char(const char c, uint32_t color);

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str);

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, uint32_t color);

#endif // INCLUDE_VIDEO_H_
