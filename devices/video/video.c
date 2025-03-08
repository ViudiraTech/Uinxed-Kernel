/*
 *
 *		video.c
 *		基本视频驱动
 *
 *		2024/9/16 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "limine.h"
#include "video.h"
#include "string.h"
#include "common.h"

__attribute__((used, section(".limine_requests")))
static __volatile__ struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

extern uint8_t ascfont[];	// 字库

uint64_t width;				// 屏幕长
uint64_t height;			// 屏幕宽
uint64_t stride;			// 帧缓冲区行间距
uint32_t *buffer;			// 显存

int32_t x, y;				// 当前光标的绝对位置
int32_t cx, cy;				// 当前光标的字符位置
uint32_t c_width, c_height;	// 屏幕的字符宽高

uint32_t fore_color;		// 前景色
uint32_t back_color;		// 背景色

/* 初始化视频驱动 */
void video_init(void)
{
	if (framebuffer_request.response == 0 || framebuffer_request.response->framebuffer_count < 1) {
		krn_halt();
	}
	struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
	buffer	= framebuffer->address;
	width	= framebuffer->width;
	height	= framebuffer->height;
	stride	= framebuffer->pitch / 4;

	x = 2;
	y = cx = cy = 0;
	c_width = width / 9;
	c_height = height / 16;

	fore_color = 0xffffffff;
	back_color = 0xff000000;
	video_clear();
}

/* 获取帧缓冲区 */
struct limine_framebuffer *get_framebuffer(void)
{
	return framebuffer_request.response->framebuffers[0];
}

/* 清屏 */
void video_clear(void)
{
	for (uint32_t i = 0; i < (width * height); i++) {
		buffer[i] = 0xff000000;
	}
	back_color = 0xff000000;
	x = 2;
	y = 0;
	cx = cy = 0;
}

/* 带颜色清屏 */
void video_clear_color(int color)
{
	for (uint32_t i = 0; i < (width * height); i++) {
		buffer[i] = color;
	}
	back_color = color;
	x = 2;
	y = 0;
	cx = cy = 0;
}

/* 屏幕滚动操作 */
inline void video_scroll(void)
{
	if ((uint32_t)cx > c_width) {
		cx = 0;
		cy++;
	} else cx++;
	if ((uint32_t)cy >= c_height) {
		const uint32_t *sr = buffer + stride * 16;
		uint32_t *dst = buffer;
		for (unsigned long len = (height - 16) * stride; len > 0; len--) {
			*dst++ = *sr++;
		}
		memset(buffer + (height - 16) * stride, back_color, 16 * stride * sizeof(uint32_t));
		cy = c_height - 1;
	}
}

/* 在屏幕指定坐标绘制一个像素 */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
	if (x >= width || y >= height) {
		return;
	}
	uint32_t *p = (uint32_t *)buffer + y * width + x;
	*p = color;
}

/* 在屏幕指定坐标绘制一个矩阵 */
void video_draw_rect(int x0, int y0, int x1, int y1, int color)
{
	int x, y;
	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++) {
			(buffer)[y * width + x] = color;
		}
	}
}

/* 在屏幕指定坐标绘制一个字符 */
void video_draw_char(char c, int32_t x, int32_t y, int color)
{
	uint8_t *font = ascfont;
	font += c * 16;
	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 9; j++) {
			if (font[i] & (0x80 >> j)) {
				buffer[(y + i) * width + x + j] = color;
			} else buffer[(y + i) * width + x + j] = back_color;
		}
	}
}

/* 在屏幕指定坐标打印一个字符 */
void video_put_char(char c, int color)
{
	if (c == '\n') {
		video_scroll();
		cx = 0;
		cy++;
		return;
	} else if (c == '\r') {
		cx = 0;
		return;
	} else if(c == '\t') {
		for (int i = 0; i < 4; i++) video_put_char(' ', color);
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
		video_draw_rect(x, y, x + 9, y + 16, back_color);
		return;
	}
	video_scroll();
	video_draw_char(c, cx * 9 - 7, cy * 16, color);
}

/* 在屏幕指定坐标打印一个字符串 */
void video_put_string(const char *str)
{
	for (; *str; ++str) {
		char c = *str;
		video_put_char(c, fore_color);
	}
}

/* 带颜色在屏幕指定坐标打印一个字符串 */
void video_put_string_color(const char *str, int color)
{
	for (; *str; ++str) {
		char c = *str;
		video_put_char(c, color);
	}
}
