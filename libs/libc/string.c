/*
 *
 *      string.c
 *      Handling string and memory block
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <heap.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Copy n bytes from memory area str2 to memory area str1 */
void *memcpy(void *str1, const void *str2, size_t n)
{
#if defined(__builtin_memcpy)
    return __builtin_memcpy(str1, str2, n);
#else
    volatile uint8_t       *dest = (volatile uint8_t *)str1;
    const volatile uint8_t *src  = (const volatile uint8_t *)str2;
    const volatile uint8_t *end  = (const volatile uint8_t *)((uint8_t *)str2 + n);

    if (dest == src) return str1;
    while (src != end) *dest++ = *src++;
    return str1;
#endif
}

/* Sets a memory area to the specified value */
void *memset(void *str, int c, size_t n)
{
#if defined(__builtin_memset)
    return __builtin_memset(str, c, n);
#else
    volatile uint8_t *_str = (volatile uint8_t *)str;
    volatile uint8_t *end  = (volatile uint8_t *)((uint8_t *)str + n);
    const uint8_t     _c   = c;

    for (; _str < end; _str++) *_str = _c;
    return str;
#endif
}

/* Copies n characters from str2 to str1, accounting for overlaps */
void *memmove(void *str1, const void *str2, size_t n)
{
#if defined(__builtin_memmove)
    return __builtin_memmove(str1, str2, n);
#else
    volatile uint8_t       *dest = (volatile uint8_t *)str1;
    const volatile uint8_t *src  = (const volatile uint8_t *)str2;
    const volatile uint8_t *end  = (const volatile uint8_t *)((uint8_t *)str2 + n);

    if (dest == src) return str1;
    if (dest > src && dest < end) {
        dest += n;
        while (src != end) *--dest = *--end;
    } else {
        while (src != end) *dest++ = *src++;
    }
    return str1;
#endif
}

/* Compares the first n bytes of memory area str1 with those of memory area str2 */
int memcmp(const void *str1, const void *str2, size_t n)
{
#if defined(__builtin_memcmp)
    return __builtin_memcmp(str1, str2, n);
#else
    const uint8_t *_str1 = (const uint8_t *)str1;
    const uint8_t *_str2 = (const uint8_t *)str2;
    const uint8_t *end   = (const uint8_t *)((uint8_t *)str1 + n);

    for (; _str1 < end; _str1++, _str2++) {
        if (*_str1 < *_str2) return -1;
        if (*_str1 > *_str2) return 1;
    }
    return 0;
#endif
}

/* Calculates the length of the string str */
size_t strlen(const char *str)
{
#if defined(__builtin_strlen)
    return __builtin_strlen(str);
#else
    size_t len = 0;
    while (*str++ != '\0') len++;
    return len;
#endif
}

/* Copies the string pointed to by src to dest */
char *strcpy(char *dest, const char *src)
{
#if defined(__builtin_strcpy)
    return __builtin_strcpy(dest, src);
#else
    char *_dest = dest;
    while ((*dest++ = *src++) != '\0');
    return _dest;
#endif
}

/* Copies the string pointed to by src to dest, up to n characters. */
char *strncpy(char *dest, const char *src, size_t n)
{
#if defined(__builtin_strncpy)
    return __builtin_strncpy(dest, src, n);
#else
    char       *_dest = dest;
    const char *end   = src + n;
    while (src < end && *src != '\0') *(dest++) = *(src++);
    return _dest;
#endif
}

/* Compares the string pointed to by str1 with the string pointed to by str2 */
int strcmp(const char *str1, const char *str2)
{
#if defined(__builtin_strcmp)
    return __builtin_strcmp(str1, str2);
#else
    const uint8_t *_str1 = (const uint8_t *)str1;
    const uint8_t *_str2 = (const uint8_t *)str2;
    int            c1, c2;

    do {
        c1 = *_str1++;
        c2 = *_str2++;
        if (!c1) return c1 - c2;
    } while (c1 == c2);
    return c1 - c2;
#endif
}

/* Compares the first n characters of two strings for equality */
int strncmp(const char *str1, const char *str2, size_t n)
{
#if defined(__builtin_strncmp)
    return __builtin_strncmp(str1, str2, n);
#else
    const uint8_t *_str1 = (const uint8_t *)str1;
    const uint8_t *end   = (const uint8_t *)str1 + n;
    const uint8_t *_str2 = (const uint8_t *)str2;
    uint8_t        c1, c2;

    while (_str1 != end) {
        c1 = *_str1++;
        c2 = *_str2++;
        if (!c1) return (int)c1 - (int)c2;
        if (c1 != c2) return c1 - c2;
    }
    return 0;
#endif
}

