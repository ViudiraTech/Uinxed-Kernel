/*
 *
 *		printk.c
 *		内核调试和打印信息程序
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "string.h"
#include "stdlib.h"
#include "vargs.h"
#include "printk.h"
#include "common.h"
#include "cmos.h"
#include "tty.h"
#include "debug.h"

/* 打印带有”[ ** ]“的字符串 */
void print_busy(const char *str)
{
	printk("[");
	printk(" ** ");
	printk("] ");
	printk("%s", str);
}

/* 打印带有”[ OK ]“的字符串 */
void print_succ(const char *str)
{
	printk("[");
	printk("\033[32m OK \033[0m");
	printk("] ");
	printk("%s", str);
}

/* 打印带有”[ WARN ]“的字符串 */
void print_warn(const char *str)
{
	printk("[");
	printk("\033[33mWARN\033[0m");
	printk("] ");
	printk("%s", str);
}

/* 打印带有”[ ERRO ]“的字符串 */
void print_erro(const char *str)
{
	printk("[");
	printk("\033[31mERRO\033[0m");
	printk("] ");
	printk("%s", str);
}

/* 打印带有"[HH:MM:SS]"的字符串 */
void print_time(const char *str)
{
	printk("[");
	printk("%02d:%02d:%02d", get_hour_hex(), get_min_hex(), get_sec_hex());
	printk("] ");
	printk("%s", str);
}

/* 内核打印字符 */
void putchar(int ch)
{
	tty_print_logch(ch);
}

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

	if (i+1 > sizeof(buff)) panic(PFFF);

	buff[i] = '\0';

	tty_print_logstr(buff);
}

#define is_digit(c)	((c) >= '0' && (c) <= '9')

/* 跳过字符串中的数字并将这些连续数字的值返回 */
static int skip_atoi(const char **s)
{
	int i = 0;

	while (is_digit(**s)) {
		i = i * 10 + *((*s)++) - '0';
	}
	return i;
}

#define ZEROPAD		1	// pad with zero
#define SIGN		2 	// unsigned/signed long
#define PLUS    	4	// show plus
#define SPACE		8 	// space if plus
#define LEFT		16	// left justified
#define SPECIAL		32	// 0x
#define SMALL		64	// use 'abcdef' instead of 'ABCDEF'

#define do_div(n,base) ({														\
        int __res;																\
        __asm__("divl %4":"=a" (n),"=d" (__res):"0" (n),"1" (0),"r" (base));	\
        __res; })

/* 将整数格式化为字符串 */
static char *number(char *str, int num, int base, int size, int precision, int type)
{
	char c, sign, tmp[36];
	const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	int i;

	if (type & SMALL) {
		digits ="0123456789abcdefghijklmnopqrstuvwxyz";
	}
	if (type & LEFT) {
		type &= ~ZEROPAD;
	}
	if (base < 2 || base > 36) {
		return 0;
	}

	c = (type & ZEROPAD) ? '0' : ' ' ;

	if (type & SIGN && num < 0) {
		sign = '-';
		num = -num;
	} else {
		sign = (type&PLUS) ? '+' : ((type&SPACE) ? ' ' : 0);
	}
	if (sign) {
		size--;
	}
	if (type & SPECIAL) {
		if (base == 16) {
			size -= 2;
		} else if (base == 8) {
			size--;
		}
	}
	i = 0;
	if (num == 0) {
		tmp[i++] = '0';
	} else {
		while (num != 0) {
			tmp[i++] = digits[do_div(num,base)];
		}
	}
	if (i > precision) {
		precision = i;
	}
	size -= precision;

	if (!(type&(ZEROPAD+LEFT))) {
		while (size-- > 0) {
			*str++ = ' ';
		}
	}
	if (sign) {
		*str++ = sign;
	}
	if (type & SPECIAL) {
		if (base == 8) {
			*str++ = '0';
		} else if (base == 16) {
			*str++ = '0';
			*str++ = digits[33];
		}
	}
	if (!(type&LEFT)) {
		while (size-- > 0) {
			*str++ = c;
		}
	}
	while (i < precision--) {
		*str++ = '0';
	}
	while (i-- > 0) {
		*str++ = tmp[i];
	}
	while (size-- > 0) {
		*str++ = ' ';
	}
	return str;
}

/* 格式化字符串并将其输出到一个字符数组中 */
int vsprintf(char *buff, const char *format, va_list args)
{
	int len;
	int i;
	char *str;
	char *s;
	int *ip;

	int flags;			// flags to number()

	int field_width;	// width of output field
	int precision;		// min. # of digits for integers; max number of chars for from string

	for (str = buff ; *format ; ++format) {
		if (*format != '%') {
			*str++ = *format;
			continue;
		}
		flags = 0;
		repeat:
			++format;	// this also skips first '%'
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

		/* get field width */
		field_width = -1;
		if (is_digit(*format)) {
			field_width = skip_atoi(&format);
		} else if (*format == '*') {
			/* it's the next argument */
			field_width = va_arg(args, int);
			if (field_width < 0) {
				field_width = -field_width;
				flags |= LEFT;
			}
		}

		/* get the precision */
		precision = -1;
		if (*format == '.') {
			++format;	
			if (is_digit(*format)) {
				precision = skip_atoi(&format);
			} else if (*format == '*') {
				/* it's the next argument */
				precision = va_arg(args, int);
			}
			if (precision < 0) {
				precision = 0;
			}
		}

		/* get the conversion qualifier */
		/* int qualifier = -1;	// 'h', 'l', or 'L' for integer fields */
		if (*format == 'h' || *format == 'l' || *format == 'L') {
			// qualifier = *format;
			++format;
		}
		switch (*format) {
		case 'c':
			if (!(flags & LEFT)) {
				while (--field_width > 0) {
					*str++ = ' ';
				}
			}
			*str++ = (unsigned char) va_arg(args, int);
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
			str = number(str, va_arg(args, unsigned long), 8,
				field_width, precision, flags);
			break;
		case 'p':
			if (field_width == -1) {
				field_width = 8;
				flags |= ZEROPAD;
			}
			str = number(str, (unsigned long) va_arg(args, void *), 16,
				field_width, precision, flags);
			break;
		case 'x':
			flags |= SMALL;
		case 'X':
			str = number(str, va_arg(args, unsigned long), 16,
				field_width, precision, flags);
			break;
		case 'd':
		case 'i':
			flags |= SIGN;
		case 'u':
			str = number(str, va_arg(args, unsigned long), 10,
				field_width, precision, flags);
			break;
		case 'f':
			str = ftoa(va_arg(args, double), str, precision);
			break;
		case 'b':
			str = number(str, va_arg(args, unsigned long), 2,
				field_width, precision, flags);
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

/* 将格式化的输出存储在字符数组中 */
void sprintf(char *str,const char *fmt, ...)
{
	va_list arg;
	va_start(arg,fmt);
	vsprintf(str,fmt,arg);
	va_end(arg);
}
