/*
 *
 *		printk.h
 *		内核字符串打印头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_PRINTK_H_
#define INCLUDE_PRINTK_H_

#include "vargs.h"

/* 内核打印字符串 */
void printk(const char *format, ...);

/* 内核打印带颜色的字符串 */
void printk_color(int fore, const char *format, ...);

/* 内核打印日志 */
void plogk(const char *format, ...);

/* 内核打印带颜色的日志  */
void plogk_color(int fore, const char *format, ...);

/* 将格式化的输出存储在字符数组中 */
void sprintf(char *str, const char *fmt, ...);

/* 格式化字符串并将其输出到一个字符数组中 */
int vsprintf(char *buff, const char *format, va_list args);

#endif // INCLUDE_PRINTK_H_
