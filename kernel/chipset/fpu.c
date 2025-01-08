/*
 *
 *		fpu.c
 *		fpu浮点协处理器
 *
 *		2024/9/17 By min0911Y
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "common.h"
#include "fpu.h"
#include "sched.h"
#include "printk.h"

/* 初始化FPU */
void init_fpu(void)
{
	print_busy("Initializing FPU floating-point coprocessor...\r");
	register_interrupt_handler(7, fpu_handler);
	__asm__ __volatile__("fninit");
	set_cr0(get_cr0() | (1 << 2) | (1 << 3) | (1 << 5));
	print_succ("The FPU coprocessor is initialized.               \n");
}

/* FPU中断 */
void fpu_handler(pt_regs *regs)
{
	set_cr0(get_cr0() & ~((1 << 2) | (1 << 3)));
	if (!current->fpu_flag) {
		__asm__ __volatile__("fnclex \n"
                             "fninit \n" ::
                             : "memory");
		memset(&(current->context.fpu_regs), 0, sizeof(fpu_regs_t));
	} else {
		__asm__ __volatile__("frstor (%%eax) \n" ::"a"(&(current->context.fpu_regs)) : "memory");
	}
	current->fpu_flag = 1;
}
