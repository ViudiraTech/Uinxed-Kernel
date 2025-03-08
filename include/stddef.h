/*
 *
 *		stddef.h
 *		基本变量与宏定义头文件
 *
 *		2025/2/15 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_STDDEF_H_
#define INCLUDE_STDDEF_H_

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__ wchar_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
typedef typeof(nullptr) nullptr_t;
#endif

#undef NULL
#define NULL ((void *)0)

#undef offsetof
#define offsetof(s, m) __builtin_offsetof(s, m)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
#	undef unreachable
#	define unreachable() __builtin_unreachable()
#	define __STDC_VERSION_STDDEF_H__ 202311L
#endif

#endif // INCLUDE_STDDEF_H_
