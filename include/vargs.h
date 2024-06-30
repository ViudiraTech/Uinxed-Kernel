/*
 *
 *		vargs.h
 *		定义C语言可变参数头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_VARGS_H_
#define INCLUDE_VARGS_H_

typedef __builtin_va_list va_list;

#define va_start(ap, last)		(__builtin_va_start(ap, last))
#define va_arg(ap, type)		(__builtin_va_arg(ap, type))
#define va_end(ap)

#endif // INCLUDE_VARGS_H_
