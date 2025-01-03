/*
 *
 *		timer.h
 *		内核定时器程序头文件
 *
 *		2024/7/6 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_TIMER_H_
#define INCLUDE_TIMER_H_

#define MAX_TIMER 500
#define TIMER_FLAGS_ALLOC 1 // 已配置状态
#define TIMER_FLAGS_USING 2 // 定时器运行中

#include "types.h"

struct TIMER {
	struct TIMER *next;
	unsigned int timeout, flags;
	struct FIFO8 *fifo;
	unsigned char data;
	struct task_struct *waiter;
};

struct TIMERCTL {
	unsigned int count, next;
	struct TIMER *t0;
	struct TIMER timers0[MAX_TIMER];
};

/* 根据传入的定时器频率初始化定时器 */
void init_timer(uint32_t timer);

/* 释放指定的定时器 */
void timer_free(struct TIMER *timer);

/* 初始化定时器 */
void timer_init(struct TIMER *timer, struct FIFO8 *fifo, unsigned char data);

/* 设置定时器的超时时间 */
void timer_settime(struct TIMER *timer, unsigned int timeout);

/* 实现sleep函数的内部逻辑 */
void clock_sleep(uint32_t timer);

/* 使当前进程休眠指定的时间 */
void sleep(uint32_t timer);

/* 分配一个定时器结构体 */
struct TIMER *timer_alloc(void);

/* 获取当前秒（带小数后六位） */
double get_current_second(void);

/* 初始化可编程间隔定时器 */
void init_pit(void);

#endif // INCLUDE_TIMER_H_
