/*
 *
 *		ctypes.h
 *		C语言字符分类函数库头文件
 *
 *		2024/11/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_CTYPES_H_
#define INCLUDE_CTYPES_H_

#include "types.h"

/* 判断是否是空白字符 */
bool isspace(int c);

/* 判断是否是数字 */
bool isdigit(int c);

#endif // INCLUDE_CTYPES_H_
