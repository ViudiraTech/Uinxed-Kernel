/*
 *
 *      klogo.h
 *      Kernel logo header file
 *
 *      2026/7/22 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_KLOGO_H_
#define INCLUDE_KLOGO_H_

#include <libs/std/stdint.h>

#define KLOGO_WIDTH       77
#define KLOGO_HEIGHT      90
#define KLOGO_AREA_HEIGHT 112
#define KLOGO_LEFT_MARGIN 5
#define KLOGO_GAP         15

#ifndef BOOT_LOGO
#    define BOOT_LOGO 1
#endif

extern uint8_t klogo_data[];

/* Draw the kernel logo */
void video_draw_logo(uint32_t count);

/* Clean the kernel logo */
void video_clear_logo(void);

#endif // INCLUDE_KLOGO_H_
