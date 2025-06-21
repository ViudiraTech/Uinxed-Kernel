/*
 *
 *      bmp.c
 *      Bitmap image
 *
 *      2025/6/22 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "bmp.h"
#include "video.h"

/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */

/* Parse bitmap images and draw them to the screen */
void bmp_analysis(Bmp *bmp, uint32_t offsetX, uint32_t offsetY, int enableTransparency)
{
    if (bmp->magic != 0x4d42 || bmp->bitsPerPixel != 24) return;

    uint8_t *data     = (uint8_t *)bmp + bmp->bmpDataOffset;
    uint32_t rowBytes = (bmp->frameWidth * 3 + 3) & ~3;

    for (uint32_t yOffset = 0; yOffset < bmp->frameHeight; ++yOffset) {
        for (uint32_t xOffset = 0; xOffset < bmp->frameWidth; ++xOffset) {
            uint32_t pixelOffset = yOffset * rowBytes + xOffset * 3;
            uint8_t b            = data[pixelOffset + 0];
            uint8_t g            = data[pixelOffset + 1];
            uint8_t r            = data[pixelOffset + 2];
            uint32_t color       = (r << 16) | (g << 8) | b;

            if (enableTransparency && color == 0) continue;
            video_draw_pixel(offsetX + xOffset, offsetY + bmp->frameHeight - 1 - yOffset, color);
        }
    }
    return;
}

/* NOLINTEND(bugprone-easily-swappable-parameters) */
