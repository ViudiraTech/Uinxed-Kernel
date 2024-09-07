/*
 *
 *		task.c
 *		多任务处理程序
 *
 *		2024/9/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "gdt.h"
#include "memory.h"
#include "task.h"
#include "sched.h"
#include "string.h"
#include "debug.h"
#include "printk.h"

/* 全局 pid 值 */
pid_t now_pid = 0;

/* 内核进程创建 */
int32_t kernel_thread(int (*fn)(void *), void *arg, char *name)
{
	struct task_struct *new_task = (struct task_struct *)kmalloc(STACK_SIZE);
	
	assertx(new_task != NULL, "KERNEL_THREAD_KMALLOC_ERROR");

	/* 将栈低端结构信息初始化为 0 */ 
	bzero(new_task, sizeof(struct task_struct));

	new_task->state = TASK_RUNNABLE;
	new_task->stack = current;
	new_task->pid = now_pid++;
	new_task->mm = NULL;
	new_task->name = name;

	uint32_t *stack_top = (uint32_t *)((uint32_t)new_task + STACK_SIZE);

	*(--stack_top) = (uint32_t)arg;
	*(--stack_top) = (uint32_t)kthread_exit;
	*(--stack_top) = (uint32_t)fn;

	new_task->context.esp = (uint32_t)new_task + STACK_SIZE - sizeof(uint32_t) * 3;

	/* 设置新任务的标志寄存器未屏蔽中断,很重要 */
	new_task->context.eflags = 0x200;
	new_task->next = running_proc_head;
	
	/* 找到当前进任务队列，插入到末尾 */
	struct task_struct *tail = running_proc_head;
	assertx(tail != NULL, "MUST_INIT_SCHED");

	while (tail->next != running_proc_head) {
		tail = tail->next;
	}
	tail->next = new_task;

	return new_task->pid;
}

/* 进程退出函数 */
void kthread_exit(void)
{
	register uint32_t val asm ("eax");
	printk("Task [PID: %d] exited with value %d\n", current->pid, val);
	while (1);
}

/* 打印当前的所有进程 */
int print_task(struct task_struct *base, struct task_struct *cur)
{
	int i = 1;
	if (cur->pid == base->pid) {
		switch (cur->state) {
			case TASK_RUNNABLE:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Running");
				i++;
				break;
			case TASK_SLEEPING:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Sleeping");
				i++;
				break;
			case TASK_UNINIT:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Init");
				i++;
				break;
			case TASK_ZOMBIE:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Zombie");
				i++;
				break;
		}
	} else {
		switch (cur->state) {
			case TASK_RUNNABLE:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Running");
				i++;
				break;
			case TASK_SLEEPING:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Sleeping");
				i++;
				break;
			case TASK_UNINIT:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Init");
				i++;
				break;
			case TASK_ZOMBIE:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Zombie");
				i++;
				break;
		}
		print_task(base, cur->next);
	}
	return i;
}
