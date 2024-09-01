/*
 *
 *		sched.c
 *		进程调度程序
 *
 *		2024/9/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "sched.h"
#include "memory.h"
#include "debug.h"
#include "printk.h"

/* 可调度进程链表 */
struct task_struct *running_proc_head = NULL;

/* 等待进程链表 */
struct task_struct *wait_proc_head = NULL;

/* 当前运行的任务 */
struct task_struct *current = NULL;

extern uint32_t kern_stack_top;

/* 初始化任务调度 */
void init_sched(void)
{
	print_busy("Initializing multi-task...\r");

	/* 为当前执行流创建信息结构体 该结构位于当前执行流的栈最低端 */
	current = (struct task_struct *)(kern_stack_top - STACK_SIZE);

	current->state = TASK_RUNNABLE;
	current->pid = now_pid++;
	current->stack = current;	// 该成员指向栈低地址
	current->mm = NULL;			// 内核线程不需要该成员

	/* 单向循环链表 */
	current->next = current;

	running_proc_head = current;
	print_succ("Multi-task initialized.   \n");
}

/* 任务调度 */
void schedule(void)
{
	if (current) {
		change_task_to(current->next);
	}
}

/* 任务切换准备 */
void change_task_to(struct task_struct *next)
{
	if (current != next) {
		struct task_struct *prev = current;
		current = next;
		switch_to(&(prev->context), &(current->context));
	}
}
