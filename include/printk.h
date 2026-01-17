/*
 *
 *      printk.h
 *      Kernel string print header file
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PRINTK_H_
#define INCLUDE_PRINTK_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

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

typedef struct {
        char  *buf;
        size_t idx;
} unsafe_buf_data;

typedef struct {
        const char **fmt_ptr;       // a pointer to `fmt`
        size_t      *write_counter; // for `%n`
} args_fmter;

/* Kernel print string */
void printk(const char *format, ...);

/* Kernel print log */
void plogk(const char *format, ...);

/* Handler of unsafe buf writing */
uint8_t unsafe_buf_write(writer *writer, char c);

/* Store the formatted output in a character array */
int sprintf(char *str, const char *fmt, ...);

/* Format with va_list, then store the formatted output in a character array */
int vsprintf(char *str, const char *fmt, va_list args);

/* Formatted output processing */
void wfmt_arg(writer *writer, args_fmter *fmter, va_list args);

/* Use a `writer` to write formatted string */
size_t vwprintf(writer *writer, const char *fmt, va_list args);

#endif // INCLUDE_PRINTK_H_
