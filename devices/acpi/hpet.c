/*
 *
 *		hpet.c
 *		高精度事件计时器
 *
 *		2025/2/16 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "acpi.h"
#include "hhdm.h"
#include "printk.h"
#include "idt.h"

HpetInfo *hpet_addr;
static uint32_t hpetPeriod = 0;

void timer_handle(interrupt_frame_t *frame);

/* 返回当前时间的纳秒值 */
uint64_t nanoTime(void)
{
	if (hpet_addr == 0) return 0;
	uint64_t mcv = hpet_addr->mainCounterValue;
	return mcv * hpetPeriod;
}

/* 初始化高精度事件计时器 */
void hpet_init(Hpet *hpet)
{
	hpet_addr = phys_to_virt(hpet->base_address.address);
	plogk("HPET: Base address mapped to virtual address 0x%016x\n", (unsigned long long)hpet_addr);

	uint32_t counterClockPeriod = hpet_addr->generalCapabilities >> 32;
	hpetPeriod = counterClockPeriod / 1000000;
	*(__volatile__ uint64_t *)(hpet->base_address.address + 0xf0) = 0;

	plogk("HPET: Main counter is initialized to 0.\n");
	plogk("HPET: Counter Clock Period: %u (ns)\n", counterClockPeriod);
	plogk("HPET: Timer Period: %u (us)\n", hpetPeriod);

	hpet_addr->generalConfiguration |= 1;
	register_interrupt_handler(IRQ_32, timer_handle, 0, 0x8e);
	plogk("HPET: General Configuration Register set to 0x%08x\n", hpet_addr->generalConfiguration);
}
