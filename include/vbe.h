/*
 *
 *		vbe.h
 *		VBE图形模式驱动头文件
 *
 *		2024/9/16 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_VBE_H_
#define INCLUDE_VBE_H_

#include "types.h"
#include "multiboot.h"

// RRRRR GGGGGG BBBBB
#define SVGA_24TO16BPP(x) ((x & 0xF80000) >> 8) | ((x & 0xFC00) >> 5) | ((x & 0xF8) >> 3)

/* VBE图形模式清屏（默认颜色） */
void vbe_clear(void);

/* VBE图形模式清屏（带颜色） */
void vbe_clear_color(int color);

/* OS-Terminal清屏 */
void screen_clear(void);

/* 打印一个空行 */
void vbe_write_newline(void);

/* VBE图形模式屏幕滚动操作 */
void vbe_scroll(void);

/* 在图形界面上进行像素绘制 */
void vbe_draw_pixel(uint32_t x, uint32_t y, uint32_t color);

/* 在图形界面指定坐标绘制一个矩阵 */
void vbe_draw_rect(int x0, int y0, int x1, int y1, int color);

/* 在图形界面指定坐标上显示字符 */
void vbe_draw_char(char c, int32_t x, int32_t y, int color);

/* 在图形界面指定坐标上打印字符 */
void vbe_put_char(char c, int color);

/* 在图形界面指定坐标上打印字符串（默认颜色） */
void vbe_put_string(const char *str);

/* 在图形界面指定坐标上打印字符串（带颜色） */
void vbe_put_string_color(const char *str, int color);

/* VBE输出映射到串口 */
void vbe_to_serial(int op);

/* 设置前景色 */
void vbe_set_fore_color(int color);

/* 设置背景色 */
void vbe_set_back_color(int color);

/* 获取VBE宽 */
uint32_t vbe_get_width(void);

/* 获取VBE高 */
uint32_t vbe_get_height(void);

/* 初始化VBE图形模式 */
void init_vbe(multiboot_t *info, int back, int fore);

#endif // INCLUDE_VBE_H_
