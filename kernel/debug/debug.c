/*
 *
 *		debug.c
 *		内核调试程序
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "debug.h"
#include "elf.h"
#include "printk.h"

static elf_t kernel_elf;

/* 初始化 Debug 信息 */
void init_debug(void)
{
	/* 从 GRUB 提供的信息中获取到内核符号表和代码地址信息 */
	kernel_elf = elf_from_multiboot(glb_mboot_ptr);
}

/* 打印当前的段存器值 */
void print_cur_status(void)
{
	static int round = 0;
	uint16_t reg1, reg2, reg3, reg4;

	asm volatile("mov %%cs, %0;"
                 "mov %%ds, %1;"
                 "mov %%es, %2;"
                 "mov %%ss, %3;"
                 : "=m"(reg1), "=m"(reg2), "=m"(reg3), "=m"(reg4));

	/* 打印当前的运行级别 */
	printk("%d: @ring %d\n", round, reg1 & 0x3);
	printk("%d:  cs = %x\n", round, reg1);
	printk("%d:  ds = %x\n", round, reg2);
	printk("%d:  es = %x\n", round, reg3);
	printk("%d:  ss = %x\n", round, reg4);
	++round;
}

/* 内核恐慌 */
void panic(const char *msg)
{
	printk("*** Kernel panic: %s\n", msg);
	print_stack_trace();
	printk("***\n");
	
	/* 致命错误发生后打印栈信息后停止在这里 */
	while(1) {asm("hlt");}
}

/* 打印内核堆栈跟踪 */
void print_stack_trace(void)
{
	uint32_t *ebp, *eip;

	asm volatile("mov %%ebp, %0" : "=r" (ebp));
	while (ebp) {
		eip = ebp + 1;
		printk("   [0x%x] %s\n", *eip, elf_lookup_symbol(*eip, &kernel_elf));
		ebp = (uint32_t*)*ebp;
	}
}

/* 强制阻塞 */
void spin(char *name)
{
	printk("spinning in %s ...", name);
	while(1) {asm("hlt");}
}

/* 断言失败 */
void assertion_failure(char *exp, char *file, char *base, int line)
{
	printk("assert(%s) failed!\n"
           "file: %s\n"
           "base: %s\n"
           "line: %d\n\n",
           exp, file, base, line);

	spin("assertion_failure()");

	/* 不可能走到这里，否则出错 */
	asm volatile("ud2");
}
