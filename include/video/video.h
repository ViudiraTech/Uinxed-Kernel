/*
 *
 *      video.h
 *      Basic video
 *
 *      2024/9/16 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIDEO_H_
#define INCLUDE_VIDEO_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

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

/* Shared variables for video subsystem */
extern uint64_t  width;  // Screen width
extern uint64_t  height; // Screen height
extern uint64_t  stride; // Frame buffer line spacing
extern uint32_t *buffer; // Video Memory

extern uint32_t cx, cy;            // The character position of the current cursor
extern uint32_t c_width, c_height; // Screen character width and height

extern uint32_t fore_color; // Foreground color
extern uint32_t back_color; // Background color

extern uint32_t font_width;  // Font width
extern uint32_t font_height; // Font height

/* Get video information */
video_info_t video_get_info(void);

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void);

/*
 * Read raw bytes from the primary framebuffer backing /dev/fb0.
 *
 * `offset` and `size` are byte-based. Data is copied from the live framebuffer
 * memory exactly as stored in RAM.
 */
size_t video_fb_read(void *ctx, void *addr, size_t offset, size_t size);

/*
 * Write raw bytes to the primary framebuffer backing /dev/fb0.
 *
 * `offset` and `size` are byte-based. Writes immediately affect the visible
 * framebuffer contents.
 */
size_t video_fb_write(void *ctx, const void *addr, size_t offset, size_t size);

/* Query /dev/fb0 metadata such as dimensions, stride and pixel layout. */
int video_fb_ioctl(void *ctx, size_t req, void *arg);

/* Initialize Video */
void video_init(void);

/* Flush callback type — called after framebuffer writes to push to host */
typedef void (*video_flush_fn_t)(void);

/*
 * Switch the console framebuffer to a DRM-backed buffer.
 * After this call all fbcon output (printk, tty) renders into @backing
 * and @flush is called after each batch draw to push pixels to the host.
 */
void video_switch_to_drm(void *backing, uint32_t w, uint32_t h, uint32_t pitch, video_flush_fn_t flush);

/* Clear screen */
void video_clear(void);

/* Clear screen with color */
void video_clear_color(uint32_t color);

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color);

/* Get a pixel at the specified coordinates on the screen */
uint32_t video_get_pixel(uint32_t x, uint32_t y);

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_invoke_area(position_t p0, position_t p1, void (*callback)(position_t p));

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(position_t p0, position_t p1, uint32_t color);

#endif // INCLUDE_VIDEO_H_
