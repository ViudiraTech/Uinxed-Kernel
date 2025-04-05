/*
 *
 *		string.h
 *		Handling string and memory block header files
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_STRING_H_
#define INCLUDE_STRING_H_

#include "stdint.h"

/* Copy n bytes from memory area str2 to memory area str1 */
void *memcpy(void *str1, const void *str2, unsigned long n);

/* Sets a memory area to the specified value */
void *memset(void *str, int c, unsigned long n);

/* Copies n characters from str2 to str1, accounting for overlaps */
void *memmove(void *str1, const void *str2, unsigned long n);

/* Compares the first n bytes of memory area str1 with those of memory area str2 */
int memcmp(const void *str1, const void *str2, unsigned long n);

/* Calculates the length of the string str */
unsigned long strlen(const char *str);

/* Copies the string pointed to by src to dest */
char *strcpy(char *dest, const char *src);

/* Copies the string pointed to by src to dest, up to n characters. */
char *strncpy(char *dest, const char *src, unsigned long n);

/* Compares the string pointed to by str1 with the string pointed to by str2 */
int strcmp(const char *str1, const char *str2);

/* Compares the first n characters of two strings for equality */
int strncmp(const char *str1, const char *str2, unsigned long n);

/* Append the string pointed to by src to the end of the string pointed to by dest */
char *strcat(char *dest, const char *src);

/* Finds a character in a string and returns the position of the character in the string */
char *strchr(const char *str, int c);

/* Searches the string pointed to by the parameter str for the last occurrence of the character c */
char *strrchr(const char *str, int c);

/* Find the first occurrence of the string needle in the string haystack, excluding the terminator */
char *strstr(const char *haystack, const char *needle);

#endif // INCLUDE_STRING_H_
