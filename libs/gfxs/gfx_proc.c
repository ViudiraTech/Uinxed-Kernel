/*
 *
 *      gfx_proc.c
 *      Graphics processing
 *
 *      2025/7/28 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <gfx_proc.h>
#include <limine.h>

/* Convert color to fb_color */
uint32_t color_to_fb_color(color_t color)
{
    struct limine_framebuffer *fb = get_framebuffer();

    uint32_t fb_red   = COLOR_MASK(color.red, fb->red_mask_size, fb->red_mask_shift);       // Red
    uint32_t fb_green = COLOR_MASK(color.green, fb->green_mask_size, fb->green_mask_shift); // Blue
    uint32_t fb_blue  = COLOR_MASK(color.blue, fb->blue_mask_size, fb->blue_mask_shift);    // Green
    uint32_t fb_color = fb_red | fb_green | fb_blue;
    return fb_color;
}

/* Convert fb_color to color */
color_t fb_color_to_color(uint32_t fb_color)
{
    struct limine_framebuffer *fb = get_framebuffer();

    uint32_t red   = COLOR_UNMASK(fb_color, fb->red_mask_size, fb->red_mask_shift);     // Red
    uint32_t green = COLOR_UNMASK(fb_color, fb->green_mask_size, fb->green_mask_shift); // Blue
    uint32_t blue  = COLOR_UNMASK(fb_color, fb->blue_mask_size, fb->blue_mask_shift);   // Green
    return (color_t) {red, green, blue};
}
