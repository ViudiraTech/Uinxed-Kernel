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
	init_gdt();
	init_idt();
	init_page();

	console_clear();

	printk("Uinxed-Kernel V1.0\n");
	printk("Copyright 2020 ViudiraTech.\n\n");

	panic("No operation!");
}
