/*
 *
 *		string.h
 *		Basic memory operation and string processing function library header file
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_STRING_H_
#define INCLUDE_STRING_H_

#include "stdint.h"

/* Compares the first count bytes of two memory areas */
int memcmp(const void *buffer1, const void *buffer2, unsigned long count);

/* Copies len bytes from source address to destination address */
void *memcpy(void *dest, const void *src, unsigned long len);

/* Set the first len ​​bytes of the target memory area to the value val */
void *memset(void *dest, int val, unsigned long len);

/* Set the first len ​​bytes of the target memory area to 0 */
void bzero(void *dest, unsigned long len);

/* Clears the memory of a character array s */
void memclean(char *s, int len);

/* Comparing two strings */
int strcmp(const char *dest, const char *src);

/* Copies string src to dest */
char *strcpy(char *dest, const char *src);

/* Copies the first len ​​characters of string to dest */
char *strncpy(char *dest, const char *src, unsigned long len);

/* Concatenates the string src to the end of dest */
char *strcat(char *dest, const char *src);

/* Searches for a character in a string and returns the position of the first occurrence of that character in the string */
char *strchr(char *str, int c);

/* Returns the length of string src */
int strlen(const char *src);

/* Delete the characters at the specified position in a string */
void delete_char(char *str, int pos);

/* Insert a character at a specified position in a string */
void insert_char(char *str, int pos, char ch);

/* Inserts a string into another string at a specified position */
void insert_str(char *str, char *insert_str, int pos);

/* Convert all letters in a string to uppercase */
char *strupr(char *src);

/* Convert all letters in a string to lowercase */
char *strlwr(char *src);

#endif // INCLUDE_STRING_H_
