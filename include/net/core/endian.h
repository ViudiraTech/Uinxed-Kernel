/*
 *
 *      endian.h
 *      Endian conversion functions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ENDIAN_H_
#define INCLUDE_ENDIAN_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

static inline uint16_t net_bswap16(uint16_t value)
{
    return __builtin_bswap16(value);
}

static inline uint32_t net_bswap32(uint32_t value)
{
    return __builtin_bswap32(value);
}

static inline uint16_t net_htons(uint16_t value)
{
    return net_bswap16(value);
}

static inline uint16_t net_ntohs(uint16_t value)
{
    return net_bswap16(value);
}

static inline uint32_t net_htonl(uint32_t value)
{
    return net_bswap32(value);
}

static inline uint32_t net_ntohl(uint32_t value)
{
    return net_bswap32(value);
}

static inline uint16_t net_read_be16(const void *ptr)
{
    const uint8_t *p = ptr;
    return (uint16_t)((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t net_read_be32(const void *ptr)
{
    const uint8_t *p = ptr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void net_write_be16(void *ptr, uint16_t value)
{
    uint8_t *p = ptr;
    p[0]       = (uint8_t)(value >> 8);
    p[1]       = (uint8_t)value;
}

static inline void net_write_be32(void *ptr, uint32_t value)
{
    uint8_t *p = ptr;
    p[0]       = (uint8_t)(value >> 24);
    p[1]       = (uint8_t)(value >> 16);
    p[2]       = (uint8_t)(value >> 8);
    p[3]       = (uint8_t)value;
}

/* Incremental internet checksum helpers. */
uint32_t net_checksum_add(uint32_t sum, const void *data, size_t length);
uint16_t net_checksum_finish(uint32_t sum);
uint16_t net_checksum(const void *data, size_t length);
uint16_t net_checksum_ipv4_pseudo(uint32_t source, uint32_t destination, uint8_t protocol, const void *data, size_t length);

#endif // INCLUDE_ENDIAN_H_
