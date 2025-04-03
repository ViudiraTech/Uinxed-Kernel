/*
 *
 *		timer.c
 *		Timer
 *
 *		2025/2/17 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "acpi.h"
#include "idt.h"
#include "common.h"
#include "interrupt.h"
#include "apic.h"

/* Timer interrupt */
__attribute__((interrupt)) void timer_handle(interrupt_frame_t *frame)
{
	(void)frame;
	disable_intr();
	send_eoi();
	enable_intr();
}

/* Microsecond-based delay functions */
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

/* Nanosecond-based delay function */
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
