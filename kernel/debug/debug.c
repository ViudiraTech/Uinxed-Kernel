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
#include "common.h"
#include "console.h"

static elf_t kernel_elf;

/* 初始化 Debug 信息 */
void init_debug(void)
{
	/* 从 GRUB 提供的信息中获取到内核符号表和代码地址信息 */
	kernel_elf = elf_from_multiboot(glb_mboot_ptr);
}

/* 打印当前的段存器值 */
void print_cur_status(uint16_t* ring, uint16_t* regs1, uint16_t* regs2, uint16_t* regs3, uint16_t* regs4)
{
	static int round = 0;
	uint16_t reg1, reg2, reg3, reg4;

	asm volatile("mov %%cs, %0;"
                 "mov %%ds, %1;"
                 "mov %%es, %2;"
                 "mov %%ss, %3;"
                 : "=m"(reg1), "=m"(reg2), "=m"(reg3), "=m"(reg4));

	*ring = reg1 & 0x3;
	*regs1 = reg1;
	*regs2 = reg2;
	*regs3 = reg3;
	*regs4 = reg4;
	++round;
}

/* 内核恐慌 */
void panic(const char *msg)
{
	uint16_t ring = 0, regs1 = 0, regs2 = 0, regs3 = 0, regs4 = 0;
	uint32_t eips[5];
	const char *syname[5];
	int ps = 0, sy = 0;
	print_cur_status(&ring, &regs1, &regs2, &regs3, &regs4);
	print_stack_trace(eips, syname);

	console_clear_color(rc_blue, rc_white);
	printk_color(rc_blue, rc_white, "Your kernel has encountered a fatal error.\n");
	printk_color(rc_blue, rc_white, "We've shut down the kernel to keep you and the kernel safe.\n");
	printk_color(rc_blue, rc_white, "If you want to resume use, restart your computer.\n\n");
	printk_color(rc_blue, rc_white, "If you encounter this interface again, perform the following steps:\n");
	printk_color(rc_blue, rc_white, "    1.Remove the most recent hardware device or faulty device.\n");
	printk_color(rc_blue, rc_white, "    2.Check the compatibility of the kernel with the hardware.\n");
	printk_color(rc_blue, rc_white, "    3.Seek help from a professional.\n\n");
	printk_color(rc_blue, rc_white, "Technical information:\n");
	printk_color(rc_blue, rc_white, "\n*** STOP - Kernel-Panic: %s\nEIP:    ", msg);
	for (int i = 0; i < 5; i++) {
		printk_color(rc_blue, rc_white, "[0x%X: %s]", eips[ps++], syname[sy++]);
	}
	printk_color(rc_blue, rc_white, "\nSTATUS: [RING: %d][CS: %d][DS: %d][ES: %d][SS: %d]\n", ring, regs1, regs2, regs3, regs4);
	krn_halt();
}

/* 打印内核堆栈跟踪 */
void print_stack_trace(uint32_t *eips, const char **syname)
{
	uint32_t *ebp, *eip;
	int ps = 0, sy = 0;

	asm volatile("mov %%ebp, %0" : "=r" (ebp));
	while (ebp) {
		eip = ebp + 1;
		eips[ps++] = *eip;
		syname[sy++] = elf_lookup_symbol(*eip, &kernel_elf);
		ebp = (uint32_t*)*ebp;
	}
}

/* 强制阻塞 */
void spin(char *name)
{
	printk("spinning in %s ...", name);
	krn_halt();
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
