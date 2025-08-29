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
#include "stdint.h"

#ifndef KERNEL_LOG
#define KERNEL_LOG 1
#endif

typedef enum {
  OFLOW_AT_FMTARG,
  OFLOW_AT_FMTSTR,
} overflow_kind_t;

typedef struct {
  uint64_t size;    // The size of the buff to write
  char *buff;       // The buff to write
  char *last_write; // The last write position
} fmt_arg_t;

typedef struct {
  overflow_kind_t kind; // The kind of overflow
  fmt_arg_t *arg;       // The argument that overflow
} overflow_signal_t;

/* Kernel print string */
void printk(const char *format, ...);

/* Kernel print string (it means don't check the buffer overflow) */
void printk_unsafe(const char *format, ...);

/* Kernel print log (it means don't check the buffer overflow) */
void plogk_unsafe(const char *format, ...);

/* Kernel print log */
void plogk(const char *format, ...);

/* Store the formatted output in a character array */
int sprintf(char *str, const char *fmt, ...);

/* Format a string and output it to a character array */
int vsprintf(char *buff, const char *format, va_list args);

/* Release the memory used by the fmt_arg_t structure */
void free_fmtarg(fmt_arg_t *arg);

/* Create a new fmt_arg_t structure and initialize it */
fmt_arg_t *new_fmtarg(uint64_t size, char *buff, char *last_write);

/* Parse the format string and read the corresponding variadic parameters to
 * generate an fmt_arg_t structure */
fmt_arg_t *read_fmtarg(const char **format, va_list args);

/* Create a new overflow_signal_t structure */
overflow_signal_t *new_overflow(overflow_kind_t kind, fmt_arg_t *arg);

/* Format a string with size and output it to a character array */
overflow_signal_t *vsprintf_s(overflow_signal_t *signal, char *buff,
                              intptr_t size, const char **format, va_list args);

#endif // INCLUDE_PRINTK_H_
