/*
 *
 *      stdarg.h
 *      C language variable parameter header file
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VARGS_H_
#define INCLUDE_VARGS_H_

typedef __builtin_va_list va_list;

#define va_start(ap, last) (__builtin_va_start(ap, last))
#define va_arg(ap, type)   (__builtin_va_arg(ap, type))
#define va_end(ap)         (__builtin_va_end(ap))
#define va_copy(dest, src) (__builtin_va_copy(dest, src))

#endif // INCLUDE_VARGS_H_
