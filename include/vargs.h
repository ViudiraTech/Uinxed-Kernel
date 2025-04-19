/*
 *
 *		vargs.h
 *		C language variable parameter header file
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_VARGS_H_
#define INCLUDE_VARGS_H_

typedef __builtin_va_list va_list;

#define va_start(ap, last) (__builtin_va_start(ap, last))
#define va_arg(ap, type)   (__builtin_va_arg(ap, type))
#define va_end(ap)

#endif // INCLUDE_VARGS_H_
