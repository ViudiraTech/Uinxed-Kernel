/*
 *
 *		keyboard.h
 *		键盘驱动头文件
 *
 *		2024/2/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#ifndef INCLUDE_KEYBOARD_H_
#define INCLUDE_KEYBOARD_H_

#include "fifo.h"

extern fifo_t terminal_key;

void getch(char *ch);		// 等待键盘传来的字符
void init_keyboard(void);	// 初始化键盘驱动器

#define KB_DATA		0x60
#define KB_CMD		0x64
#define LED_CODE	0xED
#define KB_ACK		0xFA

#endif // INCLUDE_KEYBPOARD_H_
