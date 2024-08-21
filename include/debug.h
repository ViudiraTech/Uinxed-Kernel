/*
 *
 *		debug.h
 *		内核调试程序头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_DEBUG_H_
#define INCLUDE_DEBUG_H_

#include "console.h"

#define assert(exp)	\
    if (exp);		\
    else			\
		assertion_failure(#exp, __FILE__, get_filename(__FILE__), __LINE__)

/* 初始化 Debug 信息 */
void init_debug(void);

/* 打印当前的段存器值 */
void print_cur_status(void);

/* 内核恐慌 */
void panic(const char *msg);

/* 打印内核堆栈跟踪 */
void print_stack_trace(void);

/* 强制阻塞 */
void spin(char *name);

/* 断言失败 */
void assertion_failure(char *exp, char *file, char *base, int line);

#endif // INCLUDE_DEBUG_H_
