/*
 *
 *      overflow.c
 *      Checked arithmetic on the kernel integer types
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/util/overflow.h>

/* Add two unsigned 32-bit values, reporting a carry out of the result type. */
extern inline __attribute__((always_inline)) bool add_overflow_u32(uint32_t left, uint32_t right, uint32_t *res)
{
#if __has_builtin(__builtin_add_overflow)
    return __builtin_add_overflow(left, right, res);
#else
    uint32_t sum = left + right;
    *res         = sum;
    return sum < left;
#endif
}

/* Add two unsigned 64-bit values, reporting a carry out of the result type. */
extern inline __attribute__((always_inline)) bool add_overflow_u64(uint64_t left, uint64_t right, uint64_t *res)
{
#if __has_builtin(__builtin_add_overflow)
    return __builtin_add_overflow(left, right, res);
#else
    uint64_t sum = left + right;
    *res         = sum;
    return sum < left;
#endif
}

/* Add two pointer-width unsigned values, reporting a carry out of the result type. */
extern inline __attribute__((always_inline)) bool add_overflow_ul(unsigned long left, unsigned long right, unsigned long *res)
{
#if __has_builtin(__builtin_add_overflow)
    return __builtin_add_overflow(left, right, res);
#else
    unsigned long sum = left + right;
    *res              = sum;
    return sum < left;
#endif
}

/* Add two signed 32-bit values, reporting a carry into or out of the sign bit. */
extern inline __attribute__((always_inline)) bool add_overflow_s32(int32_t left, int32_t right, int32_t *res)
{
#if __has_builtin(__builtin_add_overflow)
    return __builtin_add_overflow(left, right, res);
#else
    int32_t sum = (int32_t)((uint32_t)left + (uint32_t)right);
    *res        = sum;
    return ((left ^ sum) & (right ^ sum)) < 0;
#endif
}

/* Add two signed 64-bit values, reporting a carry into or out of the sign bit. */
extern inline __attribute__((always_inline)) bool add_overflow_s64(int64_t left, int64_t right, int64_t *res)
{
#if __has_builtin(__builtin_add_overflow)
    return __builtin_add_overflow(left, right, res);
#else
    int64_t sum = (int64_t)((uint64_t)left + (uint64_t)right);
    *res        = sum;
    return ((left ^ sum) & (right ^ sum)) < 0;
#endif
}

/* Add two pointer-width signed values, reporting a carry into or out of the sign bit. */
extern inline __attribute__((always_inline)) bool add_overflow_sl(long left, long right, long *res)
{
#if __has_builtin(__builtin_add_overflow)
    return __builtin_add_overflow(left, right, res);
#else
    long sum = (long)((unsigned long)left + (unsigned long)right);
    *res     = sum;
    return ((left ^ sum) & (right ^ sum)) < 0;
#endif
}

/* Subtract two unsigned 32-bit values, reporting a borrow out of the result type. */
extern inline __attribute__((always_inline)) bool sub_overflow_u32(uint32_t left, uint32_t right, uint32_t *res)
{
#if __has_builtin(__builtin_sub_overflow)
    return __builtin_sub_overflow(left, right, res);
#else
    uint32_t difference = left - right;
    *res                = difference;
    return left < right;
#endif
}

/* Subtract two unsigned 64-bit values, reporting a borrow out of the result type. */
extern inline __attribute__((always_inline)) bool sub_overflow_u64(uint64_t left, uint64_t right, uint64_t *res)
{
#if __has_builtin(__builtin_sub_overflow)
    return __builtin_sub_overflow(left, right, res);
#else
    uint64_t difference = left - right;
    *res                = difference;
    return left < right;
#endif
}

/* Subtract two pointer-width unsigned values, reporting a borrow out of the result type. */
extern inline __attribute__((always_inline)) bool sub_overflow_ul(unsigned long left, unsigned long right, unsigned long *res)
{
#if __has_builtin(__builtin_sub_overflow)
    return __builtin_sub_overflow(left, right, res);
#else
    unsigned long difference = left - right;
    *res                     = difference;
    return left < right;
#endif
}

