/*
 *
 *      stdlib.h
 *      Standard library header file
 *
 *      2024/10/2 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_STDLIB_H_
#define INCLUDE_STDLIB_H_

#include "alloc.h"
#include "stddef.h"
#include "stdint.h"

#define ZEROPAD 1  // pad with zero
#define SIGN    2  // unsigned/signed long
#define PLUS    4  // show plus
#define SPACE   8  // space if plus
#define LEFT    16 // left justified
#define SPECIAL 32 // 0x
#define SMALL   64 // use 'abcdef' instead of 'ABCDEF'

#define do_div(n, base)                                                                   \
    ({                                                                                    \
        int64_t __res;                                                                    \
        __asm__("divq %4" : "=a"(n), "=d"(__res) : "0"(n), "1"(0), "r"((int64_t)(base))); \
        __res;                                                                            \
    })

/* Determine whether it is a number */
int is_digit(int c);

/* Convert a string number to an integer number */
int atoi(const char *pstr);

/* Skip numbers in a string and return the value of those consecutive numbers */
int skip_atoi(const char **s);

/* Formatting an integer as a string */
char *number(char *str, size_t num, size_t base, size_t size, size_t precision, int type);

/* Returns the size of a string with an integer formatted by `number()` */
uint64_t number_length(size_t num, size_t base, size_t size, size_t precision, int type);

#endif // INCLUDE_STDLIB_H_
