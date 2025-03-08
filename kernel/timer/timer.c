/*
 *
 *		timer.c
 *		定时器
 *
 *		2025/2/17 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "acpi.h"
#include "idt.h"
#include "common.h"
#include "interrupt.h"
#include "apic.h"

/* 定时器中断 */
__attribute__((interrupt)) void timer_handle(interrupt_frame_t *frame)
{
	(void)frame;
	disable_intr();
	send_eoi();
	enable_intr();
}

/* 基于微秒的延迟函数 */
void sleep(uint64_t micro)
{
	uint64_t targetTime = nanoTime();
	uint64_t after = 0;
	uint64_t nano = micro * 1000000;

	while (1) {
		uint64_t n = nanoTime();
		if (n < targetTime) {
			after += 0xffffffff - targetTime + n;
			targetTime = n;
		} else {
			after += n - targetTime;
			targetTime = n;
		}
		if (after >= nano) {
			return;
		}
	}
}

/* 基于纳秒的延迟函数 */
void usleep(uint64_t nano)
{
	uint64_t targetTime = nanoTime();
	uint64_t after = 0;

	while (1) {
		uint64_t n = nanoTime();
		if (n < targetTime) {
			after += 0xffffffff - targetTime + n;
			targetTime = n;
		} else {
			after += n - targetTime;
			targetTime = n;
		}
		if (after >= nano) {
			return;
		}
	}
}
