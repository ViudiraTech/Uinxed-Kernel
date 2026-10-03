/*
 *
 *      stddef.h
 *      Basic variables and macro definition header files
 *
 *      2025/2/15 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_STDDEF_H_
#define INCLUDE_STDDEF_H_

/* __has_builtin is a compiler builtin; older GCC does not provide it. */
#ifndef __has_builtin
#    define __has_builtin(x) 0
#endif

#define NULL ((void *)0)

/* The builtin is what keeps offsetof() a constant expression, which the layout _Static_assert()s rely on. */
#if __has_builtin(__builtin_offsetof)
#    define offsetof(s, m) __builtin_offsetof(s, m)
#else
#    define offsetof(s, m) ((size_t) & (((s *)0)->m))
#endif

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
#    if __has_builtin(__builtin_unreachable)
#        define unreachable() __builtin_unreachable()
#    else
#        define unreachable() ((void)0)
#    endif
#    define __STDC_VERSION_STDDEF_H__ 202311L
#endif

typedef __SIZE_TYPE__    size_t;
typedef __INTPTR_TYPE__  ssize_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#if defined(__WCHAR_TYPE__)
typedef __WCHAR_TYPE__ wchar_t;
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
typedef typeof(nullptr) nullptr_t;
#endif

#endif // INCLUDE_STDDEF_H_
