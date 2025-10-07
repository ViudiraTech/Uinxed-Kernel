/*
 *
 *      utflib.h
 *      UTF encoding library header file
 *
 *      2025/10/7 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_UTFLIB_H_
#define INCLUDE_UTFLIB_H_

#include "stddef.h"
#include "stdint.h"

#define UTFmax    6            // maximum bytes per rune
#define Runeerror ((Rune) - 1) // decoding error in utf

typedef int32_t Rune;

/* Check if the rune is a valid Unicode character */
int isvalidrune(Rune r);

/* Convert a UTF-8 sequence to a Unicode rune */
int charntorune(Rune *p, const char *s, size_t len);

/* Convert a single UTF-8 character to a Unicode rune */
int chartorune(Rune *p, const char *s);

/* Get the number of Unicode characters in a UTF-8 string */
size_t utfnlen(const char *s, size_t len);

/* Get the number of Unicode characters in a UTF-8 string (without length limit) */
size_t utflen(const char *s);

#endif // INCLUDE_UTFLIB_H_
