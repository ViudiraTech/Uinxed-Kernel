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
#include "string.h"
#include "printk.h"

/* 全局描述符表长度 */
#define GDT_LENGTH 6

/* 全局描述符表定义 */
gdt_entry_t gdt_entries[GDT_LENGTH];

/* TSS */
tss_entry tss;

/* GDTR */
gdt_ptr_t gdt_ptr;

static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);

/* 声明内核栈地址 */
extern uint32_t stack;

/* 初始化全局描述符表 */
void init_gdt(void)
{
	print_busy("Initializing the global descriptor table...\r"); // 提示用户正在初始化全局描述符表，并回到行首等待覆盖

	/* 全局描述符表界限 e.g. 从 0 开始，所以总长要 - 1 */
	gdt_ptr.limit	= sizeof(gdt_entry_t) * GDT_LENGTH - 1;
	gdt_ptr.base	= (uint32_t)&gdt_entries;

	/* 采用 Intel 平坦模型 */
	gdt_set_gate(0, 0, 0, 0, 0);					// 按照 Intel 文档要求，第一个描述符必须全 0
	gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);		// 指令段
	gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);		// 数据段
	gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);		// 用户模式代码段
	gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);		// 用户模式数据段

	register uint32_t esp __asm__("esp");
	write_tss(5, 0x10, esp);

	/* 加载全局描述符表地址到 GPTR 寄存器 */
	gdt_flush((uint32_t)&gdt_ptr);

	print_succ("Global Descriptor Table initialized successfully.\n"); // 提示用户gdt初始化完成
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
void write_tss(int32_t num, uint16_t ss0, uint32_t esp0)
{
	uintptr_t base = (uintptr_t) & tss;
	uintptr_t limit = base + sizeof(tss);

	gdt_set_gate(num, base, limit, 0xE9, 0x00);
	memset((uint8_t * ) & tss, 0x0, sizeof(tss));

	tss.ss0 = ss0;
	tss.esp0 = esp0;
	tss.cs = 0x0b;
	tss.ss = 0x13;
	tss.ds = 0x13;
	tss.es = 0x13;
	tss.fs = 0x13;
	tss.gs = 0x13;

	tss.iomap_base = sizeof(tss);
}

/* 设置任务状态段 */
void set_kernel_stack(uintptr_t stack)
{
	tss.esp0 = stack;
}

/* 设置任务状态段中的ss0字段 */
void set_tss_ss0(uintptr_t ss)
{
	tss.ss0 = ss;
}
