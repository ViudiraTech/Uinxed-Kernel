/*
 *
 *		debug.h
 *		内核调试头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_DEBUG_H_
#define INCLUDE_DEBUG_H_

#define assert(exp) \
	if (!exp) assertion_failure(#exp, __FILE__, __LINE__)

/* 内核异常 */
void panic(const char *format, ...);

/* 断言失败 */
void assertion_failure(const char *exp, const char *file, int line);

#endif // INCLUDE_DEBUG_H_
