/*
 *
 *		sched.c
 *		进程调度程序
 *
 *		2024/9/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "sched.h"
#include "gdt.h"
#include "memory.h"
#include "debug.h"
#include "printk.h"
#include "common.h"

/* 可调度进程链表 */
struct task_struct *running_proc_head = 0;

/* 等待进程链表 */
struct task_struct *wait_proc_head = 0;

/* 当前运行的任务 */
struct task_struct *current = 0;

/* 调度标志位 */
int can_sche = 0;

/* 初始化任务调度 */
void init_sched(void)
{
	print_busy("Initializing multi-task...\r");

	uintptr_t *page_directory = (uintptr_t *)CURRENT_PD_BASE;

	/* 为当前执行流创建信息结构体 该结构位于当前执行流的栈最低端 */
	current = kmalloc(sizeof(struct task_struct));
	current->level = KERNEL_TASK;
	current->state = TASK_RUNNABLE;
	current->pid = now_pid++;

	/* page directory 最后一项是自己指向自己 */
	current->pgd_dir = PT_ADDRESS(page_directory[1023]);
	current->mem_size = 0;
	current->name = "Uinxed-Kernel";	// 内核进程名称
	current->fpu_flag = 0;				// 还没使用FPU
	current->cpu_clock = 0;
	current->sche_time = 1;

	for (int i = 0; i < 255; i++)current->file_table[i] = 0;

	/* 单向循环链表 */
	current->next = current;

	running_proc_head = current;
	print_succ("Multi-task initialized.       \n");
}

/* 允许进程调度 */
void enable_scheduler(void)
{
	can_sche = 1;
}

/* 停止进程调度 */
void disable_scheduler(void)
{
	can_sche = 0;
}

/* 任务调度 */
void schedule()
{
	if (current && can_sche) {
		if (current->state != TASK_RUNNABLE) {
			current = running_proc_head;
		}
		current->cpu_clock++;
		change_task_to(current->next);
	}
}

__attribute__((noipa,naked,fastcall)) // 切换页表后，完成longjmp之前，esp还没改过来，所以必须naked
static void resume()
{
	__asm__("mov %0, %%cr3" : : "a"(current->pgd_dir));
	__builtin_longjmp(current->jmp_buf, 1);
}

/* 任务切换准备 */
void change_task_to(struct task_struct *next)
{
	if (current->sche_time > 1) {
		current->sche_time--;
		return;
	}
	if (current != next) {
		if (next == NULL) next = running_proc_head;
		current->sche_time = 1;
		fpu_regs_t fpu = {0};
		/* 保存FPU状态 */
		if (current->fpu_flag) {
			__asm__ ("fnsave %0" :: "m"(fpu) : "memory");
			/* 设置CR0.EM 禁用FPU */
			set_cr0(get_cr0() | (1 << 2));
		}
		if (!__builtin_setjmp(current->jmp_buf)) {
			current = next;
			resume();
			panic(PFFF);
		}
		/* 恢复FPU状态 */
		if (current->fpu_flag) {
			set_cr0(get_cr0() & ~((1 << 2)));
			__asm__ ("frstor %0" :: "m"(fpu) : "memory");
		}
	}
}
