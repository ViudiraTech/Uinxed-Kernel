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

#include <libs/std/stdint.h>

/* Return a `fbdev_info_t` describing /dev/fb0. */
#define FBDEV_IOCTL_GET_INFO 0x1000

/*
 * Linux framebuffer userspace ABI.
 *
 * Keep these layouts in sync with include/uapi/linux/fb.h.  Xorg passes the
 * structures below directly through ioctl(2), so this is an ABI contract,
 * not an internal convenience structure.
 */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP          0x4604
#define FBIOPUTCMAP          0x4605
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2
#define FB_ACCEL_NONE         0

#define FB_ACTIVATE_NOW  0
#define FB_ACTIVATE_TEST 2
#define FB_ACTIVATE_MASK 15

#define FB_SYNC_HOR_HIGH_ACT  1
#define FB_SYNC_VERT_HIGH_ACT 2

#define FB_VMODE_NONINTERLACED 0
#define FB_VMODE_MASK          255

#define FB_BLANK_UNBLANK       0
#define FB_BLANK_NORMAL        1
#define FB_BLANK_VSYNC_SUSPEND 2
#define FB_BLANK_HSYNC_SUSPEND 3
#define FB_BLANK_POWERDOWN     4

typedef struct {
        uint32_t offset;
        uint32_t length;
        uint32_t msb_right;
} fbdev_bitfield_t;

typedef struct {
        char     id[16];
        uint64_t smem_start;
        uint32_t smem_len;
        uint32_t type;
        uint32_t type_aux;
        uint32_t visual;
        uint16_t xpanstep;
        uint16_t ypanstep;
        uint16_t ywrapstep;
        uint32_t line_length;
        uint64_t mmio_start;
        uint32_t mmio_len;
        uint32_t accel;
        uint16_t capabilities;
        uint16_t reserved[2];
} fbdev_fix_screeninfo_t;

typedef struct {
        uint32_t         xres;
        uint32_t         yres;
        uint32_t         xres_virtual;
        uint32_t         yres_virtual;
        uint32_t         xoffset;
        uint32_t         yoffset;
        uint32_t         bits_per_pixel;
        uint32_t         grayscale;
        fbdev_bitfield_t red;
        fbdev_bitfield_t green;
        fbdev_bitfield_t blue;
        fbdev_bitfield_t transp;
        uint32_t         nonstd;
        uint32_t         activate;
        uint32_t         height;
        uint32_t         width;
        uint32_t         accel_flags;
        uint32_t         pixclock;
        uint32_t         left_margin;
        uint32_t         right_margin;
        uint32_t         upper_margin;
        uint32_t         lower_margin;
        uint32_t         hsync_len;
        uint32_t         vsync_len;
        uint32_t         sync;
        uint32_t         vmode;
        uint32_t         rotate;
        uint32_t         colorspace;
        uint32_t         reserved[4];
} fbdev_var_screeninfo_t;

typedef struct {
        uint32_t  start;
        uint32_t  len;
        uint16_t *red;
        uint16_t *green;
        uint16_t *blue;
        uint16_t *transp;
} fbdev_cmap_t;

_Static_assert(sizeof(fbdev_bitfield_t) == 12, "Linux fb_bitfield ABI mismatch");
_Static_assert(sizeof(fbdev_fix_screeninfo_t) == 80, "Linux fb_fix_screeninfo ABI mismatch");
_Static_assert(sizeof(fbdev_var_screeninfo_t) == 160, "Linux fb_var_screeninfo ABI mismatch");
_Static_assert(sizeof(fbdev_cmap_t) == 40, "Linux fb_cmap ABI mismatch");

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
