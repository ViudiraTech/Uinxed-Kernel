/*
 *
 *      stdlib.c
 *      Standard library
 *
 *      2024/10/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <heap.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Standardized file paths */
char *normalize_path(const char *path)
{
    if (!path) return 0;

    size_t len    = strlen(path);
    char  *result = malloc(len + 1);
    if (!result) return 0;

    char *dup = strdup(path);
    if (!dup) {
        free(result);
        return 0;
    }

    strcpy(result, "/");
    if (!strcmp(path, "/")) {
        free(dup);
        return result;
    }

    char *start = dup;
    if (*start == '/') start++;

    char *token = strtok(start, "/");
    while (token) {
        if (strcmp(token, ".") == 0) {
            /* Ignore the current directory */
        } else if (strcmp(token, "..") == 0) {
            char *last_slash = strrchr(result, '/');
            if (last_slash != result) {
                *last_slash = '\0';
            } else {
                result[1] = '\0';
            }
        } else {
            if (result[strlen(result) - 1] != '/') strcat(result, "/");
            strcat(result, token);
        }
        token = strtok(0, "/");
    }
    free(dup);
    return result;
}

/* Write a formatted number to a writer */
size_t wnumber(writer *writer, num_formatter_t fmter, num_fmt_type type)
{
    char        c = 0;
    char        tmp[65];
    int         sign      = 0;
    const char *digits    = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int         i         = 0; // index for tmp
    int64_t     size      = (int64_t)fmter.size;
    int64_t     precision = (int64_t)fmter.precision;
    size_t      base      = fmter.base;

    write_handler write  = writer->handler;
    size_t        result = 0;

    if (type.small) digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (type.left) type.zeropad = 0;     // if left adjust, zero padding is not allowed
    if (base < 2 || base > 36) return 0; // Invalid base

    c = (type.zeropad) ? '0' : ' ';

    /* Check sign */
    if (type.sign && (int64_t)fmter.num < 0) {
        sign      = '-';
        fmter.num = -(int64_t)fmter.num;
    } else if (type.plus) {
        sign = '+';
    } else if (type.space) {
        sign = ' ';
    } else {
        sign = 0;
    }
    if (sign) size--;

    /* Special like 0x, 0 */
    if (type.special) {
        if (base == 16) {
            size -= 2;
        } else if (base == 8) {
            size--;
        }
    }

    i = 0;

    if (!(fmter.num)) {
        tmp[i++] = '0';
    } else {
        while (fmter.num) {
            tmp[i++]  = digits[(uint64_t)fmter.num % (uint64_t)base];
            fmter.num = (uint64_t)fmter.num / (uint64_t)fmter.base;
        }
    }
    if (i > precision) precision = i; // precision = max(precision, i);

    size -= precision;

    /* If type no include LEFT or ZEROPAD */
    if (!(type.zeropad || type.left)) {
        /* Fill in the space */
        while (size-- > 0) write(writer, ' '), result++;
    }

    /* Write the sign */
    if (sign) write(writer, (char)sign), result++;

    /* Write the prefix */
    if (type.special) {
        if (base == 8) {
            write(writer, '0'), result++;
        } else if (base == 16) {
            write(writer, '0'), result++;
            write(writer, digits[33]), result++; // 33 is 'x' or 'X'
        }
    }

    if (!(type.left)) {
        /* Write the padding */
        while (size-- > 0) write(writer, c), result++;
    }

    /* Write the zero padding */
    while (i < precision--) write(writer, '0'), result++;

    /* Write the number */
    while (i-- > 0) write(writer, tmp[i]), result++;

    /* LEFT adjust */
    while (size-- > 0) write(writer, ' '), result++;
    return result;
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
    while (IS_DIGIT(**s)) i = i * 10 + *((*s)++) - '0';
    return i;
}

/* Formatting an integer as a string */
char *number(char *str, size_t num, size_t base, size_t size, size_t precision, int type)
{
    char        c, tmp[65];
    int         sign;
    const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int         i;
    int64_t     size_      = (int64_t)size;
    int64_t     precision_ = (int64_t)precision;

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
uint64_t number_length(size_t num, size_t base, size_t size, size_t precision, int type)
{
    /* This function is for malloc a enough space for `number()` */
    char     sign          = 0; // is there a sign (0: no sign, 1: sign)
    size_t   number_digits = 0;
    uint64_t res           = 0;
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
