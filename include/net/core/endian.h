/*
 *
 *      endian.h
 *      Network byte-order and checksum helpers
 *
 *      2026/7/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ENDIAN_H_
#define INCLUDE_ENDIAN_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/util/byteorder.h>

/* Convert a 16/32-bit value between host and network byte order. */
uint16_t net_htons(uint16_t value);
uint16_t net_ntohs(uint16_t value);
uint32_t net_htonl(uint32_t value);
uint32_t net_ntohl(uint32_t value);

/* Incremental internet checksum helpers. */
uint32_t net_checksum_add(uint32_t sum, const void *data, size_t length);
uint16_t net_checksum_finish(uint32_t sum);
uint16_t net_checksum(const void *data, size_t length);
uint16_t net_checksum_ipv4_pseudo(uint32_t source, uint32_t destination, uint8_t protocol, const void *data, size_t length);

#endif // INCLUDE_ENDIAN_H_
