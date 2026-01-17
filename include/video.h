/*
 *
 *      video.h
 *      Basic video
 *
 *      2024/9/16 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIDEO_H_
#define INCLUDE_VIDEO_H_

#include <stdint.h>

#define TTF_CONSOLE       1
#define CONSOLE_FONT_SIZE 10

typedef struct {
        uint8_t red;
        uint8_t green;
        uint8_t blue;
} color_t;

typedef struct {
        uint32_t x;
        uint32_t y;
} position_t;

typedef struct {
        uint32_t *framebuffer;       // Frame buffer
        uint32_t  cx, cy;            // The character position of the current cursor
        uint32_t  c_width, c_height; // Screen character width and height
        uint64_t  width;             // Screen length
        uint64_t  height;            // Screen width
        uint64_t  stride;            // Frame buffer line spacing
        uint32_t  fore_color;        // Foreground color
        uint32_t  back_color;        // Background color
        uint16_t  bpp;               // Bits per pixel
        uint8_t   memory_model;      // Display memory model
        uint8_t   red_mask_size;     // Red mask size
        uint8_t   red_mask_shift;    // Red mask displacement
        uint8_t   green_mask_size;   // Green mask size
        uint8_t   green_mask_shift;  // Green mask displacement
        uint8_t   blue_mask_size;    // Blue mask size
        uint8_t   blue_mask_shift;   // Blue mask displacement
        uint64_t  edid_size;         // EDID data size
        void     *edid;              // EDID data pointer
} video_info_t;

/* Get video information */
video_info_t video_get_info(void);

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
void video_invoke_area(position_t p0, position_t p1, void (*callback)(position_t p));

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(position_t p0, position_t p1, uint32_t color);

/* Flush character buffer to screen */
void video_flush_buffer(uint32_t color);

/* Print a character at the specified coordinates on the screen */
void video_put_char(const char c, uint32_t color);

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str);

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, uint32_t color);

#endif // INCLUDE_VIDEO_H_
