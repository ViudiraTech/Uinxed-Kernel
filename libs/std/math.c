/*
 *
 *      math.c
 *      Mathematical library
 *
 *      2025/10/7 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/fpu.h>
#include <libs/std/math.h>

/*
 * Enable SSE2 for this translation unit so FP code compiles despite the
 * kernel-wide -mno-sse -mno-sse2 flags.  GCC uses #pragma GCC target;
 * clang does not implement it and needs #pragma clang attribute instead.
 */

#ifdef __clang__
#    pragma clang attribute push(__attribute__((target("sse2"))), apply_to = function)
#elif defined(__GNUC__)
#    pragma GCC target("sse2")
#endif

#if defined(__GNUC__) && !defined(__clang__)
#    define __ROUND_BUILTIN_FOLDS 1
#else
#    define __ROUND_BUILTIN_FOLDS 0
#endif

/* Rounding offsets indexed by decimal precision (used by ftoa). */
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
int round(float64_t x)
{
    if (!kernel_sse_available()) return 0;
    kernel_fpu_begin();
    int r;
    if (x >= 0.0) {
        r = (int)(x + 0.5);
    } else {
        r = (int)(x - 0.5);
    }
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
        if (f < 1.0) {
            precision = 6;
        } else if (f < 10.0) {
            precision = 5;
        } else if (f < 100.0) {
            precision = 4;
        } else if (f < 1000.0) {
            precision = 3;
        } else if (f < 10000.0) {
            precision = 2;
        } else if (f < 100000.0) {
            precision = 1;
        } else {
            precision = 0;
        }
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
#if __ROUND_BUILTIN_FOLDS && __has_builtin(__builtin_ceilf)
    float r = __builtin_ceilf(x);
#else
    float fract = x - (float)(int)x;
    float r     = (fract > 0) ? (float)((int)x + 1) : (float)(int)x;
#endif
    kernel_fpu_end();
    return r;
}

/* Return the largest integer value less than or equal to the argument */
float floorf(float x)
{
    if (!kernel_sse_available()) return 0.0f;
    kernel_fpu_begin();
#if __ROUND_BUILTIN_FOLDS && __has_builtin(__builtin_floorf)
    float r = __builtin_floorf(x);
#else
    float fract = x - (float)(int)x;
    float r     = (fract < 0) ? (float)((int)x - 1) : (float)(int)x;
#endif
    kernel_fpu_end();
    return r;
}

/* Round a floating-point number to the nearest integer */
float roundf(float number)
{
    if (!kernel_sse_available()) return 0.0f;
    kernel_fpu_begin();
    float r;
    if (number < 0.0f) {
        r = ceilf(number - 0.5f);
    } else {
        r = floorf(number + 0.5f);
    }
    kernel_fpu_end();
    return r;
}

/* Return the absolute value of a double */
double fabs(double x)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
#if __has_builtin(__builtin_fabs)
    double r = __builtin_fabs(x);
#else
    double r = (x < 0) ? -x : x;
#endif
    kernel_fpu_end();
    return r;
}

/* Return the largest integer less than or equal to x */
double floor(double x)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
#if __ROUND_BUILTIN_FOLDS && __has_builtin(__builtin_floor)
    double r = __builtin_floor(x);
#else
    double fract = x - (int)x;
    double r     = (fract < 0) ? (int)x - 1 : (int)x;
#endif
    kernel_fpu_end();
    return r;
}

/* Return the smallest integer greater than or equal to x */
double ceil(double x)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
#if __ROUND_BUILTIN_FOLDS && __has_builtin(__builtin_ceil)
    double r = __builtin_ceil(x);
#else
    double fract = x - (int)x;
    double r     = (fract > 0) ? (int)x + 1 : (int)x;
#endif
    kernel_fpu_end();
    return r;
}

/* Bit pattern of a double. */
static uint64_t double_bits(double x)
{
    union {
            double   value;
            uint64_t bits;
    } v = {x};
    return v.bits;
}

