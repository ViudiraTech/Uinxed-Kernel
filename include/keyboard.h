/*
 *
 *		keyboard.h
 *		键盘驱动头文件
 *
 *		2024/2/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_KEYBOARD_H_
#define INCLUDE_KEYBOARD_H_

#include "fifo.h"

extern fifo_t keyfifo, decoded_key;

void init_keyboard(void);							// 键盘初始化函数

#define NR_SCAN_CODES	0x80
#define MAP_COLS		3

#define FLAG_BREAK		0x0080
#define FLAG_EXT		0x0100
#define FLAG_SHIFT_L	0x0200
#define FLAG_SHIFT_R	0x0400
#define FLAG_CTRL_L		0x0800
#define FLAG_CTRL_R		0x1000
#define FLAG_ALT_L		0x2000
#define FLAG_ALT_R		0x4000
#define FLAG_PAD		0x8000

#define MASK_RAW		0x1ff

#define ESC				(0x01 + FLAG_EXT)
#define TAB				(0x02 + FLAG_EXT)
#define ENTER			(0x03 + FLAG_EXT)
#define BACKSPACE		(0x04 + FLAG_EXT)

#define GUI_L			(0x05 + FLAG_EXT)
#define GUI_R			(0x06 + FLAG_EXT)
#define APPS			(0x07 + FLAG_EXT)

#define SHIFT_L			(0x08 + FLAG_EXT)
#define SHIFT_R			(0x09 + FLAG_EXT)
#define CTRL_L			(0x0A + FLAG_EXT)
#define CTRL_R			(0x0B + FLAG_EXT)
#define ALT_L			(0x0C + FLAG_EXT)
#define ALT_R			(0x0D + FLAG_EXT)

#define CAPS_LOCK 		(0x0E + FLAG_EXT)
#define NUM_LOCK		(0x0F + FLAG_EXT)
#define SCROLL_LOCK		(0x10 + FLAG_EXT)

#define F1				(0x11 + FLAG_EXT)
#define F2				(0x12 + FLAG_EXT)
#define F3				(0x13 + FLAG_EXT)
#define F4				(0x14 + FLAG_EXT)
#define F5				(0x15 + FLAG_EXT)
#define F6				(0x16 + FLAG_EXT)
#define F7				(0x17 + FLAG_EXT)
#define F8				(0x18 + FLAG_EXT)
#define F9				(0x19 + FLAG_EXT)
#define F10				(0x1A + FLAG_EXT)
#define F11				(0x1B + FLAG_EXT)
#define F12				(0x1C + FLAG_EXT)

#define PRINTSCREEN		(0x1D + FLAG_EXT)
#define PAUSEBREAK		(0x1E + FLAG_EXT)
#define INSERT			(0x1F + FLAG_EXT)
#define DELETE			(0x20 + FLAG_EXT)
#define HOME			(0x21 + FLAG_EXT)
#define END				(0x22 + FLAG_EXT)
#define PAGEUP			(0x23 + FLAG_EXT)
#define PAGEDOWN		(0x24 + FLAG_EXT)
#define UP				(0x25 + FLAG_EXT)
#define DOWN			(0x26 + FLAG_EXT)
#define LEFT			(0x27 + FLAG_EXT)
#define RIGHT			(0x28 + FLAG_EXT)

#define POWER			(0x29 + FLAG_EXT)
#define SLEEP			(0x2A + FLAG_EXT)
#define WAKE			(0x2B + FLAG_EXT)

#define PAD_SLASH		(0x2C + FLAG_EXT)		// 小键盘
#define PAD_STAR 		(0x2D + FLAG_EXT)
#define PAD_MINUS		(0x2E + FLAG_EXT)
#define PAD_PLUS		(0x2F + FLAG_EXT)
#define PAD_ENTER		(0x30 + FLAG_EXT)
#define PAD_DOT			(0x31 + FLAG_EXT)
#define PAD_0			(0x32 + FLAG_EXT)
#define PAD_1			(0x33 + FLAG_EXT)
#define PAD_2			(0x34 + FLAG_EXT)
#define PAD_3			(0x35 + FLAG_EXT)
#define PAD_4			(0x36 + FLAG_EXT)
#define PAD_5			(0x37 + FLAG_EXT)
#define PAD_6			(0x38 + FLAG_EXT)
#define PAD_7			(0x39 + FLAG_EXT)
#define PAD_8			(0x3A + FLAG_EXT)
#define PAD_9			(0x3B + FLAG_EXT)

#define PAD_UP			PAD_8
#define PAD_DOWN		PAD_2
#define PAD_LEFT		PAD_4
#define PAD_RIGHT		PAD_6
#define PAD_HOME		PAD_7
#define PAD_END			PAD_1
#define PAD_PAGEUP		PAD_9
#define PAD_PAGEDOWN	PAD_3
#define PAD_INS			PAD_0
#define PAD_MID			PAD_5
#define PAD_DEL			PAD_DOT

#define KB_DATA			0x60
#define KB_CMD			0x64
#define LED_CODE		0xED
#define KB_ACK			0xFA

#endif // INCLUDE_KEYBPOARD_H_
