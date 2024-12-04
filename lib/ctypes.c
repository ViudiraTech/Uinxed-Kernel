/*
 *
 *		ctypes.c
 *		C语言字符分类函数库
 *
 *		2024/11/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "ctypes.h"

/* 判断是否是空白字符 */
int isspace(int c)
{
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f');
}

/* 判断是否是数字 */
int isdigit(int c)
{
	return (c >= '0' && c <= '9');
}
