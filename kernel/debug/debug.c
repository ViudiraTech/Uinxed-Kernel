/*
 *
 *		debug.c
 *		内核调试程序
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "debug.h"
#include "elf.h"
#include "printk.h"
#include "common.h"
#include "vargs.h"

static elf_t kernel_elf;

/* 初始化 Debug 信息 */
void init_debug(void)
{
	/* 从 GRUB 提供的信息中获取到内核符号表和代码地址信息 */
	kernel_elf = elf_from_multiboot((multiboot_elf_section_header_table_t *)glb_mboot_ptr);
}

/* 当前的段存器值 */
void get_cur_status(uint16_t* ring, uint16_t* regs1, uint16_t* regs2, uint16_t* regs3, uint16_t* regs4)
{
	static int round = 0;
	uint16_t reg1, reg2, reg3, reg4;

	__asm__ __volatile__("mov %%cs, %0;"
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

/* 内核异常 */
void panic(const char *format, ...)
{
	/* 避免频繁创建临时变量，内核的栈很宝贵 */
	static char buff[1024];
	va_list args;
	int i;

	va_start(args, format);
	i = vsprintf(buff, format, args);
	va_end(args);

	buff[i] = '\0';

	print_time("Kernel panic - not syncing: ");
	printk("%s", buff);
	krn_halt();
}

/* 内核堆栈跟踪 */
void get_stack_trace(uint32_t *eips, const char **syname)
{
	uint32_t *ebp, *eip;
	int ps = 0, sy = 0;

	__asm__ __volatile__("mov %%ebp, %0" : "=r" (ebp));
	while (ebp) {
		eip = ebp + 1;
		eips[ps++] = *eip;
		syname[sy++] = elf_lookup_symbol(*eip, &kernel_elf);
		ebp = (uint32_t*)*ebp;
	}
}

/* 强制阻塞 */
void spin(const char *name)
{
	printk("spinning in %s ...", name);
	krn_halt();
}

/* 断言失败 */
void assertion_failure(const char *exp, const char *file, int line)
{
	printk("assert(%s) failed!\n"
           "file: %s\n"
           "line: %d\n\n",
           exp, file, line);

	spin("assertion_failure()");

	/* 不可能走到这里，否则出错 */
	__asm__ __volatile__("ud2");
}
