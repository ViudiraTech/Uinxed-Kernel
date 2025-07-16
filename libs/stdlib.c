/*
 *
 *      stdlib.c
 *      Standard library
 *
 *      2024/10/2 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "stdlib.h"
#include "stdint.h"

/* Determine whether it is a number */
int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* Convert a string number to an integer number */
int atoi(const char *pstr)
{
    int ret_integer  = 0;
    int integer_sign = 1;

    if (*pstr == '-') integer_sign = -1;
    if (*pstr == '-' || *pstr == '+') pstr++;

    while (*pstr >= '0' && *pstr <= '9') {
        ret_integer = ret_integer * 10 + *pstr - '0';
        pstr++;
    }
    ret_integer = integer_sign * ret_integer;
    return ret_integer;
}

/* Skip numbers in a string and return the value of those consecutive numbers */
int skip_atoi(const char **s)
{
    int i = 0;
    while (is_digit(**s)) i = i * 10 + *((*s)++) - '0';
    return i;
}

/* Formatting an integer as a string */
char *number(char *str, size_t num, size_t base, size_t size, size_t precision, int type) // NOLINT
{
    char c, tmp[65];
    int sign;
    const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int i;
    int64_t size_      = (int64_t)size;
    int64_t precision_ = (int64_t)precision;

    if (type & SMALL) digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (type & LEFT) type &= ~ZEROPAD;   // if left adjust, zero padding is not allowed
    if (base < 2 || base > 36) return 0; // Invalid base

    c = (type & ZEROPAD) ? '0' : ' ';

    /* Check sign */
    if (type & SIGN && (int64_t)num < 0) {
        sign = '-';
        num  = -(int64_t)num;
    } else {
        sign = (type & PLUS) ? '+' : ((type & SPACE) ? ' ' : 0);
    }
    if (sign) size_--;

    /* Special like 0x, 0 */
    if (type & SPECIAL) {
        if (base == 16) {
            size_ -= 2;
        } else if (base == 8) {
            size_--;
        }
    }

    i = 0;
    if (num == 0) {
        tmp[i++] = '0';
    } else {
        while (num != 0) {
            tmp[i++] = digits[(uint64_t)num % (uint64_t)base];
            num      = (uint64_t)num / (uint64_t)base;
        }
    }
    if (i > precision_) precision_ = i;
    size_ -= precision_;

    /* If type no include LEFT or ZEROPAD */
    if (!(type & (ZEROPAD + LEFT))) {
        /* Fill in the space */
        while (size_-- > 0) *str++ = ' ';
    }

    /* Write the sign */
    if (sign) *str++ = (char)sign;

    /* Write the prefix */
    if (type & SPECIAL) {
        if (base == 8) {
            *str++ = '0';
        } else if (base == 16) {
            *str++ = '0';
            *str++ = digits[33]; // 33 is 'x' or 'X'
        }
    }

    if (!(type & LEFT)) {
        /* Write the padding */
        while (size_-- > 0) *str++ = c;
    }

    /* Write the zero padding */
    while (i < precision_--) *str++ = '0';

    /* Write the number */
    while (i-- > 0) *str++ = tmp[i];

    /* LEFT adjust */
    while (size_-- > 0) *str++ = ' ';
    return str;
}

/* Returns the size of a string with an integer formatted by `number()` */
uint64_t number_length(size_t num, size_t base, size_t size, size_t precision, int type) // NOLINT
{
    /* This function is for malloc a enough space for `number()` */
    char sign            = 0; // is there a sign (0: no sign, 1: sign)
    size_t number_digits = 0;
    uint64_t res         = 0;
    if ((type & SIGN && (int64_t)num < 0)) {
        num  = -(int64_t)num;
        sign = 1;
    }
    if (type & PLUS || type & SPACE) sign = 1;
    if (num == 0) {
        number_digits = 1;
    } else {
        while (num != 0) {
            number_digits++;
            num = (uint64_t)num / (uint64_t)base;
        }
    }
    if (type & SPECIAL) {
        if (base == 16) {
            res += 2;
        } else if (base == 8) {
            res += 1;
        }
    }
    if (precision > size) size = precision;

    if (number_digits + sign < size) {
        res += size;
        return res;
    }
    res += number_digits + sign;
    return res;
};
