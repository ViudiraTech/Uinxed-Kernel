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

#define assert(x, info)		\
	do {					\
		if (!(x)) {			\
			panic(info);	\
		}					\
	} while (0)

/* 初始化 Debug 信息 */
void init_debug(void);

/* 打印当前的函数调用栈信息 */
void panic(const char *msg);

/* 打印当前的段存器值 */
void print_cur_status();

/* 内核的打印函数 */
void printk(const char *format, ...);

/* 内核的打印函数 带颜色 */
void printk_color(real_color_t back, real_color_t fore, const char *format, ...);

/* 打印SUCCESS信息 */
void print_succ(char *str);

/* 打印WARNING信息 */
void print_warn(char *str);

/* 打印 ERROR 信息 */
void print_erro(char *str);

#endif // INCLUDE_DEBUG_H_
