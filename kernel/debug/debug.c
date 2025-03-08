/*
 *
 *		debug.c
 *		内核调试
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "debug.h"
#include "printk.h"
#include "common.h"
#include "vargs.h"

/* 内核异常 */
void panic(const char *format, ...)
{
	/* 避免频繁创建临时变量，内核的栈很宝贵 */
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

/* 断言失败 */
void assertion_failure(const char *exp, const char *file, int line)
{
	printk("assert(%s) failed!\nfile: %s\nline: %d\n\n", exp, file, line);
	krn_halt();
}
