/*
 *
 *		gdt.c
 *		设置全局描述符程序
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "gdt.h"
#include "memory.h"

/* 全局描述符表长度 */
#define GDT_LENGTH 6

/* 全局描述符表定义 */
static gdt_entry_t gdt_entries[GDT_LENGTH] = {0};

/* TSS */
static tss_entry tss = {0};

/* GDTR */
static gdt_ptr_t gdt_ptr = {0};

static void gdt_flush(void) {
	__asm__("lgdt %0" : : "m"(gdt_ptr));
	__asm__ goto ("ljmpl $%c0, $%l1": : "i"(KERNEL_CS) : "memory" : next);
next:
	__asm__("mov %w0, %%ss" : : "r"(KERNEL_SS));
	__asm__("mov %w0, %%ds; mov %w0, %%es; mov %w0, %%fs; mov %w0, %%gs" : : "r"(USER_SS));
	__asm__("ret");
}

/* 全局描述符表构造函数，根据下标构造 */
/* 参数分别是 数组下标、基地址、限长、访问标志，其它访问标志 */
static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
	gdt_entries[num].base_low		= (base & 0xFFFF);
	gdt_entries[num].base_middle	= (base >> 16) & 0xFF;
	gdt_entries[num].base_high		= (base >> 24) & 0xFF;

	gdt_entries[num].limit_low		= (limit & 0xFFFF);
	gdt_entries[num].granularity	= (limit >> 16) & 0x0F;

	gdt_entries[num].granularity	|= gran & 0xF0;
	gdt_entries[num].access			= access;
}

/* 设置任务状态段并将其写入全局描述符表 */
static void write_tss(void)
{
	tss.ss0 = KERNEL_SS;
	tss.esp0 = KERNEL_STACK_TOP;
	tss.iomap_base = sizeof(tss);
	uintptr_t base = (uintptr_t) & tss;
	uintptr_t limit = base + sizeof(tss);
	gdt_set_gate(TSS_INDEX, base, limit, 0xE9, 0x00);
}

/* 初始化全局描述符表 */
void init_gdt(void)
{
	/* 全局描述符表界限 e.g. 从 0 开始，所以总长要 - 1 */
	gdt_ptr.limit	= sizeof(gdt_entry_t) * GDT_LENGTH - 1;
	gdt_ptr.base	= (uint32_t)&gdt_entries;

	/* 采用 Intel 平坦模型 */
	gdt_set_gate(0, 0, 0, 0, 0);					// 按照 Intel 文档要求，第一个描述符必须全 0
	gdt_set_gate(KERNEL_CS_INDEX, 0, 0xFFFFFFFF, 0x9A, 0xCF);		// 指令段
	gdt_set_gate(KERNEL_SS_INDEX, 0, 0xFFFFFFFF, 0x92, 0xCF);		// 数据段
	gdt_set_gate(USER_CS_INDEX, 0, 0xFFFFFFFF, 0xFA, 0xCF);		// 用户模式代码段
	gdt_set_gate(USER_SS_INDEX, 0, 0xFFFFFFFF, 0xF2, 0xCF);		// 用户模式数据段
	write_tss();
	/* 加载全局描述符表地址到 GDTR 寄存器 */
	gdt_flush();
	__asm__ ("ltr %w0" : : "r"(TSS));
}
