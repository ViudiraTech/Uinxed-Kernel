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
#include "os_terminal.lib.h"

/* 可调度进程链表 */
struct ilist_node running_proc_list;

/* 等待进程链表 */
struct ilist_node wait_proc_list;

/* 当前运行的任务 */
struct task_struct *current = 0;

struct task_struct *idle = 0;

/* 调度标志位 */
int can_sche = 0;

static int idle_loop(void *arg)
{
	enable_intr();
	while (1)
		krn_halt();
	return 1;
}

/* 初始化任务调度 */
void init_sched(void)
{
	print_busy("Initializing multi-task...\r");

	ilist_init(&running_proc_list);
	ilist_init(&wait_proc_list);

	idle = kernel_thread(idle_loop, NULL, "idle", KERNEL_TASK);
	ilist_remove(&idle->wait_list);

	/* 为当前执行流创建信息结构体 该结构位于当前执行流的栈最低端 */
	current = kmalloc(sizeof(struct task_struct));

	current->level = KERNEL_TASK;
	current->state = TASK_RUNNABLE;
	current->pid = now_pid++;
	current->pgd_dir = kernel_directory;
	current->mem_size = 0;
	current->name = "Uinxed-Kernel";	// 内核进程名称
	current->fpu_flag = 0;				// 还没使用FPU
	current->cpu_clock = 0;
	current->sche_time = 1;

	current->context.kill_eip = (uint32_t)kthread_exit;

	for (int i = 0; i < 255; i++)current->file_table[i] = 0;

	ilist_insert_before(&running_proc_list, &(current->running_list));
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
struct task_struct *schedule(void)
{
	ILIST_FOREACH(node, wait_proc_list) {
		struct task_struct *pcb = container_of(node, struct task_struct, wait_list);
		ilist_remove(&pcb->wait_list);
		if (pcb->state == TASK_RUNNABLE)
			return pcb;
		free_directory(pcb->pgd_dir);
	}
	return idle;
}

__attribute__((noipa,naked)) // 切换页表后，完成longjmp之前，esp还没改过来，所以必须naked
static void resume()
{
	__asm__("mov %0, %%cr3" : : "r"(&(current->pgd_dir->table_phy)));
	if (current->exit_status < 0)
		current->context.jmp_buf[1] = current->context.kill_eip;
	__builtin_longjmp(current->context.jmp_buf, 1);
}

/* 任务切换准备 */
void change_task_to(struct task_struct *next)
{
	if (current != next) {
		uint32_t cr0 = get_cr0();
		if (!(cr0 & (1 << 3))) {
			/* CR0.TS 为零，说明进程用过FPU了，需要保存 */
			__asm__ ("fnsave %0" : : "m"(current->context.fpu_regs) : "memory");

			/* 设置CR0.TS FPU操作将产生#NM */
			set_cr0(cr0 | (1 << 3));
		}
		if (!__builtin_setjmp(current->context.jmp_buf)) {
			current = next;
			resume();
			panic(PFFF);
		}
	}
}

void yield(void)
{
	struct task_struct *next = schedule();
	current->cpu_clock++;
	change_task_to(next);
}

void queue_task(struct task_struct *pcb)
{
	if (pcb == idle)
		return;
	ilist_insert_before(&wait_proc_list, &pcb->wait_list);
}

void queue_task_list(struct ilist_node *list) {
	if (list->next == list)
		return;
	/* wait -> wait->next ... wait->prev <- wait */
	/* list -> list->next ... list->prev <- list */
	/* wait -> wait->next ... wait->prev list->next ... list->prev <- list */
	struct ilist_node *last = wait_proc_list.prev;
	list->prev->next = &wait_proc_list;
	list->next->prev = last;
	wait_proc_list.prev = list->prev;
	last->next = list->next;
	ilist_init(list);
}
