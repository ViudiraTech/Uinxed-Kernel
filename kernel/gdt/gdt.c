/*
 *
 *		gdt.c
 *		全局描述符
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "gdt.h"
#include "string.h"
#include "printk.h"

/* 全局描述符表定义 */
gdt_entries_t gdt_entries;
struct gdt_register gdt_pointer;

/* TSS */
tss_stack_t tss_stack;
tss_t tss0;

/* 初始化全局描述符表 */
void init_gdt(void)
{
	gdt_entries[0] = 0x0000000000000000;
	gdt_entries[1] = 0x00a09a0000000000;
	gdt_entries[2] = 0x00c0920000000000;
	gdt_entries[3] = 0x00c0f20000000000;
	gdt_entries[4] = 0x00a0fa0000000000;

	gdt_pointer = ((struct gdt_register) {
		.size = ((uint16_t)((uint32_t)sizeof(gdt_entries_t) - 1)),
		.ptr = &gdt_entries
	});

	__asm__ __volatile__("lgdt %[ptr]; push %[cseg]; lea 1f, %%rax; push %%rax; lretq;"
                         "1:"
                         "mov %[dseg], %%ds;"
                         "mov %[dseg], %%fs;"
                         "mov %[dseg], %%gs;"
                         "mov %[dseg], %%es;"
                         "mov %[dseg], %%ss;"
                         :: [ptr] "m"(gdt_pointer),
                         [cseg] "rm"((uint16_t)0x8),
                         [dseg] "rm"((uint16_t)0x10) : "memory");
	plogk("GDT: CS reloaded with 0x%04x, DS/ES/FS/GS/SS set to 0x%04x\n", 0x8, 0x10);
	plogk("GDT: GDT initialized at 0x%016x (6 entries)\n", &gdt_entries);
	tss_init();
}

/* 初始化TSS */
void tss_init(void)
{
	uint64_t address		= ((uint64_t)(&tss0));
	uint64_t low_base		= (((address & 0xffffff)) << 16);
	uint64_t mid_base		= (((((address >> 24)) & 0xff)) << 56);
	uint64_t high_base		= (address >> 32);
	uint64_t access_byte	= (((uint64_t)(0x89)) << 40);
	uint64_t limit			= ((uint64_t)((uint32_t)(sizeof(tss_t) - 1)));

	gdt_entries[5]			= (((low_base | mid_base) | limit) | access_byte);
	gdt_entries[6]			= high_base;
	tss0.ist[0]				= ((uint64_t)&tss_stack) + sizeof(tss_stack_t);

	plogk("TSS: TSS descriptor configured (addr: 0x%016x, limit: 0x%04x)\n", &tss0, sizeof(tss_t)-1);
	plogk("TSS: IST0 stack set to 0x%016x\n", tss0.ist[0]);
	__asm__ __volatile__("ltr %[offset];" :: [offset] "rm"(0x28) : "memory");
	plogk("TSS: TR register loaded with selector 0x%04x\n", 0x28);
}

/* 设置内核栈 */
void set_kernel_stack(uint64_t rsp)
{
	tss0.rsp[0] = rsp;
}
