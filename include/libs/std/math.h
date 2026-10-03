/*
 *
 *      math.h
 *      Mathematical library header files
 *
 *      2025/10/7 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MATH_H_
#define INCLUDE_MATH_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define PI            (float64_t)3.1415926535
#define TAU           (float64_t)6.2831853071
#define PI_DIV_2      (float64_t)1.5707963267
#define LONG_LONG_MAX (0x7FFFFFFFFFFFFFFFLL)
#define LONG_LONG_MIN (-0x7FFFFFFFFFFFFFFFLL - 1)
#define UINT64_MIN    (0x0000000000000000ULL)
#define UINT32_MIN    (0x00000000UL)
#define UINT16_MIN    (0x0000U)
#define UINT8_MIN     (0x00U)
#define SHRT_MAX      (0x7FFF)
#define SHRT_MIN      (-0x7FFF - 1)
#define INT_MAX       (0x7FFFFFFF)
#define INT_MIN       (-0x7FFFFFFF - 1)
#define UINT_MAX      (0xFFFFFFFFU)
#define UINT_MIN      (0x00000000U)
#define LONG_MAX      (0x7FFFFFFFFFFFFFFFL)
#define LONG_MIN      (-0x7FFFFFFFFFFFFFFFL - 1)

/* Categories returned by fpclassify(). */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define FORCE_EVAL(x)                                         \
    do {                                                      \
        if (sizeof(x) == sizeof(float)) {                     \
            volatile float __x __attribute__((unused));       \
            __x = (x);                                        \
        } else if (sizeof(x) == sizeof(double)) {             \
            volatile double __x __attribute__((unused));      \
            __x = (x);                                        \
        } else {                                              \
            volatile long double __x __attribute__((unused)); \
            __x = (x);                                        \
        }                                                     \
    } while (0)

/* Round a floating-point number to the nearest integer */
int round(float64_t x);

/* Convert a float to a string with a specified precision */
char *ftoa(double f, char *buf, int precision);

/* Return the smallest integer value greater than or equal to the argument */
float ceilf(float x);

/* Return the largest integer value less than or equal to the argument */
float floorf(float x);

/* Round a floating-point number to the nearest integer */
float roundf(float number);

/* Return the absolute value of a double */
double fabs(double x);

/* Return the largest integer less than or equal to x */
double floor(double x);

/* Return the smallest integer greater than or equal to x */
double ceil(double x);

/* Return the remainder of x divided by y */
double fmod(double x, double y);

/* Calculate the cosine of x (in radians) */
double cos(double x);

/* Calculate the square root of a number */
double sqrt(double number);

/* Calculate the arc cosine (inverse cosine) of x */
double acos(double x);

/* Calculate x raised to the power of y */
double pow(double x, int y);

/* Multiply x by 2 raised to the power of exp */
double ldexp(double x, int exp);

/* Return the absolute value of an integer */
int abs(int x);

/* True when x is a NaN */
int isnan(double x);

/* True when x is an infinity */
int isinf(double x);

/* True when x is neither a NaN nor an infinity */
int isfinite(double x);

/* True when x is a normal value (not zero, subnormal, infinite or NaN) */
int isnormal(double x);

/* True when the sign bit of x is set */
int signbit(double x);

/* Classify x into one of the FP_* categories */
int fpclassify(double x);

/* Return a quiet NaN */
float  nanf(const char *tagp);
double nan(const char *tagp);

#endif // INCLUDE_MATH_H_
