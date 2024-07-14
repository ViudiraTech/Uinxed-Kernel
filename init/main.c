/*
 *
 *		main.c
 *		内核主入口
 *
 *		2024/6/23 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "idt.h"
#include "gdt.h"
#include "console.h"
#include "debug.h"
#include "printk.h"
#include "memory.h"
#include "keyboard.h"

extern uint32_t end;
uint32_t placement_address = (uint32_t) & end;

void kernel_init(void)
{
	console_clear(); // 清屏

	printk("Uinxed-Kernel V1.0\n");				// 打印内核信息
	printk("Copyright 2020 ViudiraTech.\n");	// 打印版权信息
	printk("This version compiles at "__DATE__" "__TIME__"\n\n");	// 打印编译日期时间

	printk("Initializing operating system kernel components.\n");	// 提示用户正在初始化内核

	init_gdt();			// 初始化gdt
	init_idt();			// 初始化idt
	init_page();		// 初始化内存分页
	init_pci();			// 初始化PCI设备
	init_serial();		// 初始化计算机串口
	init_keyboard();	// 初始化键盘驱动
	block_init();		// 初始化块设备
	init_vfs();			// 初始化虚拟文件系统

	init_timer(1);		// 初始化定时器
	init_pit();			// 初始化PIT

	enable_intr();		// 开启中断

	system_beep(1000);	// 初始化完毕后蜂鸣
	sleep(10);
	system_beep(0);

	console_write_newline();	// 打印一个空行，和上面的信息保持隔离
	print_cpu_id();				// 打印当前CPU的信息

	shell(); // 进入简易的shell程序方便调试
}