/* True when x is a NaN */
int isnan(double x)
{
    if (!kernel_sse_available()) return 0;
    kernel_fpu_begin();
#if __has_builtin(__builtin_isnan)
    int r = __builtin_isnan(x);
#else
    int r = (double_bits(x) & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL;
#endif
    kernel_fpu_end();
    return r;
}

/* True when x is an infinity */
int isinf(double x)
{
    if (!kernel_sse_available()) return 0;
    kernel_fpu_begin();
#if __has_builtin(__builtin_isinf)
    int r = __builtin_isinf(x);
#else
    int r = (double_bits(x) & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
#endif
    kernel_fpu_end();
    return r;
}

/* True when x is neither a NaN nor an infinity */
int isfinite(double x)
{
    if (!kernel_sse_available()) return 0;
    kernel_fpu_begin();
#if __has_builtin(__builtin_isfinite)
    int r = __builtin_isfinite(x);
#else
    int r = (double_bits(x) & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
#endif
    kernel_fpu_end();
    return r;
}

/* True when x is a normal value (not zero, subnormal, infinite or NaN) */
int isnormal(double x)
{
    if (!kernel_sse_available()) return 0;
    kernel_fpu_begin();
#if __has_builtin(__builtin_isnormal)
    int r = __builtin_isnormal(x);
#else
    uint64_t exponent = (double_bits(x) >> 52) & 0x7FFU;
    int      r        = exponent != 0 && exponent != 0x7FFU;
#endif
    kernel_fpu_end();
    return r;
}

/* True when the sign bit of x is set */
int signbit(double x)
{
    if (!kernel_sse_available()) return 0;
    kernel_fpu_begin();
#if __has_builtin(__builtin_signbit)
    int r = __builtin_signbit(x) != 0;
#else
    int r = (double_bits(x) >> 63) != 0;
#endif
    kernel_fpu_end();
    return r;
}

/* Classify x into one of the FP_* categories */
int fpclassify(double x)
{
    if (!kernel_sse_available()) return FP_ZERO;
    kernel_fpu_begin();
#if __has_builtin(__builtin_fpclassify)
    int r = __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x);
#else
    uint64_t bits     = double_bits(x);
    uint64_t exponent = (bits >> 52) & 0x7FFU;
    int      r;
    if (exponent == 0x7FFU) {
        r = (bits & 0x000FFFFFFFFFFFFFULL) ? FP_NAN : FP_INFINITE;
    } else if (exponent == 0) {
        r = (bits & 0x000FFFFFFFFFFFFFULL) ? FP_SUBNORMAL : FP_ZERO;
    } else {
        r = FP_NORMAL;
    }
#endif
    kernel_fpu_end();
    return r;
}

/* Return a quiet NaN */
float nanf(const char *tagp)
{
    if (!kernel_sse_available()) return 0.0f;
    kernel_fpu_begin();
    (void)tagp; // the kernel produces one quiet NaN, so the payload tag is ignored.
#if __has_builtin(__builtin_nanf)
    float r = __builtin_nanf("");
#else
    union {
            uint32_t bits;
            float    value;
    } v     = {0x7FC00000U};
    float r = v.value;
#endif
    kernel_fpu_end();
    return r;
}

/* Return a quiet NaN as a double */
double nan(const char *tagp)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();
    double r = (double)nanf(tagp);
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

    if (y == 0.0 || isnan(x) || isnan(y) || isinf(x)) {
        r = nanf("");
    } else if (isinf(y) || ax < ay) {
        r = x; // fmod(x, +-Inf) == x, and |x| < |y| gives x
    } else {
        /*
         * Scale |y| up to the magnitude of |x|, then subtract it back down
         * in a binary long division.  This never truncates x/y through an
         * integer, so the quotient cannot overflow.
         */
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
    if (isnan(x) || isinf(x)) {
        kernel_fpu_end();
        return nanf("");
    }

    /*
     * Reduce x into [-pi, pi] so the Taylor series converges quickly.
     * fmod() performs the reduction without truncating x / 2pi through an
     * integer, so it stays correct for large arguments.
     */
    const double pi     = 3.14159265358979323846;
    const double two_pi = 2.0 * pi;
    x                   = fmod(x, two_pi);
    if (x > pi) {
        x -= two_pi;
    } else if (x < -pi) {
        x += two_pi;
    }

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
        r = nanf("");
    } else if (number == 0.0) {
        r = 0.0;
    } else {
#if defined(__NO_MATH_ERRNO__) && __has_builtin(__builtin_sqrt)
        r = __builtin_sqrt(number);
#else
        double x       = number;
        double epsilon = 1e-15;
        double diff;

        do {
            x    = (x + number / x) / 2;
            diff = fabs(x - (number / x));
        } while (diff > epsilon);
        r = x;
#endif
    }
    kernel_fpu_end();
    return r;
}

/* Calculate the arc cosine (inverse cosine) of x, in [0, pi] */
double acos(double x)
{
    if (!kernel_sse_available()) return 0.0;
    kernel_fpu_begin();

    /* acos is only defined on [-1, 1]; the exact endpoints are handled explicitly because acos is ill-conditioned there. */
    const double pi = 3.14159265358979323846;
    if (isnan(x) || x > 1.0 || x < -1.0) {
        kernel_fpu_end();
        return nanf("");
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
        double next = theta + ((c - x) / s);
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

#ifdef __clang__
#    pragma clang attribute pop
#endif
#if defined(__GNUC__) && !defined(__clang__)
#    pragma GCC reset_options // drop the file-wide SSE2 target for pure-integer code
#endif

/* Return the absolute value of an integer */
int abs(int x)
{
#if __has_builtin(__builtin_abs)
    return __builtin_abs(x);
#else
    return (x < 0 ? -x : x);
#endif
}
