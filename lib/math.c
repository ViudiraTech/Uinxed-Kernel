/*
 *
 *		math.c
 *		数学处理的内联函数库
 *
 *		2024/10/2 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "math.h"
#include "types.h"
#include "stdlib.h"
#include "string.h"

/* sin运算 */
double sin(double x)
{
	asm volatile("fldl %0 \n"
                 "fsin \n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

/* cos运算 */
double cos(double x)
{
	asm volatile("fldl %0 \n"
                 "fcos \n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

/* tan运算 */
double tan(double x)
{
	asm volatile("fldl %0 \n"
                 "fptan \n"
                 "fstpl %0\n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

/* sqrt运算 */
double sqrt(double x)
{
	asm volatile("fldl %0 \n"
                 "fsqrt \n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

/*
 * @attention Rainy101112
 * These functions have been create by AI, may cause some bugs.
 */

double sinh(double x)
{
    double e_x = exp(x); 
    double e_neg_x = exp(-x);
    return (e_x - e_neg_x) / 2.0;
}

double cosh(double x)
{
    double e_x = exp(x);
    double e_neg_x = exp(-x);
    return (e_x + e_neg_x) / 2.0;
}

double tanh(double x)
{
    double e_x = exp(x);
    double e_neg_x = exp(-x);
    return (e_x - e_neg_x) / (e_x + e_neg_x);
}

double asin(double x)
{
    if (x < -1 || x > 1) {
        return NAN;
    }
    double y = x;
    double delta;
    const double tolerance = 1e-7;
    const double pi = 3.14159265358979323846;
    const double pi_over_2 = pi / 2;
    while (1) {
        double y1 = (1 - y * y) > 0 ? sqrt(1 - y * y) : 0;
        delta = (x - y / y1) / (1 + y * y / (y1 * y1));
        y += delta;

        if (fabs(delta) < tolerance) {
            break;
        }
    }
    return y + (x < 0 ? -pi_over_2 : pi_over_2);
}

double acos(double x)
{
    double sqrtPart = sqrt(1 - x * x);
    double atanPart;
    if (x >= 0) {
        atanPart = atan(sqrtPart / x);
    } else {
        atanPart = atan(sqrtPart / -x);
        atanPart = M_PI - atanPart;
    }
    return M_PI_2 - atanPart;
}

double atan(double x)
{
    double term = x;
    double sum = 0.0;
    int n = 1;
    while (fabs(term) > 1e-7) {
        sum += term;
        term = -term * x * x / (2 * n * (2 * n + 1));
        n += 1;
    }
    return sum;
}

double atan2(double y, double x)
{
    double result;
    if (x == 0) {
        if (y > 0) {
            result = M_PI_2;
        } else if (y < 0) {
            result = -M_PI_2;
        } else {
            return NAN;
        }
    } else if (x > 0) {
        result = atan(y / x);
    } else {
        if (y >= 0) {
            result = atan(y / x) + M_PI;
        } else {
            result = atan(y / x) - M_PI;
        }
    }
    return result;
}

double exp(double x)
{
    double sum = 1.0;
    double term = 1.0;
    int i = 1;
    do {
        term *= x / i;
        sum += term;
        i++;
    } while (term > 1e-7);
    return sum;
}

double log(double x)
{
    if (x <= 0) {
        return NAN;
    }
    double sum = 0.0;
    double term = 1.0;
    int i = 1;
    while (term > 1e-15) {
        sum += term;
        term *= (x - 1.0) / i;
        i++;
    }
    return sum;
}

double log10(double x)
{
    return log(x) / log(10);
}

double pow(double x, double y)
{
    double result = 1.0;
    int i;
    if (y < 0) {
        x = 1 / x;
        y = -y;
    }
    for (i = 0; i < y; i++) {
        result *= x;
    }
    return result;
}
double ceil(double x)
{
    if (x > 0) {
        return ceil(x);
    } else {
        return -floor(-x);
    }
}

double floor(double x)
{
    union {
        double d;
        uint64_t ui;
    } u = {x};
    int sign = u.ui >> 63;
    int exponent = (int)((u.ui >> 52) & 0x7ff) - 0x3ff;
    uint64_t mantissa = u.ui & 0xfffffffffffff;
    if (exponent < 0) {
        return sign ? -1 : 0;
    } else if (exponent == 0) {
        return (sign ? -mantissa : mantissa);
    } else {
        uint64_t mask = 1ULL << (52 + exponent);
        mantissa |= mask;
        u.ui = (u.ui & ((mask << 1) - 1)) | (sign ? (mask - 1) : 0);
        return u.d;
    }
}

float fabsf(float x)
{
    return (x < 0.0f) ? -x : x;
}

long double fabsl(long double x)
{
    return (x < 0.0L) ? -x : x;
}

double fabs(double x)
{
    return (x < 0.0) ? -x : x;
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

long labs(long x)
{
    return x < 0 ? -x : x;
}

double atof(const char *str)
{
    double result = 0.0;
    double factor = 1.0;
    int sign = 1;
    const char *p = str;

    while (isspace((unsigned char)*p)) p++;

    if (*p == '+' || *p == '-') {
        sign = (*p == '-') ? -1 : 1;
        p++;
    }
    while (isdigit((unsigned char)*p)) {
        result = result * 10.0 + (*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        double decimal = 0.1;
        while (isdigit((unsigned char)*p)) {
            result += (*p - '0') * decimal;
            decimal *= 0.1;
            p++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        int expSign = 1;
        if (*p == '+' || *p == '-') {
            expSign = (*p == '-') ? -1 : 1;
            p++;
        }
        int exponent = 0;
        while (isdigit((unsigned char)*p)) {
            exponent = exponent * 10 + (*p - '0');
            p++;
        }
        factor = expSign == 1 ? pow(10.0, exponent) : pow(10.0, -exponent);
    }
    return sign * result * factor;
}

/* log2运算 */
double log2(double x)
{
	asm volatile("fld1 \n"
                 "fldl %0 \n"
                 "fyl2x \n"
                 "fwait \n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

double nan(const char *tagp)
{
    uint64_t nan_bits = 0x7ff8000000000000ULL;
    return *(double *)&nan_bits;
}

float nanf(const char *tagp)
{
    uint32_t nan_bits = 0x7FC00000;
    float result;
    memcpy((uint8_t*)&result, (uint8_t*)&nan_bits, sizeof(result));
    return result;
}

long double nanl(const char *tagp)
{
    uint64_t nan_bits_high = 0x7FF8000000000000ULL;
    uint64_t nan_bits_low = 0;
    long double result;
    memcpy((uint8_t*)&result, (uint8_t*)&nan_bits_high, sizeof(nan_bits_high));
    memcpy((char *)&result + sizeof(nan_bits_high), (uint8_t*)&nan_bits_low, sizeof(nan_bits_low));
    return result;
}
