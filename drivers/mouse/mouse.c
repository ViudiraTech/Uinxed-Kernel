/*
 *
 *		mouse.c
 *		鼠标驱动
 *
 *		2024/12/6 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "mouse.h"
#include "idt.h"
#include "printk.h"
#include "keyboard.h"
#include "vbe.h"

struct MOUSE_DEC mdec;
unsigned char dat;
int MOUSEX, MOUSEY, MOUSEBTN;
int mx, my;

/* 等待键盘控制器 */
static void kb_wait(void)
{
	uint8_t kb_stat;
	do {
		kb_stat = inb(KB_CMD);
	} while (kb_stat & 0x02);
}

/* 鼠标解码 */
static int mouse_decode(void)
{
	/* 根据当前阶段处理接收到的数据 */
	if (mdec.phase == 0) {
		/* 等待鼠标发送0xfa的阶段 */
		if (dat == 0xfa) {
			mdec.phase = 1;
		}
		return 0;
	}
	if (mdec.phase == 1) {
		/* 等待鼠标第一字节数据的阶段 */
		if ((dat & 0xc8) == 0x08) {
			mdec.buf[0] = dat;
			mdec.phase = 2;
		}
		return 0;
	}
	if (mdec.phase == 2) {
		/* 等待鼠标第二字节数据的阶段 */
		mdec.buf[1] = dat;
		mdec.phase = 3;
		return 0;
	}
	if (mdec.phase == 3) {
		/* 等待鼠标第三字节数据的阶段 */
		mdec.buf[2] = dat;
		mdec.phase = 1;

		/* 解码按钮状态 */
		mdec.btn = mdec.buf[0] & 0x07;

		/* 解码x和y坐标 */
		mdec.x = mdec.buf[1];
		mdec.y = mdec.buf[2];

		/* 如果某一位被设置，则将坐标扩展为负数 */
		if ((mdec.buf[0] & 0x10) != 0) {
			mdec.x |= 0xffffff00;
		}
		if ((mdec.buf[0] & 0x20) != 0) {
			mdec.y |= 0xffffff00;
		}

		/* 鼠标的y方向与画面符号相反，进行转换 */
		mdec.y = -mdec.y;
		return 1;
	}
	return -1;
}

/* 鼠标中断处理 */
void mouse_handler(struct interrupt_frame *frame)
{
	dat = inb(0x60);
	if (mouse_decode() != 0) {
		mx += mdec.x;
		my += mdec.y;
		if (mx < 0) {
			mx = 0;
		}
		if (my < 0) {
			my = 0;
		}
		if (mx > vbe_get_width()) {
			mx = vbe_get_width();
		}
		if (my > vbe_get_height()) {
			my = vbe_get_height();
		}
		MOUSEX = mx;
		MOUSEY = my;
		MOUSEBTN = mdec.btn;
	}
}

/* PS/2鼠标初始化 */
void mouse_init(void)
{
	print_busy("Initializing PS/2 mouse controller...\r"); // 提示用户正在初始化鼠标接口，并回到行首等待覆盖

	kb_wait();
	outb(KB_CMD, 0x60);
	kb_wait();
	outb(KB_DATA, 0x47);

	kb_wait();
	outb(KB_CMD, 0xd4);
	kb_wait();
	outb(KB_DATA, 0xf4);

	register_interrupt_handler(0x20 + 12, &mouse_handler);
	mdec.phase = 0;

	print_succ("PS/2 mouse controller initialized successfully.\n");
}

/* 获取鼠标X轴 */
int get_mouse_x(void)
{
	return MOUSEX;
}

/* 获取鼠标Y轴 */
int get_mouse_y(void)
{
	return MOUSEY;
}

/* 获取鼠标按钮状态 */
int get_mouse_btn(void)
{
	return MOUSEBTN;
}