/* Subtract two signed 32-bit values, reporting a borrow into or out of the sign bit. */
extern inline __attribute__((always_inline)) bool sub_overflow_s32(int32_t left, int32_t right, int32_t *res)
{
#if __has_builtin(__builtin_sub_overflow)
    return __builtin_sub_overflow(left, right, res);
#else
    int32_t difference = (int32_t)((uint32_t)left - (uint32_t)right);
    *res               = difference;
    return ((left ^ right) & (left ^ difference)) < 0;
#endif
}

/* Subtract two signed 64-bit values, reporting a borrow into or out of the sign bit. */
extern inline __attribute__((always_inline)) bool sub_overflow_s64(int64_t left, int64_t right, int64_t *res)
{
#if __has_builtin(__builtin_sub_overflow)
    return __builtin_sub_overflow(left, right, res);
#else
    int64_t difference = (int64_t)((uint64_t)left - (uint64_t)right);
    *res               = difference;
    return ((left ^ right) & (left ^ difference)) < 0;
#endif
}

/* Subtract two pointer-width signed values, reporting a borrow into or out of the sign bit. */
extern inline __attribute__((always_inline)) bool sub_overflow_sl(long left, long right, long *res)
{
#if __has_builtin(__builtin_sub_overflow)
    return __builtin_sub_overflow(left, right, res);
#else
    long difference = (long)((unsigned long)left - (unsigned long)right);
    *res            = difference;
    return ((left ^ right) & (left ^ difference)) < 0;
#endif
}

/* Multiply two unsigned 32-bit values, reporting a product that does not fit. */
extern inline __attribute__((always_inline)) bool mul_overflow_u32(uint32_t left, uint32_t right, uint32_t *res)
{
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(left, right, res);
#else
    uint32_t product = left * right;
    *res             = product;
    return left != 0 && product / left != right;
#endif
}

/* Multiply two unsigned 64-bit values, reporting a product that does not fit. */
extern inline __attribute__((always_inline)) bool mul_overflow_u64(uint64_t left, uint64_t right, uint64_t *res)
{
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(left, right, res);
#else
    uint64_t product = left * right;
    *res             = product;
    return left != 0 && product / left != right;
#endif
}

/* Multiply two pointer-width unsigned values, reporting a product that does not fit. */
extern inline __attribute__((always_inline)) bool mul_overflow_ul(unsigned long left, unsigned long right, unsigned long *res)
{
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(left, right, res);
#else
    unsigned long product = left * right;
    *res                  = product;
    return left != 0 && product / left != right;
#endif
}

/* Multiply two signed 32-bit values, reporting a product that does not fit. */
extern inline __attribute__((always_inline)) bool mul_overflow_s32(int32_t left, int32_t right, int32_t *res)
{
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(left, right, res);
#else
    /* The full product of two 32-bit values always fits in 64 bits. */
    int64_t product = (int64_t)left * (int64_t)right;
    *res            = (int32_t)product;
    return product < INT32_MIN || product > INT32_MAX;
#endif
}

/* Multiply two signed 64-bit values, reporting a product that does not fit. */
extern inline __attribute__((always_inline)) bool mul_overflow_s64(int64_t left, int64_t right, int64_t *res)
{
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(left, right, res);
#else
    /* The full product of two 64-bit values always fits in 128 bits. */
    __int128 product = (__int128)left * (__int128)right;
    *res             = (int64_t)product;
    return product < INT64_MIN || product > INT64_MAX;
#endif
}

/* Multiply two pointer-width signed values, reporting a product that does not fit. */
extern inline __attribute__((always_inline)) bool mul_overflow_sl(long left, long right, long *res)
{
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(left, right, res);
#else
    /* The full product of two pointer-width values always fits in 128 bits. */
    __int128 product = (__int128)left * (__int128)right;
    *res             = (long)product;
    return product < INT64_MIN || product > INT64_MAX;
#endif
}
