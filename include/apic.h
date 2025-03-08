/*
 *
 *		apic.h
 *		高级可编程中断控制器头文件
 *
 *		2025/2/17 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_APIC_H_
#define INCLUDE_APIC_H_

#include "stdint.h"
#include "acpi.h"

#define MADT_APIC_CPU			0x00
#define MADT_APIC_IO			0x01
#define MADT_APIC_INT			0x02
#define MADT_APIC_NMI			0x03

#define LAPIC_REG_ID			32
#define LAPIC_REG_TIMER_CURCNT	0x390
#define LAPIC_REG_TIMER_INITCNT	0x380
#define LAPIC_REG_TIMER			0x320
#define LAPIC_REG_SPURIOUS		0xf0
#define LAPIC_REG_TIMER_DIV		0x3e0

#define APIC_ICR_LOW			0x300
#define APIC_ICR_HIGH			0x310

typedef struct {
	struct ACPISDTHeader h;
	uint32_t local_apic_address;
	uint32_t flags;
	void *entries;
} __attribute__((packed))MADT;

struct madt_hander {
	uint8_t entry_type;
	uint8_t length;
} __attribute__((packed));

struct madt_io_apic {
	struct madt_hander h;
	uint8_t apic_id;
	uint8_t reserved;
	uint32_t address;
	uint32_t gsib;
} __attribute__((packed));

struct madt_local_apic {
	struct madt_hander h;
	uint8_t ACPI_Processor_UID;
	uint8_t local_apic_id;
	uint32_t flags;
};

typedef struct madt_hander MadtHeader;
typedef struct madt_io_apic MadtIOApic;
typedef struct madt_local_apic MadtLocalApic;

/* 关闭PIC */
void disable_pic(void);

/* 写I/O APIC寄存器 */
void ioapic_write(uint32_t reg, uint32_t value);

/* 读I/O APIC寄存器 */
uint32_t ioapic_read(uint32_t reg);

/* 配置I/O APIC中断路由 */
void ioapic_add(uint8_t vector, uint32_t irq);

/* 写本地APIC寄存器 */
void lapic_write(uint32_t reg, uint64_t value);

/* 读本地APIC寄存器 */
uint32_t lapic_read(uint32_t reg);

/* 获取当前处理器的本地APIC ID */
uint64_t lapic_id(void);

/* 初始化本地APIC */
void local_apic_init(void);

/* 初始化I/O APIC */
void io_apic_init(void);

/* 发送EOI信号 */
void send_eoi(void);

/* 停止本地APIC定时器 */
void lapic_timer_stop(void);

/* 发送中断处理指令 */
void send_ipi(uint32_t apic_id, uint32_t command);

/* 初始化APIC */
void apic_init(MADT *madt);

#endif // INCLUDE_APIC_H_
