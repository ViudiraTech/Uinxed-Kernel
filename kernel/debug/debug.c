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
#include "vbe.h"
#include "lib_os_terminal.lib.h"

static elf_t kernel_elf;

/* 初始化 Debug 信息 */
void init_debug(void)
{
	/* 从 GRUB 提供的信息中获取到内核符号表和代码地址信息 */
	kernel_elf = elf_from_multiboot((multiboot_elf_section_header_table_t *)glb_mboot_ptr);
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

/* 内核异常 */
void panic(const char *msg)
{
	terminal_destroy();

	uint16_t ring = 0, regs1 = 0, regs2 = 0, regs3 = 0, regs4 = 0;
	uint32_t eips[5];
	const char *syname[5];
	int ps = 0, sy = 0;
	volatile size_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
	volatile size_t fs, gs;

	print_cur_status(&ring, &regs1, &regs2, &regs3, &regs4);
	print_stack_trace(eips, syname);

	asm("mov %%eax, %0\n\t" : "=r"(eax));
	asm("mov %%ebx, %0\n\t" : "=r"(ebx));
	asm("mov %%ecx, %0\n\t" : "=r"(ecx));
	asm("mov %%edx, %0\n\t" : "=r"(edx));
	asm("mov %%esi, %0\n\t" : "=r"(esi));
	asm("mov %%edi, %0\n\t" : "=r"(edi));
	asm("mov %%ebp, %0\n\t" : "=r"(ebp));
	asm("mov %%esp, %0\n\t" : "=r"(esp));

	asm("mov %%fs, %0\n\t" : "=r"(fs));
	asm("mov %%gs, %0\n\t" : "=r"(gs));

	vbe_clear_color(0x000080);
	vbe_printk("Your kernel has encountered a fatal error.\n");
	vbe_printk("We've shut down the kernel to keep you and the kernel safe.\n");
	vbe_printk("If you want to resume use, restart your computer.\n\n");
	vbe_printk("If you encounter this interface again, perform the following steps:\n");
	vbe_printk("    1.Remove the most recent hardware device or faulty device.\n");
	vbe_printk("    2.Check the compatibility of the kernel with the hardware.\n");
	vbe_printk("    3.Seek help from a professional.\n\n");
	vbe_printk("Technical information:\n");
	vbe_printk("\n*** STOP - Kernel-Panic: %s\n\nEIP:    ", msg);
	for (int i = 0; i < 5; i++) {
		vbe_printk("[0x%08X: %s]", eips[ps++], syname[sy++]);
	}
	vbe_printk("\nEAX:    [0x%08X] EBX: [0x%08X] ECX: [0x%08X]\n", eax, ebx, ecx);
	vbe_printk("EDX:    [0x%08X] ESI: [0x%08X] EDI: [0x%08X]\n", edx, esi, edi);
	vbe_printk("EBP:    [0x%08X] ESP: [0x%08X] EFL: [0x%08X]\n", ebp, esp, load_eflags());
	vbe_printk("FS:     [0x%04X]     GS:  [0x%04X]\n", fs, gs);
	vbe_printk("\nSTATUS: [RING: 0x%04X][CS: 0x%04X][DS: 0x%04X][ES: 0x%04X][SS: 0x%04X]\n", ring, regs1, regs2, regs3, regs4);
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
