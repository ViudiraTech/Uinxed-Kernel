/*
 *
 *      overflow.h
 *      Checked arithmetic on the kernel integer types
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_OVERFLOW_H_
#define INCLUDE_OVERFLOW_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/*
 * Each operation stores the wrapped result through `res` and reports whether it
 * overflowed.  Operands are converted to the type `res` points at before the
 * check, so the destination type governs the reported range.
 */
bool add_overflow_u32(uint32_t left, uint32_t right, uint32_t *res);
bool add_overflow_u64(uint64_t left, uint64_t right, uint64_t *res);
bool add_overflow_ul(unsigned long left, unsigned long right, unsigned long *res);
bool add_overflow_s32(int32_t left, int32_t right, int32_t *res);
bool add_overflow_s64(int64_t left, int64_t right, int64_t *res);
bool add_overflow_sl(long left, long right, long *res);

bool sub_overflow_u32(uint32_t left, uint32_t right, uint32_t *res);
bool sub_overflow_u64(uint64_t left, uint64_t right, uint64_t *res);
bool sub_overflow_ul(unsigned long left, unsigned long right, unsigned long *res);
bool sub_overflow_s32(int32_t left, int32_t right, int32_t *res);
bool sub_overflow_s64(int64_t left, int64_t right, int64_t *res);
bool sub_overflow_sl(long left, long right, long *res);

bool mul_overflow_u32(uint32_t left, uint32_t right, uint32_t *res);
bool mul_overflow_u64(uint64_t left, uint64_t right, uint64_t *res);
bool mul_overflow_ul(unsigned long left, unsigned long right, unsigned long *res);
bool mul_overflow_s32(int32_t left, int32_t right, int32_t *res);
bool mul_overflow_s64(int64_t left, int64_t right, int64_t *res);
bool mul_overflow_sl(long left, long right, long *res);

/*
 * Dispatch on the type `res` points at.  The table covers the integer types the
 * kernel uses: uint32_t / unsigned int, uint64_t / unsigned long long,
 * size_t / unsigned long / uintptr_t, int32_t / int, int64_t / long long, and
 * long / ssize_t / ptrdiff_t.  Any other destination type is a compile error.
 */
#define __OVERFLOW_PICK(name, res) _Generic((res), uint32_t *: name##_u32, uint64_t *: name##_u64, unsigned long *: name##_ul, int32_t *: name##_s32, int64_t *: name##_s64, long *: name##_sl)

#define add_overflow(left, right, res) __OVERFLOW_PICK(add_overflow, (res))((left), (right), (res))
#define sub_overflow(left, right, res) __OVERFLOW_PICK(sub_overflow, (res))((left), (right), (res))
#define mul_overflow(left, right, res) __OVERFLOW_PICK(mul_overflow, (res))((left), (right), (res))

#endif // INCLUDE_OVERFLOW_H_
