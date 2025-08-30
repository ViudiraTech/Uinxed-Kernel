/*
 *
 *      printk.h
 *      Kernel string print header file
 *
 *      2024/6/27 By Rainy101112
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_PRINTK_H_
#define INCLUDE_PRINTK_H_

#include "stdarg.h"
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"

#ifndef KERNEL_LOG
#    define KERNEL_LOG 1
#endif

typedef enum {
    OFLOW_AT_FMTARG,
    OFLOW_AT_FMTSTR,
} overflow_kind_t;

typedef struct {
        uint64_t size;       // The size of the buff to write
        char    *buff;       // The buff to write
        char    *last_write; // The last write position
} fmt_arg_t;

typedef struct {
        overflow_kind_t kind; // The kind of overflow
        fmt_arg_t      *arg;  // The argument that overflow
} overflow_signal_t;

/* Kernel print string */
void printk(const char *format, ...);

/* Kernel print log */
void plogk(const char *format, ...);

/* Store the formatted output in a character array */
int sprintf(char *str, const char *fmt, ...);

/* Format with va_list, then store the formatted output in a character array */
int vsprintf(char *str, const char *fmt, va_list args);

/* Use a `writer` to write formatted string */
size_t vwprintf(Writer *writer, const char *fmt, va_list args);

#endif // INCLUDE_PRINTK_H_
