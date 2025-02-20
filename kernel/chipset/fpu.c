/*
 *
 *		fpu.c
 *		fpu浮点协处理器
 *
 *		2024/9/17 By min0911Y
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#include "common.h"
#include "fpu.h"
#include "sched.h"
#include "printk.h"

/*
 * x86 FPU设计上是设置CR0.TS后，任何FPU操作都会产生#NM
 * 这样操作系统可以在切换进程时，不把FPU状态保存下来
 * 等到产生了#NM再存。所以TS是Task Switched的缩写
 *
 * 这里采用简化的方案
 * 默认设置CR0.TS，fpu_flag为1表示标记进程的FPU已经初始化了
 * #NM产生时，根据fpu_flag决定是初始化FPU还是用存下来的数据恢复FPU寄存器，并清零CR0.TS
 * 这样切换进程时，根据CR0.TS是否为0，就知道是否需要保存FPU寄存器了
 * 保存好之后立即设置CR0.TS
 */

/* FPU中断 */
static void fpu_handler(pt_regs *regs)
{
	set_cr0(get_cr0() & ~(1 << 3));
	if (!current->fpu_flag) {
		__asm__ ("fnclex; fninit" : : : "memory");
		current->fpu_flag = 1;
	} else {
		__asm__ ("frstor %0" : :"m"(current->context.fpu_regs) : "memory");
	}
}

/* 初始化FPU */
void init_fpu(void)
{
	print_busy("Initializing FPU floating-point coprocessor...\r");
	register_interrupt_handler(0x7, &fpu_handler);

	/*
	 * CR0.EM: 0x00000004 禁用FPU
	 * CR0.TS: 0x00000008
	 * CR0.NE: 0x00000020 FPU直接产生异常而不是外部中断
	 */
	set_cr0((get_cr0() & ~(1 << 2)) | (1 << 3) | (1 << 5));
	print_succ("The FPU coprocessor is initialized.               \n");
}
