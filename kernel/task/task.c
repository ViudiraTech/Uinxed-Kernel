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
#include "common.h"
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
	new_task->fpu_flag = 0;
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
	task_kill(current->pid);
	while (1);
}

/* 打印当前的所有进程 */
int print_task(struct task_struct *base, struct task_struct *cur, int count)
{
	if (cur->pid == base->pid) {
		switch (cur->state) {
			case TASK_RUNNABLE:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Running");
				break;
			case TASK_SLEEPING:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Sleeping");
				break;
			case TASK_UNINIT:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Init");
				break;
			case TASK_ZOMBIE:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Zombie");
				break;
		}
		count++;
	} else {
		switch (cur->state) {
			case TASK_RUNNABLE:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Running");
				break;
			case TASK_SLEEPING:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Sleeping");
				break;
			case TASK_UNINIT:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Init");
				break;
			case TASK_ZOMBIE:
				printk("|%-30s %02d  %-8s %d\n", cur->name, cur->pid, "Zombie");
				break;
		}
		count++;
		count = print_task(base, cur->next, count);
	}
	return count;
}

/* 查找特定pid的结构体 */
static void found_task(int pid, struct task_struct *head, struct task_struct *base,
                       struct task_struct **argv, int first)
{
	struct task_struct *t = base;
	if (t == NULL) {
		argv = NULL;
		return;
	}
	if (t->pid == (pid_t)pid) {
		*argv = t;
		return;
	} else {
		if (!first)
			if (head->pid == t->pid) {
				argv = NULL;
				return;
			}
		found_task(pid, head, t->next, argv, 0);
	}
}

/* 传回特定pid的结构体 */
struct task_struct *found_task_pid(int pid)
{
	struct task_struct *argv = NULL;
	found_task(pid, running_proc_head, running_proc_head, &argv, 1);
	if (argv == NULL) {
		return NULL;
	}
	return argv;
}

/* 杀死指定进程 */
void task_kill(int pid)
{
	disable_intr();
	struct task_struct *argv = found_task_pid(pid);
	if (argv == NULL) {
		printk("Cannot found task PID: %d\n", pid);
		enable_intr();
		return;
	} else if (argv->pid == 0) {
		printk("Taskkill cannot terminate kernel processes.\n");
		enable_intr();
		return;
	}
	argv->state = TASK_DEATH;
	printk("Task [Name: %s][PID: %d] Stopped.\n", argv->name, argv->pid);
	kfree(argv);
	struct task_struct *head = running_proc_head;
	struct task_struct *last = NULL;
	while (1) {
		if (head->pid == argv->pid) {
			last->next = argv->next;
			enable_intr();
			return;
		}
		last = head;
		head = head->next;
	}
}
