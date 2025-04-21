/*
 *
 *      printk.h
 *      Kernel string print header file
 *
 *      2024/6/27 By Rainy101112
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_PRINTK_H_
#define INCLUDE_PRINTK_H_

#include "stdint.h"
#include "vargs.h"

/* Kernel print string */
void printk(const char *format, ...);

/* Kernel print log */
void plogk(const char *format, ...);

/* Store the formatted output in a character array */
void sprintf(char *str, const char *fmt, ...);

/* Format a string and output it to a character array */
int vsprintf(char *buff, const char *format, va_list args);

/* Format a string with size and output it to a character array */
int vsprintf_s(char *buff, intptr_t size, const char **format, va_list args);

#endif // INCLUDE_PRINTK_H_
