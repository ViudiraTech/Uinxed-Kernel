// printk.h -- 内核调试和打印信息程序头文件（基于 GPL-3.0 开源协议）
// Copyright © 2020 ViudiraTech，保留所有权利。
// 源于 小严awa 撰写于 2024-6-27.

#include "console.h"
#include "vargs.h"

/* 内核的打印函数 */
void printk(const char *format, ...);

/* 内核的打印函数 带颜色 */
void printk_color(real_color_t back, real_color_t fore, const char *format, ...);

void print_succ(char *str);
void print_info(char *str);
void print_warn(char *str);
void print_erro(char *str);
