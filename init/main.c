// main.c -- 内核主入口（基于 GPL-3.0 开源协议）
// Copyright © 2020 ViudiraTech，保留所有权利。
// 源于 MicroFish 撰写于 2024-6-23.

#include "idt.h"
#include "gdt.h"
#include "console.h"
#include "debug.h"
#include "printk.h"
#include "memory.h"

extern uint32_t end;
uint32_t placement_address = (uint32_t) & end;

void kernel_init()
{
	console_clear(); // 清屏

	printk("Uinxed-Kernel V1.0\n");				// 打印内核信息
	printk("Copyright 2020 ViudiraTech.\n\n");	// 打印版权信息

	print_info("Initializing operating system kernel components.\n"); // 提示用户正在初始化内核

	init_gdt();		// 初始化gdt
	init_idt();		// 初始化idt
	init_page();	// 初始化内存分页

	console_write_newline();	// 打印一个空行，和上面的信息保持隔离

	print_cpu_id();				// 打印当前CPU的信息
	panic("No operation!");		// 因为内核无事可做，直接panic
}
