/*
 *
 *      utflib.h
 *      UTF encoding library header file
 *
 *      2025/10/7 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_UTFLIB_H_
#define INCLUDE_UTFLIB_H_

#include <stddef.h>
#include <stdint.h>

#define UTFmax    6              // maximum bytes per rune
#define Runeerror ((rune_t) - 1) // decoding error in utf

typedef int32_t rune_t;

/* Check if the rune is a valid Unicode character */
int isvalidrune(rune_t r);

/* Convert a UTF-8 sequence to a Unicode rune */
int charntorune(rune_t *p, const char *s, size_t len);

/* Convert a single UTF-8 character to a Unicode rune */
int chartorune(rune_t *p, const char *s);

/* Get the number of Unicode characters in a UTF-8 string */
size_t utfnlen(const char *s, size_t len);

/* Get the number of Unicode characters in a UTF-8 string (without length limit) */
size_t utflen(const char *s);

#endif // INCLUDE_UTFLIB_H_
