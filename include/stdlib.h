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
#define SIGN 2     // unsigned/signed long
#define PLUS 4     // show plus
#define SPACE 8    // space if plus
#define LEFT 16    // left justified
#define SPECIAL 32 // 0x
#define SMALL 64   // use 'abcdef' instead of 'ABCDEF'

#define ALIGN_DOWN(addr, align) ((addr) & ~((align)-1))
#define ALIGN_UP(addr, align) (((addr) + (align)-1) & ~((align)-1))

#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')

#define do_div(n, base)                                                        \
  ({                                                                           \
    int64_t __res;                                                             \
    __asm__("divq %4"                                                          \
            : "=a"(n), "=d"(__res)                                             \
            : "0"(n), "1"(0), "r"((int64_t)(base)));                           \
    __res;                                                                     \
  })

/* Convert a string number to an integer number */
int atoi(const char *pstr);

/* Skip numbers in a string and return the value of those consecutive numbers */
int skip_atoi(const char **s);

/* Formatting an integer as a string */
char *number(char *str, size_t num, size_t base, size_t size, size_t precision,
             int type);

/* Returns the size of a string with an integer formatted by `number()` */
uint64_t number_length(size_t num, size_t base, size_t size, size_t precision,
                       int type);

typedef struct _klist {
  struct _klist *pre, *next; //前后指针双向列表
  void *data;                //数据指针
} list_t;

#endif // INCLUDE_STDLIB_H_
