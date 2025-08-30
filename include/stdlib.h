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

#include "stddef.h"
#include "stdint.h"

#define ZEROPAD 1  // pad with zero
#define SIGN    2  // unsigned/signed long
#define PLUS    4  // show plus
#define SPACE   8  // space if plus
#define LEFT    16 // left justified
#define SPECIAL 32 // 0x
#define SMALL   64 // use 'abcdef' instead of 'ABCDEF'

#define ALIGN_DOWN(addr, align) ((addr) & ~((align) - 1))
#define ALIGN_UP(addr, align)   (((addr) + (align) - 1) & ~((align) - 1))

#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define IS_ALPHA(a) (((a) >= 'A' && (a) <= 'Z') || ((a) >= 'a' && (a) <= 'z'))

#define do_div(n, base)                                                                   \
    ({                                                                                    \
        int64_t __res;                                                                    \
        __asm__("divq %4" : "=a"(n), "=d"(__res) : "0"(n), "1"(0), "r"((int64_t)(base))); \
        __res;                                                                            \
    })

/* BEGIN TODO BLOCK: Move these to a I/O header file */
/* Placeholder ... */
struct Writer;

/**
 * A handle of writing a char
 * `uint8_t` is a bool, if != 0 means write success, if == 0 means write failure
 */
typedef uint8_t (*WriteHandler)(struct Writer *writer, char ch);

/* A interface of writing a char (May be extended in the future)*/
typedef struct Writer {
        void        *data; /* Any data */
        WriteHandler handler;
} Writer;
/* END TODO BLOCK */

typedef struct num_fmt_type {
        uint8_t zeropad : 1; // Padding with zero
        uint8_t sign    : 1; // If signed
        uint8_t plus    : 1; // Show plus sign
        uint8_t space   : 1; // Show space if not negative
        uint8_t left    : 1; // Left align
        uint8_t special : 1; // Special prefix (e.g. 0x)
        uint8_t small   : 1; // Use lowercase letters (0X -> 0x, 1F -> 1f)
} num_fmt_type;

typedef struct num_formatter {
        size_t num;       // Number
        size_t base;      // Base (e.g. 10, 16, 8, 2)
        size_t size;      // Minimum field width
        size_t precision; // Precision (In integer, it's seems like ZEROPAD)
} num_formatter_t;

/* Write a formatted number to a writer */
size_t wnumber(Writer *writer, num_formatter_t fmter, num_fmt_type type);

/* Convert a string number to an integer number */
int atoi(const char *pstr);

/* Skip numbers in a string and return the value of those consecutive numbers */
int skip_atoi(const char **s);

/* Formatting an integer as a string */
char *number(char *str, size_t num, size_t base, size_t size, size_t precision, int type);

/* Returns the size of a string with an integer formatted by `number()` */
uint64_t number_length(size_t num, size_t base, size_t size, size_t precision, int type);

#endif // INCLUDE_STDLIB_H_
