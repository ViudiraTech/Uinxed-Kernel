/*
 *
 *      video.c
 *      Basic video
 *
 *      2024/9/16 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <chipset/common.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/gfxs/gfx_proc.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <proc/uaccess.h>
#include <video/fbcon.h>
#include <video/fbdev.h>
#include <video/klogo.h>
#include <video/video.h>

/* DRM-flush callback — set by video_switch_to_drm, invoked after draws */
static video_flush_fn_t video_flush_cb = NULL;

uint64_t  width;  // Screen width
uint64_t  height; // Screen height
uint64_t  stride; // Frame buffer line spacing
uint32_t *buffer; // Video Memory (We think BPP is 32. If BPP is other value, you have to change it)

uint32_t cx, cy;            // The character position of the current cursor
uint32_t c_width, c_height; // Screen character width and height

uint32_t fore_color; // Foreground color
uint32_t back_color; // Background color

uint32_t font_width;  // Font width
uint32_t font_height; // Font height

/* Get video information */
video_info_t video_get_info(void)
{
    video_info_t               info;
    struct limine_framebuffer *framebuffer = get_framebuffer();

    info.framebuffer = framebuffer->address;

    info.width      = framebuffer->width;
    info.height     = framebuffer->height;
    info.stride     = framebuffer->pitch / (framebuffer->bpp / 8);
    info.c_width    = info.width / font_width;
    info.c_height   = info.height / font_height;
    info.cx         = cx;
    info.cy         = cy;
    info.fore_color = fore_color;
    info.back_color = back_color;

    info.bpp              = framebuffer->bpp;
    info.memory_model     = framebuffer->memory_model;
    info.red_mask_size    = framebuffer->red_mask_size;
    info.red_mask_shift   = framebuffer->red_mask_shift;
    info.green_mask_size  = framebuffer->green_mask_size;
    info.green_mask_shift = framebuffer->green_mask_shift;
    info.blue_mask_size   = framebuffer->blue_mask_size;
    info.blue_mask_shift  = framebuffer->blue_mask_shift;
    info.edid_size        = framebuffer->edid_size;
    info.edid             = framebuffer->edid;

    return info;
}

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void)
{
    return framebuffer_request.response->framebuffers[0];
}

/* Read raw bytes from the primary framebuffer */
size_t video_fb_read(void *ctx, void *addr, size_t offset, size_t size)
{
    uint8_t *src;
    size_t   fb_size;

    (void)ctx;
    if (!addr) return 0;

    fb_size = (size_t)(stride * height * sizeof(uint32_t));
    if (offset >= fb_size) return 0;

    if (size > fb_size - offset) size = fb_size - offset;
    src = (uint8_t *)buffer + offset;
    memcpy(addr, src, size);
    return size;
}

/* Write raw bytes to the primary framebuffer */
size_t video_fb_write(void *ctx, const void *addr, size_t offset, size_t size)
{
    uint8_t *dst;
    size_t   fb_size;

    (void)ctx;
    if (!addr) return 0;

    fb_size = (size_t)(stride * height * sizeof(uint32_t));
    if (offset >= fb_size) return 0;

    if (size > fb_size - offset) size = fb_size - offset;
    dst = (uint8_t *)buffer + offset;
    memcpy(dst, addr, size);
    return size;
}

/* Query framebuffer device metadata */
int video_fb_ioctl(void *ctx, size_t req, void *arg)
{
    video_info_t info;

    (void)ctx;
    if (req != FBDEV_IOCTL_GET_INFO) return -EINVAL;
    if (!arg) return -EFAULT;

    info                 = video_get_info();
    fbdev_info_t fb_info = {
        .width            = info.width,
        .height           = info.height,
        .stride           = info.stride,
        .bpp              = info.bpp,
        .size             = info.stride * info.height * sizeof(uint32_t),
        .red_mask_size    = info.red_mask_size,
        .red_mask_shift   = info.red_mask_shift,
        .green_mask_size  = info.green_mask_size,
        .green_mask_shift = info.green_mask_shift,
        .blue_mask_size   = info.blue_mask_size,
        .blue_mask_shift  = info.blue_mask_shift,
    };
    if (copy_to_user(arg, &fb_info, sizeof(fb_info))) return -EFAULT;
    return EOK;
}

/* Initialize Video */
void video_init(void)
{
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count < 1) krn_halt();
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    buffer                                 = framebuffer->address;
    width                                  = framebuffer->width;
    height                                 = framebuffer->height;
    stride                                 = framebuffer->pitch / (framebuffer->bpp / 8);

    fbcon_init();
    video_clear();
}

/* Clear screen */
void video_clear(void)
{
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    cx = cy = 0;
    if (video_flush_cb) video_flush_cb();
}

/* Clear screen with color */
void video_clear_color(uint32_t color)
{
    back_color = color;
    for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    cx = cy = 0;
}

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    (buffer)[y * stride + x] = color;
}

/* Get a pixel at the specified coordinates on the screen */
uint32_t video_get_pixel(uint32_t x, uint32_t y)
{
    return (buffer)[y * stride + x];
}

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_invoke_area(position_t p0, position_t p1, void (*callback)(position_t p))
{
    position_t p;
    for (p.y = p0.y; p.y <= p1.y; p.y++)
        for (p.x = p0.x; p.x <= p1.x; p.x++) callback(p);
}

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(position_t p0, position_t p1, uint32_t color)
{
    uint32_t x0 = p0.x;
    uint32_t y0 = p0.y;
    uint32_t x1 = p1.x;
    uint32_t y1 = p1.y;
    for (uint32_t y = y0; y <= y1; y++) {
        /* Draw horizontal line */
#if defined(__x86_64__) || defined(__i386__)
        uint32_t *line  = buffer + y * stride + x0;
        size_t    count = x1 - x0 + 1;
        __asm__ volatile("rep stosl" : "+D"(line), "+c"(count) : "a"(color) : "memory");
#else
        for (uint32_t x = x0; x <= x1; x++) video_draw_pixel(x, y, color);
#endif
    }
    if (video_flush_cb) video_flush_cb();
}

/*
 * video_switch_to_drm — redirect fbcon output to a DRM GEM backing buffer.
 *
 * After this call all printk / tty output renders into the DRM buffer
 * instead of the boot-time Limine framebuffer.  The @flush callback is
 * invoked after each batch draw to push pixels to the host GPU.
 */
void video_switch_to_drm(void *backing, uint32_t w, uint32_t h, uint32_t pitch, video_flush_fn_t flush)
{
    if (!backing || !flush) return;

    buffer = (uint32_t *)backing;
    width  = w;
    height = h;
    stride = pitch / sizeof(uint32_t); /* pixels per line */

    video_flush_cb = flush;

    /* Rebuild the fbcon character grid for the new resolution */
    fbcon_resize();

    /* Clear to black and flush, then redraw the boot logo and flush */
    video_clear();
#if BOOT_LOGO
    video_redraw_logo();
    if (video_flush_cb) video_flush_cb(); /* flush logo pixels to host */
#endif

    plogk("video: switched console to DRM framebuffer %ux%u stride=%u\n", w, h, (uint32_t)stride);
}
