/*
 *
 *      stddef.h
 *      Basic variables and macro definition header files
 *
 *      2025/2/15 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_STDDEF_H_
#define INCLUDE_STDDEF_H_

typedef __SIZE_TYPE__    size_t;
typedef __INTPTR_TYPE__  ssize_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#if !defined(__cplusplus) && defined(__WCHAR_TYPE__)
typedef __WCHAR_TYPE__ wchar_t;
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
typedef typeof(nullptr) nullptr_t;
#endif

#undef NULL
#define NULL ((void *)0)

#undef offsetof
#define offsetof(s, m) __builtin_offsetof(s, m)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
#    undef unreachable
#    define unreachable()             __builtin_unreachable()
#    define __STDC_VERSION_STDDEF_H__ 202311L
#endif

#endif // INCLUDE_STDDEF_H_
