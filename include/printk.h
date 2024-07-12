/*
 *
 *		printk.h
 *		内核调试和打印信息程序头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */


#include "console.h"
#include "vargs.h"

/* 内核打印字符串 */
void printk(const char *format, ...);

/* 内核打印带颜色的字符串 */
void printk_color(real_color_t back, real_color_t fore, const char *format, ...);

/* 带前缀的打印函数 */
void print_busy(char *str); // 打印带有”[ ** ]“的字符串
void print_succ(char *str); // 打印带有”[ OK ]“的字符串
void print_warn(char *str); // 打印带有”[WARN]“的字符串
void print_erro(char *str); // 打印带有”[ERRO]“的字符串
