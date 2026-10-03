/*
 *
 *      string_builtin.h
 *      Builtin dispatch for the string and memory functions
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_STRING_BUILTIN_H_
#define INCLUDE_STRING_BUILTIN_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/*
 * Builtin dispatch: a call folds to the builtin where it can and otherwise lands
 * on the function declared in string.h.  string.c defines
 * __LIBS_STD_STRING_INTERNAL to keep these out of its own definitions.  The mem*
 * family caps at 64 bytes, where the builtin switches from straight-line stores
 * to rep movs.
 */

#if __has_builtin(__builtin_constant_p)
#    define __LIBS_STD_FOLD_SMALL(n) (__builtin_constant_p(n) && (size_t)(n) <= 64)
#else
#    define __LIBS_STD_FOLD_SMALL(n) 0
#endif

#if __has_builtin(__builtin_memcpy)
#    define memcpy(str1, str2, n) (__LIBS_STD_FOLD_SMALL(n) ? __builtin_memcpy((str1), (str2), (n)) : (memcpy)((str1), (str2), (n)))
#endif

#if __has_builtin(__builtin_memset)
#    define memset(str, c, n) (__LIBS_STD_FOLD_SMALL(n) ? __builtin_memset((str), (c), (n)) : (memset)((str), (c), (n)))
#endif

#if __has_builtin(__builtin_memmove)
#    define memmove(str1, str2, n) (__LIBS_STD_FOLD_SMALL(n) ? __builtin_memmove((str1), (str2), (n)) : (memmove)((str1), (str2), (n)))
#endif

#if __has_builtin(__builtin_memcmp)
#    define memcmp(str1, str2, n) (__LIBS_STD_FOLD_SMALL(n) ? __builtin_memcmp((str1), (str2), (n)) : (memcmp)((str1), (str2), (n)))
#endif

#if __has_builtin(__builtin_memchr)
#    define memchr(str, c, n) (__LIBS_STD_FOLD_SMALL(n) ? __builtin_memchr((str), (c), (n)) : (memchr)((str), (c), (n)))
#endif

#if __has_builtin(__builtin_strlen)
#    define strlen(str) __builtin_strlen((str))
#endif

#if __has_builtin(__builtin_strcpy)
#    define strcpy(dest, src) __builtin_strcpy((dest), (src))
#endif

#if __has_builtin(__builtin_strncpy)
#    define strncpy(dest, src, n) __builtin_strncpy((dest), (src), (n))
#endif

#if __has_builtin(__builtin_strcmp)
#    define strcmp(str1, str2) __builtin_strcmp((str1), (str2))
#endif

#if __has_builtin(__builtin_strncmp)
#    define strncmp(str1, str2, n) __builtin_strncmp((str1), (str2), (n))
#endif

#if __has_builtin(__builtin_strcat)
#    define strcat(dest, src) __builtin_strcat((dest), (src))
#endif

#if __has_builtin(__builtin_strchr)
#    define strchr(str, c) __builtin_strchr((str), (c))
#endif

#if __has_builtin(__builtin_strrchr)
#    define strrchr(str, c) __builtin_strrchr((str), (c))
#endif

#if __has_builtin(__builtin_strstr)
#    define strstr(haystack, needle) __builtin_strstr((haystack), (needle))
#endif

#if __has_builtin(__builtin_strdup)
#    define strdup(s) __builtin_strdup((s))
#endif

#endif // INCLUDE_STRING_BUILTIN_H_
