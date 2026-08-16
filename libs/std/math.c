/*
 *
 *      math.c
 *      Mathematical library
 *
 *      2025/10/7 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/fpu.h>
#include <libs/std/math.h>

/*
 * Enable SSE2 for this translation unit so FP code compiles despite the
 * kernel-wide -mno-sse -mno-sse2 flags.  GCC uses #pragma GCC target;
 * clang does not implement it and needs #pragma clang attribute instead.
 */
#if defined(__clang__)
#    pragma clang attribute push(__attribute__((target("sse2"))), apply_to = function)
#elif defined(__GNUC__)
#    pragma GCC target("sse2")
#endif

/* Round a floating-point number to the nearest integer */
int round(float64_t x)
{
    if (!kernel_sse_available()) return 0;
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
    if (!kernel_sse_available()) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }
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
    if (!kernel_sse_available()) return 0.0f;
    kernel_fpu_begin();
    float fract = x - (float)(int)x;
    float r     = (fract > 0) ? (float)((int)x + 1) : (float)(int)x;
    kernel_fpu_end();
    return r;
}

/* Return the largest integer value less than or equal to the argument */
float floorf(float x)
{
    if (!kernel_sse_available()) return 0.0f;
    kernel_fpu_begin();
    float fract = x - (float)(int)x;
    float r     = (fract < 0) ? (float)((int)x - 1) : (float)(int)x;
    kernel_fpu_end();
    return r;
}

/* Round a floating-point number to the nearest integer */
float roundf(float number)
{
    if (!kernel_sse_available()) return 0.0f;
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
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
    double r = (x < 0) ? -x : x;
    kernel_fpu_end();
    return r;
}

/* Return the largest integer less than or equal to x */
double floor(double x)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
    double fract = x - (int)x;
    double r     = (fract < 0) ? (int)x - 1 : (int)x;
    kernel_fpu_end();
    return r;
}

/* Return the smallest integer greater than or equal to x */
double ceil(double x)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
    double fract = x - (int)x;
    double r     = (fract > 0) ? (int)x + 1 : (int)x;
    kernel_fpu_end();
    return r;
}

/* Return the remainder of x divided by y, with the sign of x */
double fmod(double x, double y)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();

    double ax = fabs(x);
    double ay = fabs(y);
    double r;

    if (y == 0.0 || __builtin_isnan(x) || __builtin_isnan(y) || __builtin_isinf(x))
        r = __builtin_nanf("");
    else if (__builtin_isinf(y) || ax < ay)
        r = x; /* fmod(x, +-Inf) == x, and |x| < |y| gives x */
    else {
        /* Scale |y| up to the magnitude of |x|, then subtract it back down
         * in a binary long division.  This never truncates x/y through an
         * integer, so the quotient cannot overflow. */
        double m = ay;
        while (m <= ax * 0.5) m *= 2.0;

        r = ax;
        while (m >= ay) {
            if (r >= m) r -= m;
            m *= 0.5;
        }
        if (x < 0.0) r = -r;
    }

    kernel_fpu_end();
    return r;
}

/* Calculate the cosine of x (in radians) */
double cos(double x)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();

    /* cos is undefined at NaN and +-Inf. */
    if (__builtin_isnan(x) || __builtin_isinf(x)) {
        kernel_fpu_end();
        return __builtin_nanf("");
    }

    /* Reduce x into [-pi, pi] so the Taylor series converges quickly.
     * fmod() performs the reduction without truncating x / 2pi through an
     * integer, so it stays correct for large arguments. */
    const double pi     = 3.14159265358979323846;
    const double two_pi = 2.0 * pi;
    x                   = fmod(x, two_pi);
    if (x > pi)
        x -= two_pi;
    else if (x < -pi)
        x += two_pi;

    double sum  = 0.0;
    double term = 1.0;
    int    n    = 0;
    while (fabs(term) > 1e-15) {
        sum += term;
        term *= -x * x / ((2.0 * n + 1.0) * (2.0 * n + 2.0));
        n++;
    }
    kernel_fpu_end();
    return sum;
}

/* Calculate the square root of a number */
double sqrt(double number)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
    double r;
    if (number < 0) {
        r = __builtin_nanf("");
    } else if (number == 0.0) {
        r = 0.0;
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

/* Calculate the arc cosine (inverse cosine) of x, in [0, pi] */
double acos(double x)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();

    /* acos is only defined on [-1, 1]; the exact endpoints are handled
     * explicitly because acos is ill-conditioned there. */
    const double pi = 3.14159265358979323846;
    if (__builtin_isnan(x) || x > 1.0 || x < -1.0) {
        kernel_fpu_end();
        return __builtin_nanf("");
    }
    if (x == 1.0) {
        kernel_fpu_end();
        return 0.0;
    }
    if (x == -1.0) {
        kernel_fpu_end();
        return pi;
    }

    /* Newton's method on f(theta) = cos(theta) - x over theta in [0, pi]. */
    double theta = pi / 2.0;
    for (int i = 0; i < 100; i++) {
        double c = cos(theta);
        if (c > 1.0) c = 1.0;
        if (c < -1.0) c = -1.0;
        double s = sqrt((1.0 - c) * (1.0 + c));
        if (s < 1e-12) break;
        double next = theta + (c - x) / s;
        if (fabs(next - theta) < 1e-15) {
            theta = next;
            break;
        }
        theta = next;
    }
    kernel_fpu_end();
    return theta;
}

/* Calculate x raised to the power of y */
double pow(double x, int y)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
    double result = 1.0;
    if (y >= 0) {
        for (int i = 0; i < y; i++) result *= x;
    } else {
        for (int i = y; i < 0; i++) result *= x;
        result = 1.0 / result;
    }
    kernel_fpu_end();
    return result;
}

/* Multiply x by 2 raised to the power of exp */
double ldexp(double x, int exp)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
    double r = x;
    if (exp >= 0) {
        for (int i = 0; i < exp; i++) r *= 2.0;
    } else {
        for (int i = exp; i < 0; i++) r *= 0.5;
    }
    kernel_fpu_end();
    return r;
}

/* Return the absolute value of an integer */
int abs(int x)
{
    return (x < 0 ? -x : x);
}

#if defined(__clang__)
#    pragma clang attribute pop
#endif
