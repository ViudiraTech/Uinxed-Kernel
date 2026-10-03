/*
 *
 *      endian.c
 *      Network byte-order helpers
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <net/core/endian.h>

#if CONFIG_NET

/* Convert a 16-bit value from host to network byte order. */
extern inline __attribute__((always_inline)) uint16_t net_htons(uint16_t value)
{
    return bswap16(value);
}

/* Convert a 16-bit value from network to host byte order. */
extern inline __attribute__((always_inline)) uint16_t net_ntohs(uint16_t value)
{
    return bswap16(value);
}

/* Convert a 32-bit value from host to network byte order. */
extern inline __attribute__((always_inline)) uint32_t net_htonl(uint32_t value)
{
    return bswap32(value);
}

/* Convert a 32-bit value from network to host byte order. */
extern inline __attribute__((always_inline)) uint32_t net_ntohl(uint32_t value)
{
    return bswap32(value);
}

#endif
