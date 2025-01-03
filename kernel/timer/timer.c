/*
 *
 *		timer.c
 *		内核定时器程序
 *
 *		2024/7/6 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "timer.h"
#include "common.h"
#include "cmos.h"
#include "printk.h"
#include "idt.h"
#include "sched.h"

volatile uint32_t tick = 0;
struct TIMERCTL timerctl;

/* 获取当前秒（带小数后六位） */
double get_current_second(void)
{
	unsigned int current_second = get_sec_hex();
	unsigned int current_tick = tick;
	double fractional_part = (double)current_tick / 1000.0;
	double current_second_with_fraction = current_second + fractional_part;
	return current_second_with_fraction;
}

/* 定时器中断处理函数 */
static void timer_handle(pt_regs *regs)
{
	disable_intr();
	tick++;
	schedule(regs);
	enable_intr();
}

/* 使当前进程休眠指定的时间 */
void sleep(uint32_t timer)
{
	clock_sleep(timer);
}

/* 实现sleep函数的内部逻辑 */
void clock_sleep(uint32_t timer)
{
	uint32_t sleep = tick + timer;
	while (1) {
		__asm__ ("hlt");
		if (tick >= sleep) break;
	}
}

/* 初始化可编程间隔定时器 */
void init_pit(void)
{
	outb(0x43, 0x34);
	outb(0x40, 0x9c);
	outb(0x40, 0x2e);

	int i;
	struct TIMER *t;
	timerctl.count = 0;
	for (i = 0; i < MAX_TIMER; i++) {
		timerctl.timers0[i].flags = 0;	// 没有使用
	}
	t = timer_alloc();					// 取得一个
	t->timeout		= 0xffffffff;
	t->flags		= TIMER_FLAGS_USING;
	t->next			= 0;				// 末尾
	timerctl.t0		= t;				// 因为现在只有哨兵，所以他就在最前面
	timerctl.next	= 0xffffffff;		// 因为只有哨兵，所以下一个超时时刻就是哨兵的时刻
	return;
}

/* 根据传入的定时器频率初始化定时器 */
void init_timer(uint32_t timer)
{
	register_interrupt_handler(IRQ0, &timer_handle);
	uint32_t divisor = 1193180 / timer;

	outb(0x43, 0x36); // 频率

	uint8_t l = (uint8_t) (divisor & 0xFF);
	uint8_t h = (uint8_t) ((divisor >> 8) & 0xFF);

	outb(0x40, l);
	outb(0x40, h);
}

/* 分配一个定时器结构体 */
struct TIMER *timer_alloc(void)
{
	int i;
	for (i = 0; i < MAX_TIMER; i++) {
		if (timerctl.timers0[i].flags == 0) {
			timerctl.timers0[i].flags = TIMER_FLAGS_ALLOC;
			timerctl.timers0[i].waiter = 0;
			return &timerctl.timers0[i];
		}
	}
	return 0; // 没找到
}

/* 释放指定的定时器 */
void timer_free(struct TIMER *timer)
{
	timer->flags = 0; // 未使用
	timer->waiter = 0;
	return;
}

/* 初始化定时器 */
void timer_init(struct TIMER *timer, struct FIFO8 *fifo, unsigned char data)
{
	timer->fifo = fifo;
	timer->data = data;
	return;
}

/* 设置定时器的超时时间 */
void timer_settime(struct TIMER *timer, unsigned int timeout)
{
	int e;
	struct TIMER *t, *s;
	timer->timeout	= timeout + timerctl.count;
	timer->flags	= TIMER_FLAGS_USING;
	e = load_eflags();
	t = timerctl.t0;
	if (timer->timeout <= t->timeout) {
		/* 插入最前面的情况 */
		timerctl.t0 = timer;
		timer->next = t; // 下面是设定t
		timerctl.next = timer->timeout;
		store_eflags(e);
		return;
	}
	for (;;) {
		s = t;
		t = t->next;
		if (timer->timeout <= t->timeout) {
			/* 插入s和t之间的情况 */
			s->next = timer; // s下一个是timer
			timer->next = t; // timer的下一个是t
			store_eflags(e);
			return;
		}
	}
}
