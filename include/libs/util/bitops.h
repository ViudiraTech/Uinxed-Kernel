/*
 *
 *      bitops.h
 *      Bit manipulation helpers
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BITOPS_H_
#define INCLUDE_BITOPS_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Set one bit in a 32-bit-per-word bitmap. */
void set_bit(unsigned int bit, uint32_t *bitmap);

/* True when value is a non-zero power of two. */
bool is_power_of_two(uint64_t value);

/* Bit scanning.  A zero input gives the operand width for ctz()/clz(), and 0 for ffs(). */
uint32_t ctz32(uint32_t value);
uint32_t ctz64(uint64_t value);
uint32_t clz32(uint32_t value);
uint32_t clz64(uint64_t value);
uint32_t ffs32(uint32_t value);
uint32_t ffs64(uint64_t value);

/* Number of set bits, and whether that count is odd. */
uint32_t popcount32(uint32_t value);
uint32_t popcount64(uint64_t value);
uint32_t parity32(uint32_t value);
uint32_t parity64(uint64_t value);

#endif // INCLUDE_BITOPS_H_
