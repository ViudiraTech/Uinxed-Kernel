/*
 *
 *		mouse.h
 *		鼠标驱动头文件
 *
 *		2024/12/6 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#ifndef INCLUDE_MOUSE_H_
#define INCLUDE_MOUSE_H_

#include "types.h"

struct MOUSE_DEC {
	unsigned char buf[3], phase;
	int x, y, btn;
};

/* PS/2鼠标初始化 */
void mouse_init(void);

/* 获取鼠标X轴 */
int get_mouse_x(void);

/* 获取鼠标Y轴 */
int get_mouse_y(void);

/* 获取鼠标按钮状态 */
int get_mouse_btn(void);

#endif // INCLUDE_MOUSE_H_
