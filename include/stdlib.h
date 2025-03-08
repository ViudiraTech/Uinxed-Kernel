/*
 *
 *		stdlib.h
 *		通用工具函数库头文件
 *
 *		2024/10/2 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_STDLIB_H_
#define INCLUDE_STDLIB_H_

#include "stdint.h"

#define ZEROPAD		1	// pad with zero
#define SIGN		2 	// unsigned/signed long
#define PLUS    	4	// show plus
#define SPACE		8 	// space if plus
#define LEFT		16	// left justified
#define SPECIAL		32	// 0x
#define SMALL		64	// use 'abcdef' instead of 'ABCDEF'

#define do_div(n, base) ({																\
        int64_t __res;																	\
        __asm__("divq %4" : "=a"(n), "=d"(__res) : "0"(n), "1"(0), "r"((int64_t)base));	\
        __res; })

/* 判断是否是数字 */
int is_digit(int c);

/* 将字符串数字转换为整数数字 */
int atoi(char *pstr);

/* 跳过字符串中的数字并将这些连续数字的值返回 */
int skip_atoi(const char **s);

/* 将整数格式化为字符串 */
char *number(char *str, int64_t num, int base, int size, int precision, int type);

#endif // INCLUDE_STDLIB_H_
