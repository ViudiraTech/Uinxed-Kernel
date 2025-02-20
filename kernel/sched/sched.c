/*
 *
 *		sched.c
 *		进程调度程序
 *
 *		2024/9/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
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

	/* 为当前执行流创建信息结构体 该结构位于当前执行流的栈最低端 */
	current = kmalloc(sizeof(struct task_struct));

	current->level = KERNEL_TASK;
	current->state = TASK_RUNNABLE;
	current->pid = now_pid++;
	current->stack = current;			// 该成员指向栈低地址
	current->pgd_dir = kernel_directory;
	current->mem_size = 0;
	current->name = "Uinxed-Kernel";	// 内核进程名称
	current->fpu_flag = 0;				// 还没使用FPU
	current->cpu_clock = 0;
	current->sche_time = 1;
	current->context.esp = (uint32_t )current->stack;

	for (int i = 0; i < 255; i++)current->file_table[i] = 0;

	current->program_break = (uint32_t)program_break;
	current->program_break_end = (uint32_t)program_break_end;

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
void schedule(pt_regs *regs)
{
	disable_intr();
	if (current && can_sche) {
		if (current->state != TASK_RUNNABLE) {
			current = running_proc_head;
		}
		current->cpu_clock++;
		change_task_to(current->next, regs);
	}
	enable_intr();
}

/* 任务切换准备 */
void change_task_to(struct task_struct *next, pt_regs *regs)
{
	if (current->sche_time > 1) {
		current->sche_time--;
		return;
	}
	if (current != next) {
		if (next == NULL) next = running_proc_head;
		current->sche_time = 1;
		uint32_t cr0 = get_cr0();
		if (!(cr0 & (1 << 3))) {
			/* CR0.TS 为零，说明进程用过FPU了，需要保存 */
			__asm__ ("fnsave %0" : : "m"(current->context.fpu_regs) : "memory");

			/* 设置CR0.TS FPU操作将产生#NM */
			set_cr0(cr0 | (1 << 3));
		}
		struct task_struct *prev = current;
		current = next;
		switch_page_directory(current->pgd_dir);
		set_kernel_stack((uintptr_t)current->stack + STACK_SIZE);
		prev->context.eip = regs->eip;
		prev->context.ds = regs->ds;
		prev->context.cs = regs->cs;
		prev->context.eax = regs->eax;
		prev->context.ss = regs->ss;

		switch_to(&(prev->context), &(current->context));

		regs->ds = current->context.ds;
		regs->cs = current->context.cs;
		regs->eip = current->context.eip;
		regs->eax = current->context.eax;
		regs->ss = current->context.ss;
	}
}
