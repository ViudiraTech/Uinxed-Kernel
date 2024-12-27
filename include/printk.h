/*
 *
 *		printk.h
 *		内核调试和打印信息程序头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_PRINTK_H_
#define INCLUDE_PRINTK_H_

#include "vargs.h"

/* VBE */

/* 内核打印字符串 */
void vbe_printk(const char *format, ...);

/* 内核打印带颜色的字符串 */
void vbe_printk_color(int fore, const char *format, ...);

/* 带前缀的打印函数 */
void vbe_print_busy(const char *str); // 打印带有”[ ** ]“的字符串
void vbe_print_succ(const char *str); // 打印带有”[ OK ]“的字符串
void vbe_print_warn(const char *str); // 打印带有”[WARN]“的字符串
void vbe_print_erro(const char *str); // 打印带有”[ERRO]“的字符串
void vbe_print_time(const char *str); // 打印带有"[HH:MM:SS]"的字符串

/* OS-Terminal */

/* 内核打印字符 */
void putchar(int ch);

/* 内核打印字符串 */
void printk(const char *format, ...);

/* 带前缀的打印函数 */
void print_busy(const char *str); // 打印带有”[ ** ]“的字符串
void print_succ(const char *str); // 打印带有”[ OK ]“的字符串
void print_warn(const char *str); // 打印带有”[WARN]“的字符串
void print_erro(const char *str); // 打印带有”[ERRO]“的字符串
void print_time(const char *str); // 打印带有"[HH:MM:SS]"的字符串

/* 格式化字符串并将其输出到一个字符数组中 */
int vsprintf(char *buff, const char *format, va_list args);

/* 将格式化的输出存储在字符数组中 */
void sprintf(char *str,const char *fmt, ...);

#endif // INCLUDE_PRINTK_H_
