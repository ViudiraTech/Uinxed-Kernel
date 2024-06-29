// main.c -- 内核主入口（基于 GPL-3.0 开源协议）
// Copyright © 2020 ViudiraTech，保留所有权利。
// 源于 MicroFish 撰写于 2024-6-23.

#include "idt.h"
#include "gdt.h"
#include "console.h"
#include "debug.h"
#include "printk.h"

void kernel_init()
{
	init_gdt();
	init_idt();

	console_clear();

	console_write("Hello, Uinxed Kernel!\n");
	printk("printk done\n");

	console_write("Testing IDT...\n");
	asm volatile("int $0x3");

	for(;;){
		asm volatile("hlt");
	}
}
