/*
 *
 *      crc32c.h
 *      CRC32C (Castagnoli) checksum
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_crc32c_H_
#define INCLUDE_crc32c_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

uint32_t crc32c_update(uint32_t crc, const void *data, size_t size);

#endif // INCLUDE_crc32c_H_
