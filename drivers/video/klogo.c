/*
 *
 *      klogo.c
 *      Kernel logo
 *
 *      2026/7/22 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/gfxs/bmp.h>
#include <video/klogo.h>
#include <video/video.h>

/* Draw the kernel logo */
void video_draw_logo(uint32_t count)
{
    if (count <= 0) return;

    bmp_t   *logo = (bmp_t *)klogo_data;
    uint32_t x    = KLOGO_LEFT_MARGIN;
    uint32_t y    = (KLOGO_AREA_HEIGHT - KLOGO_HEIGHT) / 2;

    for (uint32_t i = 0; i < count; i++) {
        if (x + KLOGO_WIDTH > width) break;
        bmp_analysis(logo, x, y, 1);
        x += KLOGO_WIDTH + KLOGO_GAP;
    }
}

/* Clean the kernel logo */
void video_clear_logo(void)
{
    video_draw_rect((position_t) {0, 0}, (position_t) {width - 1, KLOGO_AREA_HEIGHT - 1}, 0x00000000);
}
