/*
 *
 *      byteorder.h
 *      Unaligned little-endian / big-endian load/store
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BYTEORDER_H_
#define INCLUDE_BYTEORDER_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Load an unaligned little-endian value from a byte buffer. */
uint16_t load_le16(const void *data);
uint32_t load_le32(const void *data);
uint64_t load_le64(const void *data);

/* Store an unaligned little-endian value into a byte buffer. */
void store_le16(void *data, uint16_t value);
void store_le32(void *data, uint32_t value);
void store_le64(void *data, uint64_t value);

/* Load an unaligned big-endian value from a byte buffer. */
uint16_t load_be16(const void *data);
uint32_t load_be32(const void *data);
uint64_t load_be64(const void *data);

/* Store an unaligned big-endian value into a byte buffer. */
void store_be16(void *data, uint16_t value);
void store_be32(void *data, uint32_t value);
void store_be64(void *data, uint64_t value);

/* Byte-swap a value between host and big-endian order. */
uint16_t bswap16(uint16_t value);
uint32_t bswap32(uint32_t value);
uint64_t bswap64(uint64_t value);

#endif // INCLUDE_BYTEORDER_H_
