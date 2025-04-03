/*
 *
 *		parallel.c
 *		Parallel Port
 *
 *		2024/9/8 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "timer.h"
#include "common.h"
#include "parallel.h"

/* Waiting for the parallel port to become ready */
void wait_parallel_ready(void)
{
	while ((!inb(LPT1_PORT_STATUS)) & 0x80) {
		sleep(10);
	}
}

/* Write to parallel port */
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
