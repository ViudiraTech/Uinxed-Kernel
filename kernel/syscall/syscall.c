/*
 *
 *		syscall.c
 *		系统调用
 *
 *		2024/12/15 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "syscall.h"
#include "printk.h"
#include "common.h"
#include "sched.h"
#include "types.h"

/* 发送格式化输出到标准输出 */
static uint32_t syscall_printf(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	printk((const char *)ebx);
	return 0;
}

/* 系统调用表 */
syscall_t syscall_handlers[MAX_SYSCALLS] = {
	[1] = syscall_printf,
};

/* 系统调用处理 */
unsigned int syscall(void)
{
	volatile unsigned int eax, ebx, ecx, edx, esi, edi;
	asm("mov %%eax, %0\n\t" : "=r"(eax));
	asm("mov %%ebx, %0\n\t" : "=r"(ebx));
	asm("mov %%ecx, %0\n\t" : "=r"(ecx));
	asm("mov %%edx, %0\n\t" : "=r"(edx));
	asm("mov %%esi, %0\n\t" : "=r"(esi));
	asm("mov %%edi, %0\n\t" : "=r"(edi));
	if (0 <= eax && eax < MAX_SYSCALLS && syscall_handlers[eax] != 0) {
		eax = ((syscall_t)syscall_handlers[eax])(ebx, ecx, edx, esi, edi);
	} else {
		eax = -1;
	}
	return eax;
}

/* 初始化系统调用 */
void syscall_init(void)
{
	print_busy("Setting up system calls...\r");
	idt_use_reg(0x80, (uint32_t)asm_syscall_handler);
	print_succ("The system call setup is complete.\n");
}
