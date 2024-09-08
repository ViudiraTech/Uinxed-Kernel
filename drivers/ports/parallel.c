/*
 *
 *		parallel.c
 *		并行端口驱动程序
 *
 *		2024/9/8 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "timer.h"
#include "common.h"
#include "parallel.h"

void wait_parallel_ready(void)
{
	while (!inb(LPT1_PORT_STATUS) & 0x80) {
		sleep(10);
	}
}

void parallel_write(unsigned char c)
{
	unsigned char lControl;

	wait_parallel_ready();

	outb(LPT1_PORT_BASE, c);
	lControl = inb(LPT1_PORT_CONTROL);
	outb(LPT1_PORT_CONTROL, lControl | 1);
	sleep(10);
	outb(LPT1_PORT_CONTROL, lControl);

	wait_parallel_ready();
}
