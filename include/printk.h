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

enum OverflowKind {
    OFLOW_AT_FMTARG,
    OFLOW_AT_FMTSTR,
};

typedef struct FmtArg {
        uint64_t size;    // The size of the buff to write
        char *buff;       // The buff to write
        char *last_write; // The last write position
} FmtArg;

typedef struct OverflowSignal {
        enum OverflowKind kind; // The kind of overflow
        struct FmtArg *arg;     // The argument that overflow
} OverflowSignal;

/* Kernel print string */
void printk(const char *format, ...);

/* Kernel print string (it means don't check the buffer overflow) */
void printk_unsafe(const char *format, ...);

/* Kernel print log */
void plogk(const char *format, ...);

/* Store the formatted output in a character array */
int sprintf(char *str, const char *fmt, ...);

/* Format a string and output it to a character array */
int vsprintf(char *buff, const char *format, va_list args);

/* Release the memory used by the FmtArg structure */
void free_fmtarg(FmtArg *arg);

/* Create a new FmtArg structure and initialize it */
FmtArg *new_fmtarg(uint64_t size, char *buff, char *last_write);

/* Parse the format string and read the corresponding variadic parameters to generate an FmtArg structure */
FmtArg *read_fmtarg(const char **format, va_list args);

/* Create a new OverflowSignal structure */
OverflowSignal *new_overflow(enum OverflowKind kind, FmtArg *arg);

/* Format a string with size and output it to a character array */
OverflowSignal *vsprintf_s(OverflowSignal *signal, char *buff, intptr_t size, const char **format, va_list args);

#endif // INCLUDE_PRINTK_H_
