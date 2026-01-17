/*
 *
 *      math.c
 *      Mathematical library
 *
 *      2025/10/7 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <math.h>

/* Round a floating-point number to the nearest integer */
int round(float64_t x)
{
    if (x >= 0.0) return (int)(x + 0.5);
    return (int)(x - 0.5);
}

/* Convert a float to a string with a specified precision */
char *ftoa(double f, char *buf, int precision)
{
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
    return ptr;
}

/* Return the smallest integer value greater than or equal to the argument */
float ceilf(float x)
{
    float fract = x - (float)(int)x;
    if (fract > 0) return (float)((int)x + 1);
    return (float)(int)x;
}

/* Return the largest integer value less than or equal to the argument */
float floorf(float x)
{
    float fract = x - (float)(int)x;
    if (fract < 0) return (float)((int)x - 1);
    return (float)(int)x;
}

/* Round a floating-point number to the nearest integer */
float roundf(float number)
{
    if (number >= 0.0f) return (float)(int)(number + 0.5f);
    return (float)(int)(number - 0.5f);
}

/* Return the absolute value of a double */
double fabs(double x)
{
    if (x < 0) return -x;
    return x;
}

/* Return the largest integer less than or equal to x */
double floor(double x)
{
    double i = (double)((long long)x);
    if (x < 0 && x != i) return i - 1.0;
    return i;
}

/* Return the smallest integer greater than or equal to x */
double ceil(double x)
{
    double fract = x - (int)x;
    if (fract > 0) return (int)x + 1;
    return (int)x;
}

/* Return the remainder of x divided by y */
double fmod(double x, double y)
{
    if (y == 0.0) return __builtin_nanf("");
    long quotient = (long)(x / y);
    return x - (double)quotient * y;
}

/* Calculate the cosine of x (in radians) */
double cos(double x)
{
    if (x < 0) x = -x;
    x = fmod(x, 2.0 * PI);

    double sum  = 1.0;
    double term = 1.0;
    double x_sq = x * x;

    for (int i = 1; i <= 12; i++) {
        term *= -x_sq / ((2.0 * i - 1.0) * (2.0 * i));
        sum += term;

        double abs_term = (term < 0) ? -term : term;
        if (abs_term < 1e-15) break;
    }
    return sum;
}

/* Calculate the square root of a number */
double sqrt(double number)
{
    if (number < 0) return __builtin_nanf("");
    if (number == 0) return 0;

    double x = number;
    double last_x;

    for (int i = 0; i < 100; i++) {
        last_x = x;
        x      = (x + number / x) * 0.5;
        if (fabs(x - last_x) < 1e-15) break;
    }
    return x;
}

/* Calculate the arc cosine (inverse cosine) of x */
double acos(double x)
{
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
    return x1;
}

/* Calculate x raised to the power of y */
double pow(double x, int y)
{
    double result = 1.0;
    for (int i = 0; i < y; i++) result *= x;
    return result;
}

/* Multiply x by 2 raised to the power of exp */
double ldexp(double x, int exp)
{
    double res = x;
    if (exp > 0)
        while (exp--) res *= 2.0;
    else
        while (exp++) res /= 2.0;
    return res;
}

/* Return the absolute value of an integer */
int abs(int x)
{
    return (x < 0 ? -x : x);
}
