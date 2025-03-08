/*
 *
 *		video.h
 *		基本视频驱动
 *
 *		2024/9/16 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_VIDEO_H_
#define INCLUDE_VIDEO_H_

#include "limine.h"
#include "stdint.h"

/* 初始化视频驱动 */
void video_init(void);

/* 获取帧缓冲区 */
struct limine_framebuffer *get_framebuffer(void);

/* 清屏 */
void video_clear(void);

/* 带颜色清屏 */
void video_clear_color(int color);

/* 屏幕滚动操作 */
void video_scroll(void);

/* 在屏幕指定坐标绘制一个像素 */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color);

/* 在屏幕指定坐标绘制一个矩阵 */
void video_draw_rect(int x0, int y0, int x1, int y1, int color);

/* 在屏幕指定坐标绘制一个字符 */
void video_draw_char(char c, int32_t x, int32_t y, int color);

/* 在屏幕指定坐标打印一个字符 */
void video_put_char(char c, int color);

/* 在屏幕指定坐标打印一个字符串 */
void video_put_string(const char *str);

/* 带颜色在屏幕指定坐标打印一个字符串 */
void video_put_string_color(const char *str, int color);

#endif // INCLUDE_VIDEO_H_
