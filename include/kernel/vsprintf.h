/*
 *
 *      vsprintf.h
 *      Kernel formatted string output header file
 *
 *      2024/6/27 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VSPRINTF_H_
#define INCLUDE_VSPRINTF_H_

#include <kernel/writer.h>
#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

typedef struct {
        char  *buf;
        size_t idx;
        size_t size; // Buffer size for safe writing, 0 means unlimited
} unsafe_buf_data;

typedef struct {
        const char **fmt_ptr;       // a pointer to `fmt`
        size_t      *write_counter; // for `%n`
} args_fmter;

/* Handler of unsafe buf writing */
uint8_t unsafe_buf_write(writer *writer, char c);

/* Handler of safe buf writing with size limit */
uint8_t unsafe_buf_write_safe(writer *writer, char c);

/* Store the formatted output in a character array */
__attribute__((format(printf, 2, 3))) int sprintf(char *str, const char *fmt, ...);

/* Store the formatted output in a character array with size limit */
__attribute__((format(printf, 3, 4))) int snprintf(char *str, size_t size, const char *fmt, ...);

/* Format with va_list, then store the formatted output in a character array */
__attribute__((format(printf, 2, 0))) int vsprintf(char *str, const char *fmt, va_list args);

/* Format with va_list, then store the formatted output in a character array with size limit */
__attribute__((format(printf, 3, 0))) int vsnprintf(char *str, size_t size, const char *fmt, va_list args);

/* Formatted output processing */
void wfmt_arg(writer *writer, args_fmter *fmter, va_list args);

/* Use a `writer` to write formatted string */
__attribute__((format(printf, 2, 0))) size_t vwprintf(writer *writer, const char *fmt, va_list args);

#endif // INCLUDE_VSPRINTF_H_
