/*
 *
 *		stdlib.c
 *		通用工具内联函数库
 *
 *		2024/10/2 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "stdlib.h"
#include "string.h"

/* 延迟 */
void delay(uint32_t count)
{
	while (count--);
}

/* 阻塞函数 */
void hang(void)
{
	while (1) printk("");
}

/* 将字符串数字转换为整数数字 */
int atoi(char* pstr)
{
	int ret_integer = 0;
	int integer_sign = 1;

	if (*pstr == '-') integer_sign = -1;
	if (*pstr == '-' || *pstr == '+') pstr++;

	while (*pstr >= '0' && *pstr <= '9') {
		ret_integer = ret_integer * 10 + *pstr - '0';
		pstr++;
	}
	ret_integer = integer_sign * ret_integer;
	return ret_integer;
}

/* 从文件件路径中获取文件名 */
char *get_filename(char *path)
{
	char ch = '/';
	char *filename = strchr(path, ch) + 1;
	return filename;
}

/* 把浮点数转换成字符数组 */
char *ftoa(double f, char *buf, int precision)
{
	char * ptr = buf;
	char * p = ptr;
	char * p1;
	char c;
	long intPart;

	if (precision > MAX_PRECISION)
		precision = MAX_PRECISION;
	if (f < 0)
	{
		f = -f;
		*ptr++ = '-';
	}
	if (precision < 0)
	{
		if (f < 1.0) precision = 6;
		else if (f < 10.0) precision = 5;
		else if (f < 100.0) precision = 4;
		else if (f < 1000.0) precision = 3;
		else if (f < 10000.0) precision = 2;
		else if (f < 100000.0) precision = 1;
		else precision = 0;
	}
	if (precision)
		f += rounders[precision];
	intPart = f;
	f -= intPart;
	if (!intPart)
		*ptr++ = '0';
	else
	{
		p = ptr;
		while (intPart)
		{
			*p++ = '0' + intPart % 10;
			intPart /= 10;
		}
		p1 = p;
		while (p > ptr)
		{
			c = *--p;
			*p = *ptr;
			*ptr++ = c;
		}
		ptr = p1;
	}
	if (precision)
	{
		*ptr++ = '.';
		while (precision--)
		{
			f *= 10.0;
			c = f;
			*ptr++ = '0' + c;
			f -= c;
		}
	}
	*ptr = 0;
	return ptr;
}
