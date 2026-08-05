/*
 *
 *      crc32c.c
 *      CRC32C (Castagnoli) checksum
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/data/crc32c.h>

uint32_t crc32c_update(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size--) {
        crc ^= *bytes++;
        for (uint32_t bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0x82F63B78U & (uint32_t) - (int32_t)(crc & 1));
    }
    return crc;
}
