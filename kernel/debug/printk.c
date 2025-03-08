/*
 *
 *		printk.c
 *		内核字符串打印
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "video.h"
#include "string.h"
#include "stdlib.h"
#include "vargs.h"
#include "printk.h"
#include "acpi.h"
#include "cmos.h"

/* 内核打印字符串 */
void printk(const char *format, ...)
{
	/* 避免频繁创建临时变量，内核的栈很宝贵 */
	static char buff[2048];
	va_list args;
	int i;

	va_start(args, format);
	i = vsprintf(buff, format, args);
	va_end(args);

	buff[i] = '\0';
	video_put_string(buff);
}

/* 内核打印带颜色的字符串 */
void printk_color(int fore, const char *format, ...)
{
	/* 避免频繁创建临时变量，内核的栈很宝贵 */
	static char buff[2048];
	va_list args;
	int i;

	va_start(args, format);
	i = vsprintf(buff, format, args);
	va_end(args);

	buff[i] = '\0';
	video_put_string_color(buff, fore);
}

/* 内核打印日志 */
void plogk(const char *format, ...)
{
	/* 避免频繁创建临时变量，内核的栈很宝贵 */
	static char buff[2048];
	va_list args;
	int i;

	va_start(args, format);
	i = vsprintf(buff, format, args);
	va_end(args);

	buff[i] = '\0';

	printk("[");
	printk("%5d.%06d", nanoTime() / 1000000000, (nanoTime() / 1000) % 1000000);
	printk("] ");
	printk(buff);
}

/* 内核打印带颜色的日志 */
void plogk_color(int fore, const char *format, ...)
{
	/* 避免频繁创建临时变量，内核的栈很宝贵 */
	static char buff[2048];
	va_list args;
	int i;

	va_start(args, format);
	i = vsprintf(buff, format, args);
	va_end(args);

	buff[i] = '\0';

	printk("[");
	printk("%5d.%06d", nanoTime() / 1000000000, (nanoTime() / 1000) % 1000000);
	printk("] ");
	printk_color(fore, buff);
}

/* 将格式化的输出存储在字符数组中 */
void sprintf(char *str, const char *fmt, ...)
{
	va_list arg;
	va_start(arg,fmt);
	vsprintf(str,fmt,arg);
	va_end(arg);
}

/* 格式化字符串并将其输出到一个字符数组中 */
int vsprintf(char *buff, const char *format, va_list args)
{
	int len, i, flags, field_width, precision;
	char *str, *s;
	int *ip;

	for (str = buff; *format; ++format) {
		if (*format != '%') {
			*str++ = *format;
			continue;
		}
		flags = 0;
		repeat:
			++format;
		switch (*format) {
				case '-': flags |= LEFT;
					goto repeat;
				case '+': flags |= PLUS;
					goto repeat;
				case ' ': flags |= SPACE;
					goto repeat;
				case '#': flags |= SPECIAL;
					goto repeat;
				case '0': flags |= ZEROPAD;
					goto repeat;
			}
		field_width = -1;
		if (is_digit(*format)) {
			field_width = skip_atoi(&format);
		} else if (*format == '*') {
			field_width = va_arg(args, int);
			if (field_width < 0) {
				field_width = -field_width;
				flags |= LEFT;
			}
		}
		precision = -1;
		if (*format == '.') {
			++format;	
			if (is_digit(*format)) {
				precision = skip_atoi(&format);
			} else if (*format == '*') {
				precision = va_arg(args, int);
			}
			if (precision < 0) {
				precision = 0;
			}
		}
		if (*format == 'h' || *format == 'l' || *format == 'L') {
			++format;
		}
		switch (*format) {
		case 'c':
			if (!(flags & LEFT)) {
				while (--field_width > 0) {
					*str++ = ' ';
				}
			}
			*str++ = (unsigned char)va_arg(args, int);
			while (--field_width > 0) {
				*str++ = ' ';
			}
			break;
		case 's':
			s = va_arg(args, char *);
			len = strlen(s);
			if (precision < 0) {
				precision = len;
			} else if (len > precision) {
				len = precision;
			}
			if (!(flags & LEFT)) {
				while (len < field_width--) {
					*str++ = ' ';
				}
			}
			for (i = 0; i < len; ++i) {
				*str++ = *s++;
			}
			while (len < field_width--) {
				*str++ = ' ';
			}
			break;
		case 'o':
			str = number(str, va_arg(args, unsigned long), 8, field_width, precision, flags);
			break;
		case 'p':
			if (field_width == -1) {
				field_width = 8;
				flags |= ZEROPAD;
			}
			str = number(str, (unsigned long)va_arg(args, void *), 16, field_width, precision, flags);
			break;
		case 'x':
			flags |= SMALL; // fallthrough
		case 'X':
			str = number(str, va_arg(args, unsigned long), 16, field_width, precision, flags);
			break;
		case 'd':
		case 'i':
			flags |= SIGN; // fallthrough
		case 'u':
			str = number(str, va_arg(args, unsigned long), 10, field_width, precision, flags);
			break;
		case 'b':
			str = number(str, va_arg(args, unsigned long), 2, field_width, precision, flags);
			break;
		case 'n':
			ip = va_arg(args, int *);
			*ip = (str - buff);
			break;
		default:
			if (*format != '%')
				*str++ = '%';
			if (*format) {
				*str++ = *format;
			} else {
				--format;
			}
			break;
		}
	}
	*str = '\0';
	return (str -buff);
}
