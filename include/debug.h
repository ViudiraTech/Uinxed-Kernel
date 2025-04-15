/*
 *
 *		debug.h
 *		Kernel debug header files
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_DEBUG_H_
#define INCLUDE_DEBUG_H_

#define assert(exp) if (!exp) assertion_failure(#exp, __FILE__, __LINE__)

/* Dump stack */
void dump_stack(void);

/* Kernel panic */
void panic(const char *format, ...);

/* Assertion failure */
void assertion_failure(const char *exp, const char *file, int line);

#endif // INCLUDE_DEBUG_H_
