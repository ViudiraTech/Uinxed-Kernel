/*
 *
 *		string.c
 *		Handling string and memory block
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "string.h"
#include "stdint.h"
#include "stddef.h"

/* Copy n bytes from memory area str2 to memory area str1 */
inline void *memcpy(void *str1, const void *str2, size_t n)
{
	__volatile__ uint8_t *dest = (__volatile__ uint8_t *)str1;
	__volatile__ const uint8_t *src = (__volatile__ const uint8_t *)str2;
	__volatile__ const uint8_t *end = (__volatile__ const uint8_t *)((uint8_t *)str2 + n);

	if (dest == src) return str1;
	while (src != end) *dest++ = *src++;
	return str1;
}

/* Sets a memory area to the specified value */
inline void *memset(void *str, int c, size_t n)
{
	__volatile__ uint8_t *_str = (__volatile__ uint8_t *)str;
	__volatile__ uint8_t *end = (__volatile__ uint8_t *)((uint8_t *)str + n);
	const uint8_t _c = c;

	for (; _str < end; _str++) *_str = _c;
	return str;
}

/* Copies n characters from str2 to str1, accounting for overlaps */
inline void *memmove(void *str1, const void *str2, size_t n)
{
	__volatile__ uint8_t *dest = (__volatile__ uint8_t *)str1;
	__volatile__ const uint8_t *src = (__volatile__ const uint8_t *)str2;
	__volatile__ const uint8_t *end = (__volatile__ const uint8_t *)((uint8_t *)str2 + n);

	if (dest == src) return str1;
	if (dest > src && dest < end) {
		dest += n;
		while (src != end) *--dest = *--end;
	} else {
		while (src != end) *dest++ = *src++;
	}
	return str1;
}

/* Compares the first n bytes of memory area str1 with those of memory area str2 */
inline int memcmp(const void *str1, const void *str2, size_t n)
{
	const uint8_t *_str1 = (const uint8_t *)str1;
	const uint8_t *_str2 = (const uint8_t *)str2;
	const uint8_t *end = (const uint8_t *)((uint8_t *)str1 + n);

	for (; _str1 < end; _str1++, _str2++) {
		if (*_str1 < *_str2) return -1;
		if (*_str1 > *_str2) return 1;
	}
	return 0;
}

/* Calculates the length of the string str */
inline size_t strlen(const char *str)
{
	size_t len = 0;
	while (*str++ != '\0') len++;
	return len;
}

/* Copies the string pointed to by src to dest */
inline char *strcpy(char *dest, const char *src)
{
	char *_dest = dest;
	while ((*dest++ = *src++) != '\0');
	return _dest;
}

/* Copies the string pointed to by src to dest, up to n characters. */
inline char *strncpy(char *dest, const char *src, size_t n)
{
	char *_dest = dest;
	const char *end = src + n;
	while (src < end && (*dest++ = *src++) != '\0');
	return _dest;
}

/* Compares the string pointed to by str1 with the string pointed to by str2 */
inline int strcmp(const char *str1, const char *str2)
{
	const uint8_t *_str1 = (const uint8_t *)str1;
	const uint8_t *_str2 = (const uint8_t *)str2;
	int c1, c2;

	do {
		c1 = *_str1++;
		c2 = *_str2++;
		if (!c1) return c1 - c2;
	} while (c1 == c2);
	return c1 - c2;
}

/* Compares the first n characters of two strings for equality */
inline int strncmp(const char *str1, const char *str2, size_t n)
{
	const uint8_t *_str1 = (const uint8_t *)str1;
	const uint8_t *end = (const uint8_t *)str1 + n;
	const uint8_t *_str2 = (const uint8_t *)str2;
	uint8_t c1, c2;

	while (_str1 != end) {
		c1 = *_str1++;
		c2 = *_str2++;
		if (!c1) return (int)c1 - (int)c2;
		if (c1 != c2) return c1 - c2;
	}
	return 0;
}

/* Append the string pointed to by src to the end of the string pointed to by dest */
inline char *strcat(char *dest, const char *src)
{
	const char *_dest = dest;
	while (*dest++ != '\0');
	dest--;
	while ((*dest++ = *src++) != '\0');
	return (char *)_dest;
}

/* Finds a character in a string and returns the position of the character in the string */
inline char *strchr(const char *str, int c)
{
	for (; *str != '\0'; str++) {
		if (*str == c) return (char *)str;
	}
	return 0;
}

/* Searches the string pointed to by the parameter str for the last occurrence of the character c */
inline char *strrchr(const char *str, int c)
{
	const char *finded = 0;
	for (; *str != '\0'; str++) {
		if (*str == c) finded = str;
	}
	return (char *)finded;
}

/* Find the first occurrence of the string needle in the string haystack, excluding the terminator */
inline char *strstr(const char *haystack, const char *needle)
{
	size_t _sn = strlen(haystack), _tn = strlen(needle);

	if (_tn == 0) return (char *)haystack;
	if (_sn < _tn) return 0;

	const char *s = haystack, *t = needle;

	for (size_t i = 0; i <= _sn - _tn; i++) {
		if (strncmp(s + i, t, _tn) == 0) return (char *)(s + i);
	}
	return 0;
}
