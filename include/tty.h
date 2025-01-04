/*
 *
 *		tty.h
 *		终端设备头文件
 *
 *		2024/12/17 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_TTY_H_
#define INCLUDE_TTY_H_

#include "fifo.h"
#include "types.h"

#define TTY_MAX 64

typedef enum {
	MODE_A = 'A',
	MODE_B = 'B',
	MODE_C = 'C',
	MODE_D = 'D',
	MODE_E = 'E',
	MODE_F = 'F',
	MODE_G = 'G',
	MODE_H = 'H',
	MODE_f = 'f',
	MODE_J = 'J',
	MODE_K = 'K',
	MODE_S = 'S',
	MODE_T = 'T',
	MODE_m = 'm'
} vt100_mode_t;

typedef struct tty_device {
	void (*print)(struct tty_device *resource, const char *string);
	void (*putchar)(struct tty_device *resource, int c);
	void (*clear)(struct tty_device *resource, int c);
	uint32_t volatile *video_memory;
	uint32_t width, height;
	int x, y;
	uint32_t fore_color, back_color;
	struct FIFO *fifo;
	char name[50];
} tty_t;

/* 获取启动时传入的tty号 */
char *get_boot_tty(void);

/* 打印日志字符到TTY */
void tty_print_logch(const char ch);

/* 打印日志字符串到TTY */
void tty_print_logstr(const char *str);

#endif // INCLUDE_TTY_H_
