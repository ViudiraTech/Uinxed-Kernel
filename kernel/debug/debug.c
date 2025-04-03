/*
 *
 *		debug.c
 *		Kernel debug
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "debug.h"
#include "printk.h"
#include "common.h"
#include "vargs.h"

/* Kernel panic */
void panic(const char *format, ...)
{
	static char buff[1024];
	va_list args;
	int i;

	va_start(args, format);
	i = vsprintf(buff, format, args);
	va_end(args);

	buff[i] = '\0';

	plogk("Kernel panic - not syncing: %s\n", buff);
	krn_halt();
}

/* Assertion failure */
void assertion_failure(const char *exp, const char *file, int line)
{
	printk("assert(%s) failed!\nfile: %s\nline: %d\n\n", exp, file, line);
	krn_halt();
}
