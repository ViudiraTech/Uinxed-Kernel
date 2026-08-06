/*
 *
 *      math.c
 *      Mathematical library
 *
 *      2025/10/7 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/fpu.h>
#include <libs/std/math.h>

#if !defined(__clang__)
#    pragma GCC target("sse2")
#endif

/* Round a floating-point number to the nearest integer */
int round(float64_t x)
{
    kernel_fpu_begin();
    int r;
    if (x >= 0.0)
        r = (int)(x + 0.5);
    else
        r = (int)(x - 0.5);
    kernel_fpu_end();
    return r;
}

/* Convert a float to a string with a specified precision */
char *ftoa(double f, char *buf, int precision)
{
    kernel_fpu_begin();

    char *ptr = buf;
    char *p1;
    char  c;
    long  intPart;

    if (precision > 10) precision = 10;
    if (f < 0) {
        f      = -f;
        *ptr++ = '-';
    }
    if (precision < 0) {
        if (f < 1.0)
            precision = 6;
        else if (f < 10.0)
            precision = 5;
        else if (f < 100.0)
            precision = 4;
        else if (f < 1000.0)
            precision = 3;
        else if (f < 10000.0)
            precision = 2;
        else if (f < 100000.0)
            precision = 1;
        else
            precision = 0;
    }
    if (precision) f += rounders[precision];

    intPart = (long)f;
    f -= (double)intPart;

    if (!intPart) {
        *ptr++ = '0';
    } else {
        char *p = ptr;
        while (intPart) {
            *p++ = (char)('0' + (int)(intPart % 10));
            intPart /= 10;
        }
        p1 = p;
        while (p > ptr) {
            c      = *--p;
            *p     = *ptr;
            *ptr++ = c;
        }
        ptr = p1;
    }
    if (precision) {
        *ptr++ = '.';
        while (precision--) {
            f *= 10.0;
            c      = (char)f;
            *ptr++ = (char)('0' + c);
            f -= c;
        }
    }
    *ptr = 0;

    kernel_fpu_end();
    return ptr;
}

/* Return the smallest integer value greater than or equal to the argument */
float ceilf(float x)
{
    kernel_fpu_begin();
    float fract = x - (float)(int)x;
    float r     = (fract > 0) ? (float)((int)x + 1) : (float)(int)x;
    kernel_fpu_end();
    return r;
}

/* Return the largest integer value less than or equal to the argument */
float floorf(float x)
{
    kernel_fpu_begin();
    float fract = x - (float)(int)x;
    float r     = (fract < 0) ? (float)((int)x - 1) : (float)(int)x;
    kernel_fpu_end();
    return r;
}

/* Round a floating-point number to the nearest integer */
float roundf(float number)
{
    kernel_fpu_begin();
    float r;
    if (number < 0.0f)
        r = ceilf(number - 0.5f);
    else
        r = floorf(number + 0.5f);
    kernel_fpu_end();
    return r;
}

/* Return the absolute value of a double */
double fabs(double x)
{
    kernel_fpu_begin();
    double r = (x < 0) ? -x : x;
    kernel_fpu_end();
    return r;
}

/* Return the largest integer less than or equal to x */
double floor(double x)
{
    kernel_fpu_begin();
    double fract = x - (int)x;
    double r     = (fract < 0) ? (int)x - 1 : (int)x;
    kernel_fpu_end();
    return r;
}

/* Return the smallest integer greater than or equal to x */
double ceil(double x)
{
    kernel_fpu_begin();
    double fract = x - (int)x;
    double r     = (fract > 0) ? (int)x + 1 : (int)x;
    kernel_fpu_end();
    return r;
}

/* Return the remainder of x divided by y */
double fmod(double x, double y)
{
    kernel_fpu_begin();
    double r;
    if (y == 0) {
        r = __builtin_nanf("");
    } else {
        double intPart   = x / y;
        double remainder = x - intPart * y;

        if (remainder < 0)
            remainder += y;
        else if (remainder > y)
            remainder -= y;
        r = remainder;
    }
    kernel_fpu_end();
    return r;
}

/* Calculate the cosine of x (in radians) */
double cos(double x)
{
    kernel_fpu_begin();
    double sum  = 0.0;
    double term = x;
    int    n    = 0;

    for (n = 0; term > 1e-15; n++) {
        term = term * (-1) * (2 * n) * (2 * n - 1) / ((2 * n) * (2 * n - 1));
        sum += term;
    }
    kernel_fpu_end();
    return sum;
}

/* Calculate the square root of a number */
double sqrt(double number)
{
    kernel_fpu_begin();
    double r;
    if (number < 0) {
        r = __builtin_nanf("");
    } else {
        double x       = number;
        double epsilon = 1e-15;
        double diff;

        do {
            x    = (x + number / x) / 2;
            diff = fabs(x - number / x);
        } while (diff > epsilon);
        r = x;
    }
    kernel_fpu_end();
    return r;
}

/* Calculate the arc cosine (inverse cosine) of x */
double acos(double x)
{
    kernel_fpu_begin();
    double x0             = x;
    double x1             = x0;
    double tolerance      = 1e-15;
    double max_iterations = 1000;
    int    iterations     = 0;

    while (iterations < max_iterations) {
        x1 = x0 - (-1 / sqrt(1 - x0 * x0)) / (1 / cos(x0));
        if (fabs(x1 - x0) < tolerance) break;
        x0 = x1;
        iterations++;
    }
    kernel_fpu_end();
    return x1;
}

/* Calculate x raised to the power of y */
double pow(double x, int y)
{
    kernel_fpu_begin();
    double result = 1.0;
    for (int i = 0; i < y; i++) result *= x;
    kernel_fpu_end();
    return result;
}

/* Multiply x by 2 raised to the power of exp */
double ldexp(double x, int exp)
{
    kernel_fpu_begin();
    int n = 2;
    for (int i = 0; i < exp - 1; i++) n *= 2;
    double r = x * (double)(n);
    kernel_fpu_end();
    return r;
}

/* Return the absolute value of an integer */
int abs(int x)
{
    return (x < 0 ? -x : x);
}
