/*
 *
 *      debug.h
 *      Kernel debug header files
 *
 *      2024/6/27 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DEBUG_H_
#define INCLUDE_DEBUG_H_

/* do-while keeps a caller's `else` bound to its own `if` */
#define assert(exp)                                              \
    do {                                                         \
        if (!(exp)) assertion_failure(#exp, __FILE__, __LINE__); \
    } while (0)

/* if the stack carries an error code, set this variable to 1 before calling panic */
extern int carry_error_code;

/* Dump stack */
void dump_stack(void);

/* Kernel panic (never returns) */
__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *format, ...);

/* Assertion failure (never returns) */
__attribute__((noreturn)) void assertion_failure(const char *exp, const char *file, int line);

#endif // INCLUDE_DEBUG_H_
