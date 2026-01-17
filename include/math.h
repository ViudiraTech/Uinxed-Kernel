/*
 *
 *      math.h
 *      Mathematical library header files
 *
 *      2025/10/7 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MATH_H_
#define INCLUDE_MATH_H_

#include <stdint.h>

#define PI            (float64_t)3.14159265358979323846
#define TAU           (float64_t)6.28318530717958647692
#define PI_DIV_2      (float64_t)1.57079632679489661923
#define LONG_LONG_MAX (0x7FFFFFFFFFFFFFFFLL)
#define LONG_LONG_MIN (-0x7FFFFFFFFFFFFFFFLL - 1)
#define UINT64_MAX    (0xFFFFFFFFFFFFFFFFULL)
#define UINT64_MIN    (0x0000000000000000ULL)
#define INT64_MAX     (0x7FFFFFFFFFFFFFFFLL)
#define INT64_MIN     (-0x7FFFFFFFFFFFFFFFLL - 1)
#define UINT32_MAX    (0xFFFFFFFFUL)
#define UINT32_MIN    (0x00000000UL)
#define INT32_MAX     (0x7FFFFFFFUL)
#define INT32_MIN     (-0x7FFFFFFFUL - 1)
#define UINT16_MAX    (0xFFFFU)
#define UINT16_MIN    (0x0000U)
#define INT16_MAX     (0x7FFF)
#define INT16_MIN     (-0x7FFF - 1)
#define UINT8_MAX     (0xFFU)
#define UINT8_MIN     (0x00U)
#define INT8_MAX      (0x7F)
#define INT8_MIN      (-0x7F - 1)
#define SHRT_MAX      (0x7FFF)
#define SHRT_MIN      (-0x7FFF - 1)
#define INT_MAX       (0x7FFFFFFF)
#define INT_MIN       (-0x7FFFFFFF - 1)
#define UINT_MAX      (0xFFFFFFFFU)
#define UINT_MIN      (0x00000000U)
#define LONG_MAX      (0x7FFFFFFFFFFFFFFFL)
#define LONG_MIN      (-0x7FFFFFFFFFFFFFFFL - 1)

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

static const double rounders[10 + 1] = {
    0.5,          // 01 decimal place
    0.05,         // 02 decimal place
    0.005,        // 03 decimal place
    0.0005,       // 04 decimal place
    0.00005,      // 05 decimal place
    0.000005,     // 06 decimal place
    0.0000005,    // 07 decimal place
    0.00000005,   // 08 decimal place
    0.000000005,  // 09 decimal place
    0.0000000005, // 10 decimal place
    0.0000000000  // 11 decimal place
};

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

#endif // INCLUDE_MATH_H_
