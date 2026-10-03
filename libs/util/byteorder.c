/*
 *
 *      byteorder.c
 *      Unaligned little-endian / big-endian load/store
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/util/byteorder.h>

/* Load an unaligned little-endian 16-bit value. */
extern inline __attribute__((always_inline)) uint16_t load_le16(const void *data)
{
    const uint8_t *p = data;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Load an unaligned little-endian 32-bit value. */
extern inline __attribute__((always_inline)) uint32_t load_le32(const void *data)
{
    const uint8_t *p = data;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Load an unaligned little-endian 64-bit value. */
extern inline __attribute__((always_inline)) uint64_t load_le64(const void *data)
{
    const uint8_t *p = data;
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* Store an unaligned little-endian 16-bit value. */
extern inline __attribute__((always_inline)) void store_le16(void *data, uint16_t value)
{
    uint8_t *p = data;
    p[0]       = (uint8_t)value;
    p[1]       = (uint8_t)(value >> 8);
}

/* Store an unaligned little-endian 32-bit value. */
extern inline __attribute__((always_inline)) void store_le32(void *data, uint32_t value)
{
    uint8_t *p = data;
    p[0]       = (uint8_t)value;
    p[1]       = (uint8_t)(value >> 8);
    p[2]       = (uint8_t)(value >> 16);
    p[3]       = (uint8_t)(value >> 24);
}

/* Store an unaligned little-endian 64-bit value. */
extern inline __attribute__((always_inline)) void store_le64(void *data, uint64_t value)
{
    uint8_t *p = data;
    store_le32(p, (uint32_t)value);
    store_le32(p + sizeof(uint32_t), (uint32_t)(value >> 32));
}

/* Load an unaligned big-endian 16-bit value. */
extern inline __attribute__((always_inline)) uint16_t load_be16(const void *data)
{
    const uint8_t *p = data;
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

/* Load an unaligned big-endian 32-bit value. */
extern inline __attribute__((always_inline)) uint32_t load_be32(const void *data)
{
    const uint8_t *p = data;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* Load an unaligned big-endian 64-bit value. */
extern inline __attribute__((always_inline)) uint64_t load_be64(const void *data)
{
    return (uint64_t)load_be32(data) << 32 | load_be32((const uint8_t *)data + 4);
}

/* Store an unaligned big-endian 16-bit value. */
extern inline __attribute__((always_inline)) void store_be16(void *data, uint16_t value)
{
    uint8_t *p = data;
    p[0]       = (uint8_t)(value >> 8);
    p[1]       = (uint8_t)value;
}

/* Store an unaligned big-endian 32-bit value. */
extern inline __attribute__((always_inline)) void store_be32(void *data, uint32_t value)
{
    uint8_t *p = data;
    p[0]       = (uint8_t)(value >> 24);
    p[1]       = (uint8_t)(value >> 16);
    p[2]       = (uint8_t)(value >> 8);
    p[3]       = (uint8_t)value;
}

/* Store an unaligned big-endian 64-bit value. */
extern inline __attribute__((always_inline)) void store_be64(void *data, uint64_t value)
{
    uint8_t *p = data;
    store_be32(p, (uint32_t)(value >> 32));
    store_be32(p + sizeof(uint32_t), (uint32_t)value);
}

/* Byte-swap a 16-bit value. */
extern inline __attribute__((always_inline)) uint16_t bswap16(uint16_t value)
{
#if __has_builtin(__builtin_bswap16)
    return __builtin_bswap16(value);
#else
    return (uint16_t)((value >> 8) | (value << 8));
#endif
}

/* Byte-swap a 32-bit value. */
extern inline __attribute__((always_inline)) uint32_t bswap32(uint32_t value)
{
#if __has_builtin(__builtin_bswap32)
    return __builtin_bswap32(value);
#else
    return (value >> 24) | ((value & 0x0000FF00U) << 8) | ((value & 0x00FF0000U) >> 8) | (value << 24);
#endif
}

/* Byte-swap a 64-bit value. */
extern inline __attribute__((always_inline)) uint64_t bswap64(uint64_t value)
{
#if __has_builtin(__builtin_bswap64)
    return __builtin_bswap64(value);
#else
    return (uint64_t)bswap32((uint32_t)value) << 32 | bswap32((uint32_t)(value >> 32));
#endif
}
