/*
 *
 *		string.c
 *		Basic memory operation and string processing function library
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "string.h"

/* Compares the first count bytes of two memory areas */
inline int memcmp(const void *buffer1, const void *buffer2, unsigned long count)
{
	const char *a = buffer1;
	const char *b = buffer2;
	while (count-- > 0) {
		if (*a != *b) return *a > *b ? 1 : -1;
		a++, b++;
	}
	return 0;
}

/* Copies len bytes from source address to destination address */
inline void *memcpy(void *dest, const void *src, unsigned long len)
{
	char *d = (char *)dest;
	const char *s = (const char *)src;
	void *ret = dest;
	if (len < 8) {
		while (len--) {
			*d++ = *s++;
		}
		return ret;
	}

	unsigned long align = (unsigned long)d & (sizeof(unsigned long) - 1);
	if (align) {
		align = sizeof(unsigned long) - align;
		len -= align;
		while (align--) {
			*d++ = *s++;
		}
	}

	unsigned long *dw = (unsigned long *)d;
	const unsigned long *sw = (const unsigned long *)s;
	for (unsigned long i = 0; i < len / sizeof(unsigned long); i++) {
		*dw++ = *sw++;
	}
	d = (char *)dw;
	s = (const char *)sw;

	unsigned long remain = len & (sizeof(unsigned long) - 1);
	while (remain--) {
		*d++ = *s++;
	}
	return ret;
}

/* Set the first len ​​bytes of the target memory area to the value val */
inline void *memset(void *dest, int val, unsigned long len)
{
	unsigned char *d = dest;
	unsigned char v = (unsigned char)val;
	while(len && ((unsigned long)d & 7)) {
		*d++ = v;
		len--;
	}

	unsigned long v8 = v * 0x0101010101010101ULL;
	while(len >= 8) {
		*(unsigned long *)d = v8;
		d += 8;
		len -= 8;
	}
	while (len--) *d++ = v;
	return dest;
}

/* Set the first len ​​bytes of the target memory area to 0 */
inline void bzero(void *dest, unsigned long len)
{
	memset(dest, 0, len);
}

/* Clears the memory of a character array s */
inline void memclean(char *s, int len)
{
	int i;
	for (i = 0; i != len; i++) {
		s[i] = 0;
	}
	return;
}

/* Comparing two strings */
inline int strcmp(const char *dest, const char *src)
{
	int ret = 0 ;

	while(!(ret = *(unsigned char *)src - *(unsigned char *)dest) && *dest) {
		++src;
		++dest;
	}
	if (ret < 0) {
		ret = -1;
	}
	else if (ret > 0) {
		ret = 1;
	}
	return ret;
}

/* Copies string src to dest */
inline char *strcpy(char *dest, const char *src)
{
	char *tmp = dest;

	while (*src) {
		*dest++ = *src++;
	}
	*dest = '\0';	
	return tmp;
}

/* Copies the first len ​​characters of string to dest */
inline char *strncpy(char *dest, const char *src, unsigned long len)
{
	char *dst = dest;

	while (len > 0) {
		while (*src) {
			*dest++ = *src++;
		}
		len--;
	}
	*dest = '\0';
	return dst;
}

/* Concatenates the string src to the end of dest */
inline char *strcat(char *dest, const char *src)
{
	char *cp = dest;

	while (*cp) {
		cp++;
	}
	while ((*cp++ = *src++));
	return dest;
}

/* Searches for a character in a string and returns the position of the first occurrence of that character in the string */
inline char *strchr(char *str, int c)
{
	for (; *str != 0; ++str) {
		if (*str == c) {
			return str;
		}
	}
	return 0;
}

/* Returns the length of string src */
inline int strlen(const char *src)
{
	const char *eos = src;
	while (*eos++);
	return (eos - src - 1);
}

/* Delete the characters at the specified position in a string */
inline void delete_char(char *str, int pos)
{
	int i;
	for (i = pos; i < strlen(str); i++) {
		str[i] = str[i + 1];
	}
}

/* Insert a character at a specified position in a string */
inline void insert_char(char *str, int pos, char ch)
{
	int i;
	for (i = strlen(str); i >= pos; i--) {
		str[i + 1] = str[i];
	}
	str[pos] = ch;
}

/* Inserts a string into another string at a specified position */
inline void insert_str(char *str, char *insert_str, int pos)
{
	for (int i = 0; i < strlen(insert_str); i++) {
		insert_char(str, pos + i, insert_str[i]);
	}
}

/* Convert all letters in a string to uppercase */
inline char *strupr(char *src)
{
	while (*src != '\0') {
		if (*src >= 'a' && *src <= 'z')
			*src -= 32;
		src++;
	}
	return src;
}

/* Convert all letters in a string to lowercase */
inline char *strlwr(char *src)
{
	while (*src != '\0') {
		if (*src > 'A' && *src <= 'Z') {
			*src += 32;
		}
		src++;
	}
	return src;
}
