/*
 *
 *		vbe.c
 *		VBE图形模式驱动
 *
 *		2024/9/16 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "vbe.h"
#include "string.h"
#include "serial.h"
#include "printk.h"
#include "memory.h"
#include "lib_os_terminal.lib.h"

uint32_t width, height;
uint32_t c_width, c_height;			// 字符绘制总宽高
int32_t x, y;
int32_t cx, cy;						// 字符坐标
uint32_t fore_color, back_color;
uint32_t *screen;
uint32_t *char_buffer;

extern uint8_t ascfont[];
extern uint8_t plfont[];
extern uint8_t bafont[];

int vbe_status = 0;
int vbe_serial = 0;
int terminalMode = 0;
/* 0 vbe */
/* 1 OS-Terminal */

/* VBE图形模式清屏（默认颜色） */
void vbe_clear(void)
{
	for (uint32_t i = 0; i < (width * (height)); i++) {
		screen[i] = back_color;
	}
	x = 2;
	y = 0;
	cx = cy = 0;
}

/* VBE图形模式清屏（带颜色） */
void vbe_clear_color(int color)
{
	vbe_set_back_color(color);
	for (uint32_t i = 0; i < (width * (height)); i++) {
		screen[i] = color;
	}
	x = 2;
	y = 0;
	cx = cy = 0;
}

/* OS-Terminal清屏 */
void screen_clear(void)
{
	printk("\033[H\033[2J\033[3J");
	// vbe_clear();
}

/* 打印一个空行 */
void vbe_write_newline(void)
{
	printk("\n");
}

/* VBE图形模式屏幕滚动操作 */
void vbe_scroll(void)
{
	if ((uint32_t)cx > c_width) {
		cx = 0;
		cy++;
	} else cx++;
	if ((uint32_t)cy >= c_height) {
		cy = c_height - 1;
		memcpy((void *)screen, (void *)screen + width * 16 * sizeof(uint32_t), width * (height - 16) * sizeof(uint32_t));
		for (uint32_t i = (width * (height - 16)); i != (width * height); i++) {
			screen[i] = back_color;
		}
	}
}

/* 在图形界面上进行像素绘制 */
void vbe_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
	if (x >= width || y >= height) {
		return;
	}
	color = (color & 0xff) | (color & 0xff00) | (color & 0xff0000);
	uint32_t  *p = (uint32_t *)screen + y * width + x;
	*p = color;
}

/* 在图形界面指定坐标绘制一个矩阵 */
void vbe_draw_rect(int x0, int y0, int x1, int y1, int color)
{
	int x, y;
	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++) {
			(screen)[y * width + x] = color;
		}
	}
}

/* 在图形界面指定坐标上显示字符 */
void vbe_draw_char(char c, int32_t x, int32_t y, int color)
{
	uint8_t *font = ascfont;
	font += c * 16;

	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 9; j++) {
			if (font[i] & (0x80 >> j)) {
				screen[(y + i) * width + x + j] = color;
			} else screen[(y + i) * width + x + j] = back_color;
		}
	}
}

/* 在图形界面指定坐标上打印字符 */
void vbe_put_char(char c, int color)
{
	// if (vbe_serial == 1) write_serial(c); // 输出控制台到串口设备（此处会导致某些计算机异常卡顿）
	if (c == '\n') {
		vbe_scroll();
		cx = 0;
		cy++;
		return;
	} else if (c == '\r') {
		cx = 0;
		return;
	} else if(c == '\t') {
		for (int i = 0; i < 4; i++) vbe_put_char(' ', color);
		return;
	} else if (c == '\b' && cx > 0) {
		cx -= 1;
		if (cx == 0) {
			cx = c_width - 1;
			if (cy != 0) cy -= 1;
			if (cy == 0) cx = 0, cy = 0;
		}
		int x = (cx+1) * 9 - 7;
		int y = cy * 16;
		vbe_draw_rect(x, y, x + 9, y + 16, back_color);
		return;
	}
	vbe_scroll();
	vbe_draw_char(c, cx * 9 - 7, cy * 16, color);
}

/* 在图形界面指定坐标上打印字符串（默认颜色） */
void vbe_put_string(const char *str)
{
	for (;*str; ++str) {
		char c = *str;
		vbe_put_char(c, fore_color);
	}
}

/* 在图形界面指定坐标上打印字符串（带颜色） */
void vbe_put_string_color(const char *str, int color)
{
	for (;*str; ++str) {
		char c = *str;
		vbe_put_char(c, color);
	}
}

/* VBE输出映射到串口 */
void vbe_to_serial(int op)
{
	if (op == 1)
		vbe_serial = 1;
	else
		vbe_serial = 0;
}

/* 设置前景色 */
void vbe_set_fore_color(int color)
{
	fore_color = color;
}

/* 设置背景色 */
void vbe_set_back_color(int color)
{
	back_color = color;
}

/* 初始化VBE图形模式 */
void init_vbe(multiboot_t *info, int back, int fore)
{
	vbe_status = 1;
	x = 2;
	y = cx	= cy = 0;
	screen	= (uint32_t *)(uintptr_t)info->framebuffer_addr;
	width	= info->framebuffer_width;
	height	= info->framebuffer_height;
	vbe_set_fore_color(fore);
	vbe_set_back_color(back);
	c_width = width / 9;
	c_height = height / 16;
	vbe_clear();

	TerminalDisplay display = {
		.width = width,
		.height = height,
		.address = screen
	};
	terminal_init(&display, 10.0, (void *)kmalloc, kfree, write_serial_string);	// 初始化terminal
	terminalMode = 1;															// 开启terminal
}
