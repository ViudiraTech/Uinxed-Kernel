/*
 *
 *		apic.c
 *		高级可编程中断控制器
 *
 *		2025/2/17 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "apic.h"
#include "printk.h"
#include "hhdm.h"
#include "common.h"
#include "limine.h"
#include "idt.h"

int x2apic_mode;
uint64_t lapic_address;
uint64_t ioapic_address;

__attribute__((used, section(".limine_requests")))
static __volatile__ struct limine_smp_request smp_request = {
	.id = LIMINE_SMP_REQUEST,
	.revision = 0,
	.response = 0,
	.flags = 1,
};

/* 关闭PIC */
void disable_pic(void)
{
	outb(0x21, 0xff);
	outb(0xa1, 0xff);
}

/* 写I/O APIC寄存器 */
void ioapic_write(uint32_t reg, uint32_t value)
{
	mmio_write32((uint32_t *)((uint64_t)ioapic_address), reg);
	mmio_write32((uint32_t *)((uint64_t)ioapic_address + 0x10), value);
}

/* 读I/O APIC寄存器 */
uint32_t ioapic_read(uint32_t reg)
{
	mmio_write32((uint32_t *)((uint64_t)ioapic_address), reg);
	return mmio_read32((uint32_t *)((uint64_t)ioapic_address + 0x10));
}

/* 配置I/O APIC中断路由 */
void ioapic_add(uint8_t vector, uint32_t irq)
{
	uint32_t ioredtbl = (uint32_t)(0x10 + (uint32_t)(irq * 2));
	uint64_t redirect = (uint64_t)vector;
	redirect |= lapic_id() << 56;
	ioapic_write(ioredtbl, (uint32_t)redirect);
	ioapic_write(ioredtbl + 1, (uint32_t)(redirect >> 32));
}

/* 写本地APIC寄存器 */
void lapic_write(uint32_t reg, uint32_t value)
{
	if (x2apic_mode) {
		wrmsr(0x800 + (reg >> 4), value);
		return;
	}
	mmio_write32((uint32_t *)((uint64_t)lapic_address + reg), value);
}

/* 读本地APIC寄存器 */
uint32_t lapic_read(uint32_t reg)
{
	if (x2apic_mode) {
		return rdmsr(0x800 + (reg >> 4));
	}
	return mmio_read32((uint32_t *)((uint64_t)lapic_address + reg));
}

/* 获取当前处理器的本地APIC ID */
uint64_t lapic_id(void)
{
	return lapic_read(LAPIC_REG_ID);
}

/* 初始化本地APIC */
void local_apic_init(void)
{
	x2apic_mode = (smp_request.response->flags & 1) != 0;

	if (x2apic_mode)
		plogk("Local APIC: x2APIC\n");
	else
		plogk("Local APIC: xAPIC\n");

	lapic_write(LAPIC_REG_TIMER, IRQ_32);
	lapic_write(LAPIC_REG_SPURIOUS, 0xff | 1 << 8);
	lapic_write(LAPIC_REG_TIMER_DIV, 11);

	uint64_t b = nanoTime();
	lapic_write(LAPIC_REG_TIMER_INITCNT, ~((uint32_t)0));
	for (;;) if (nanoTime() - b >= 1000000) break;
	uint64_t lapic_timer = (~(uint32_t)0) - lapic_read(LAPIC_REG_TIMER_CURCNT);
	uint64_t calibrated_timer_initial = (uint64_t)((uint64_t)(lapic_timer * 1000) / 250);
	lapic_write(LAPIC_REG_TIMER, lapic_read(LAPIC_REG_TIMER) | 1 << 17);
	lapic_write(LAPIC_REG_TIMER_INITCNT, calibrated_timer_initial);
}

/* 初始化I/O APIC */
void io_apic_init(void)
{
	ioapic_address = (uint64_t)phys_to_virt(ioapic_address);
	ioapic_add(IRQ_32, 0);	// 定时器
	ioapic_add(IRQ_33, 1);	// 键盘
	ioapic_add(IRQ_34, 12);	// 鼠标
}

/* 发送EOI信号 */
void send_eoi(void)
{
	lapic_write(0xb0, 0);
}

/* 停止本地APIC定时器 */
void lapic_timer_stop(void)
{
	lapic_write(LAPIC_REG_TIMER_INITCNT, 0);
	lapic_write(LAPIC_REG_TIMER, (1 << 16));
}

/* 发送中断处理指令 */
void send_ipi(uint32_t apic_id, uint32_t command)
{
	if (x2apic_mode) {
		lapic_write(APIC_ICR_LOW, (((uint64_t)apic_id) << 32) | command);
	} else {
		lapic_write(APIC_ICR_HIGH, apic_id << 24);
		lapic_write(APIC_ICR_LOW, command);
	}
}

/* 初始化APIC */
void apic_init(MADT *madt)
{
	lapic_address = (uint64_t)phys_to_virt(madt->local_apic_address);
	plogk("Local APIC: Base address 0x%016x\n", lapic_address);

	uint64_t current = 0;
	for (;;) {
		if (current + ((uint32_t)sizeof(MADT) - 1) >= madt->h.Length) {
			break;
		}
		MadtHeader *header = (MadtHeader *)((uint64_t)(&madt->entries) + current);
		if (header->entry_type == MADT_APIC_IO) {
			MadtIOApic *ioapic = (MadtIOApic *)((uint64_t)(&madt->entries) + current);
			ioapic_address = ioapic->address;
			plogk("I/O APIC: Found at address 0x%016x\n", ioapic_address);
		}
		current += (uint64_t)header->length;
	}
	disable_pic();
	local_apic_init();
	io_apic_init();
}
