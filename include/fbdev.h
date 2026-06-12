/*
 *
 *      fbdev.h
 *      Framebuffer device interface
 *
 *      2026/6/12 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FBDEV_H_
#define INCLUDE_FBDEV_H_

#include <stdint.h>

/* Return a `fbdev_info_t` describing /dev/fb0. */
#define FBDEV_IOCTL_GET_INFO 0x1000

/*
 * Userspace-visible framebuffer metadata returned by `FBDEV_IOCTL_GET_INFO`.
 *
 * `stride` is expressed in pixels per scanline.
 * `size` is the total framebuffer size in bytes.
 * Raw reads and writes to /dev/fb0 use packed framebuffer bytes starting at
 * the first pixel of the top-left corner.
 */
typedef struct {
        uint64_t width;
        uint64_t height;
        uint64_t stride;
        uint16_t bpp;
        uint64_t size;
        uint8_t  red_mask_size;
        uint8_t  red_mask_shift;
        uint8_t  green_mask_size;
        uint8_t  green_mask_shift;
        uint8_t  blue_mask_size;
        uint8_t  blue_mask_shift;
} fbdev_info_t;

#endif // INCLUDE_FBDEV_H_
