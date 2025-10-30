/*
 *
 *      debug.h
 *      Kernel debug header files
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DEBUG_H_
#define INCLUDE_DEBUG_H_

#define assert(exp) \
    if (!(exp)) assertion_failure(#exp, __FILE__, __LINE__)

/* if the stack carries an error code, set this variable to 1 before calling painc */
extern int carry_error_code;

/* Dump stack */
void dump_stack(void);

/* Kernel panic */
void panic(const char *format, ...);

/* Assertion failure */
void assertion_failure(const char *exp, const char *file, int line);

#endif // INCLUDE_DEBUG_H_
