/*
 *
 *      string.h
 *      Handling string and memory block header files
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_STRING_H_
#define INCLUDE_STRING_H_

#include <stddef.h>
#include <stdint.h>

/* Copy n bytes from memory area str2 to memory area str1 */
void *memcpy(void *str1, const void *str2, size_t n);

/* Sets a memory area to the specified value */
void *memset(void *str, int c, size_t n);

/* Copies n characters from str2 to str1, accounting for overlaps */
void *memmove(void *str1, const void *str2, size_t n);

/* Compares the first n bytes of memory area str1 with those of memory area str2 */
int memcmp(const void *str1, const void *str2, size_t n);

/* Calculates the length of the string str */
size_t strlen(const char *str);

/* Copies the string pointed to by src to dest */
char *strcpy(char *dest, const char *src);

/* Copies the string pointed to by src to dest, up to n characters. */
char *strncpy(char *dest, const char *src, size_t n);

/* Compares the string pointed to by str1 with the string pointed to by str2 */
int strcmp(const char *str1, const char *str2);

/* Compares the first n characters of two strings for equality */
int strncmp(const char *str1, const char *str2, size_t n);

/* Append the string pointed to by src to the end of the string pointed to by dest */
char *strcat(char *dest, const char *src);

/* Finds a character in a string and returns the position of the character in the string */
char *strchr(const char *str, int c);

/* Searches the string pointed to by the parameter str for the last occurrence of the character c */
char *strrchr(const char *str, int c);

/* Find the first occurrence of the string needle in the string haystack, excluding the terminator */
char *strstr(const char *haystack, const char *needle);

/* Make a copy of the string and return it */
void *strdup(const char *s);

/* String equality check */
int streq(const char *s1, const char *s2);

/* String splitting */
char *strtok(char *str, const char *delim);

/* String to long integer */
int64_t strtol(const char *str, char **endptr, int base);

#endif // INCLUDE_STRING_H_
