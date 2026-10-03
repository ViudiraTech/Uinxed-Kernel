/*
 *
 *      bitops.c
 *      Bit manipulation helpers
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/util/bitops.h>

/* Set one bit in a 32-bit-per-word bitmap. */
extern inline __attribute__((always_inline)) void set_bit(unsigned int bit, uint32_t *bitmap)
{
    bitmap[bit / 32] |= 1U << (bit % 32);
}

/* True when value is a non-zero power of two. */
extern inline __attribute__((always_inline)) bool is_power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

/* Index of the lowest set bit, or the operand width when value is zero. */
extern inline __attribute__((always_inline)) uint32_t ctz32(uint32_t value)
{
#if __has_builtin(__builtin_ctz)
    return value ? (uint32_t)__builtin_ctz(value) : 32U;
#else
    if (!value) return 32U;
    uint32_t count = 0;
    while (!(value & 1U)) {
        value >>= 1;
        count++;
    }
    return count;
#endif
}

/* Index of the lowest set bit, or the operand width when value is zero. */
extern inline __attribute__((always_inline)) uint32_t ctz64(uint64_t value)
{
#if __has_builtin(__builtin_ctzll)
    return value ? (uint32_t)__builtin_ctzll(value) : 64U;
#else
    if (!value) return 64U;
    uint32_t count = 0;
    while (!(value & 1U)) {
        value >>= 1;
        count++;
    }
    return count;
#endif
}

/* Count of leading zero bits, or the operand width when value is zero. */
extern inline __attribute__((always_inline)) uint32_t clz32(uint32_t value)
{
#if __has_builtin(__builtin_clz)
    return value ? (uint32_t)__builtin_clz(value) : 32U;
#else
    if (!value) return 32U;
    uint32_t count = 0;
    while (!(value & 0x80000000U)) {
        value <<= 1;
        count++;
    }
    return count;
#endif
}

/* Count of leading zero bits, or the operand width when value is zero. */
extern inline __attribute__((always_inline)) uint32_t clz64(uint64_t value)
{
#if __has_builtin(__builtin_clzll)
    return value ? (uint32_t)__builtin_clzll(value) : 64U;
#else
    if (!value) return 64U;
    uint32_t count = 0;
    while (!(value & 0x8000000000000000ULL)) {
        value <<= 1;
        count++;
    }
    return count;
#endif
}

/* One-based index of the lowest set bit, or zero when value is zero. */
extern inline __attribute__((always_inline)) uint32_t ffs32(uint32_t value)
{
#if __has_builtin(__builtin_ffs)
    return (uint32_t)__builtin_ffs((int)value);
#else
    return value ? ctz32(value) + 1U : 0U;
#endif
}

/* One-based index of the lowest set bit, or zero when value is zero. */
extern inline __attribute__((always_inline)) uint32_t ffs64(uint64_t value)
{
#if __has_builtin(__builtin_ffsll)
    return (uint32_t)__builtin_ffsll((long long)value);
#else
    return value ? ctz64(value) + 1U : 0U;
#endif
}

/* Number of set bits in a 32-bit value. */
extern inline __attribute__((always_inline)) uint32_t popcount32(uint32_t value)
{
    value = value - ((value >> 1) & 0x55555555U);
    value = (value & 0x33333333U) + ((value >> 2) & 0x33333333U);
    value = (value + (value >> 4)) & 0x0F0F0F0FU;
    return (value * 0x01010101U) >> 24;
}

/* Number of set bits in a 64-bit value. */
extern inline __attribute__((always_inline)) uint32_t popcount64(uint64_t value)
{
    value = value - ((value >> 1) & 0x5555555555555555ULL);
    value = (value & 0x3333333333333333ULL) + ((value >> 2) & 0x3333333333333333ULL);
    value = (value + (value >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (uint32_t)((value * 0x0101010101010101ULL) >> 56);
}

/* One when the number of set bits in a 32-bit value is odd. */
extern inline __attribute__((always_inline)) uint32_t parity32(uint32_t value)
{
#if __has_builtin(__builtin_parity)
    return (uint32_t)__builtin_parity(value);
#else
    return popcount32(value) & 1U;
#endif
}

/* One when the number of set bits in a 64-bit value is odd. */
extern inline __attribute__((always_inline)) uint32_t parity64(uint64_t value)
{
#if __has_builtin(__builtin_parityll)
    return (uint32_t)__builtin_parityll(value);
#else
    return popcount64(value) & 1U;
#endif
}
