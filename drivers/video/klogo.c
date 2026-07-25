/*
 *
 *      klogo.c
 *      Kernel logo
 *
 *      2026/7/22 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <drivers/tty.h>
#include <kernel/uinxed.h>
#include <libs/gfxs/bmp.h>
#include <video/klogo.h>
#include <video/video.h>

/* Saved CPU count so the logo can be redrawn after a framebuffer switch */
static uint32_t saved_logo_count = 0;

/* Draw the kernel logo */
void video_draw_logo(uint32_t count)
{
#if BOOT_LOGO
    if (count <= 0) return;

    saved_logo_count = count;

    bmp_t   *logo = (bmp_t *)klogo_data;
    uint32_t x    = KLOGO_LEFT_MARGIN;
    uint32_t y    = (KLOGO_AREA_HEIGHT - KLOGO_HEIGHT) / 2;

    for (uint32_t i = 0; i < count; i++) {
        if (x + KLOGO_WIDTH > width) break;
        bmp_analysis(logo, x, y, 1);
        x += KLOGO_WIDTH + KLOGO_GAP;
    }
#endif
}

/* Redraw the logo on the current framebuffer (e.g. after a DRM switch) */
void video_redraw_logo(void)
{
#if BOOT_LOGO
    if (saved_logo_count > 0) video_draw_logo(saved_logo_count);
#endif
}

/* Clean the kernel logo */
void video_clear_logo(void)
{
    video_draw_rect((position_t) {0, 0}, (position_t) {width - 1, KLOGO_AREA_HEIGHT - 1}, 0x00000000);
}

void video_show_boot_logo(void)
{
#if BOOT_LOGO
    tty_device_t *boot_tty = get_boot_tty();
    if (boot_tty->type == TTY_DEVICE_VGA || boot_tty->type == TTY_DEVICE_DRM) {
        struct limine_smp_response *smp = smp_request.response;
        video_draw_logo((!CPU_MAX_COUNT) ? smp->cpu_count : (smp->cpu_count > CPU_MAX_COUNT ? CPU_MAX_COUNT : smp->cpu_count));
    }
#endif
}