/* Append the string pointed to by src to the end of the string pointed to by dest */
char *strcat(char *dest, const char *src)
{
#if defined(__builtin_strcat)
    return __builtin_strcat(dest, src);
#else
    const char *_dest = dest;
    while (*dest++ != '\0');
    dest--;
    while ((*dest++ = *src++) != '\0');
    return (char *)_dest;
#endif
}

/* Finds a character in a string and returns the position of the character in the string */
char *strchr(const char *str, int c)
{
#if defined(__builtin_strchr)
    return __builtin_strchr(str, c);
#else
    for (; *str != '\0'; str++) {
        if (*str == c) return (char *)str;
    }
    return 0;
#endif
}

/* Searches the string pointed to by the parameter str for the last occurrence of the character c */
char *strrchr(const char *str, int c)
{
#if defined(__builtin_strrchr)
    return __builtin_strrchr(str, c);
#else
    const char *finded = 0;
    for (; *str != '\0'; str++) {
        if (*str == c) finded = str;
    }
    return (char *)finded;
#endif
}

/* Find the first occurrence of the string needle in the string haystack, excluding the terminator */
char *strstr(const char *haystack, const char *needle)
{
#if defined(__builtin_strstr)
    return __builtin_strstr(haystack, needle);
#else
    size_t _sn = strlen(haystack), _tn = strlen(needle);

    if (!_tn) return (char *)haystack;
    if (_sn < _tn) return 0;

    const char *s = haystack, *t = needle;

    for (size_t i = 0; i <= _sn - _tn; i++) {
        if (!strncmp(s + i, t, _tn)) return (char *)(s + i);
    }
    return 0;
#endif
}

/* Make a copy of the string and return it */
void *strdup(const char *s)
{
#if defined(__builtin_strdup)
    return __builtin_strdup(s);
#else
    size_t len = strlen(s) + 1;
    void  *p   = (void *)malloc(len);

    if (p) memcpy(p, (uint8_t *)s, len);
    return p;
#endif
}

/* String equality check */
int streq(const char *s1, const char *s2)
{
#if defined(__builtin_streq)
    return __builtin_streq(s1, s2);
#else
    return !strcmp(s1, s2);
#endif
}

/* String splitting */
char *strtok(char *str, const char *delim)
{
#if defined(__builtin_strtok)
    return __builtin_strtok(str, delim);
#else
    static char *last = 0;
    if (str) {
        last = str;
    } else if (!last) {
        return 0;
    }

    char *start = last;
    while (*start && strchr(delim, *start)) start++;

    if (*start == '\0') {
        last = 0;
        return 0;
    }

    char *end = start;
    while (*end && !strchr(delim, *end)) end++;

    if (*end) {
        *end = '\0';
        last = end + 1;
    } else {
        last = 0;
    }
    return start;
#endif
}

/* String to long integer */
int64_t strtol(const char *str, char **endptr, int base)
{
#if defined(__builtin_strtol)
    return __builtin_strtol(str, endptr, base);
#else
    const char *s      = str;
    uint64_t    acc    = 0;
    char        c      = '\0';
    uint64_t    cutoff = 0;
    uint64_t    neg    = 0;
    uint64_t    any    = 0;
    uint64_t    cutlim = 0;

    do {
        c = *s++;
    } while (IS_SPACE((unsigned char)c));

    if (c == '-') {
        neg = 1;
        c   = *s++;
    } else {
        neg = 0;
        if (c == '+') c = *s++;
    }
    if ((base == 0 || base == 16) && c == '0' && (*s == 'x' || *s == 'X')
        && ((s[1] >= '0' && s[1] <= '9') || (s[1] >= 'A' && s[1] <= 'F') || (s[1] >= 'a' && s[1] <= 'f'))) {
        c = s[1];
        s += 2;
        base = 16;
    }

    if (base == 0) base = c == '0' ? 8 : 10;
    acc = any = 0;
    if (base < 2 || base > 36) goto noconv;

    cutoff = neg ? (unsigned long)-(LONG_MIN + LONG_MAX) + LONG_MAX : LONG_MAX;
    cutlim = cutoff % base;
    cutoff /= base;

    for (;; c = *s++) {
        if (c >= '0' && c <= '9')
            c -= '0';
        else if (c >= 'A' && c <= 'Z')
            c -= 'A' - 10;
        else if (c >= 'a' && c <= 'z')
            c -= 'a' - 10;
        else
            break;
        if (c >= base) break;
        if (acc > cutoff || (acc == cutoff && ((uint64_t)c) > cutlim)) {
            any = -1;
        } else {
            any = 1;
            acc *= base;
            acc += c;
        }
    }
    if (!any) {
noconv:;
    } else if (neg) {
        acc = -acc;
    }
    if ((void *)endptr) *endptr = (char *)(any ? s - 1 : str);
    return (int64_t)(acc);
#endif
}
