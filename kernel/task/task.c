/*
 *
 *		task.c
 *		多任务处理程序
 *
 *		2024/9/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
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
#include "elf.h"

/* 全局 pid 值 */
int now_pid = 0;

/* 进程创建 */
static int32_t new_task(int (*fn)(void **), void **arg, const char *name, int level, vfs_node_t exefile)
{
	struct task_struct *task_pcb = (struct task_struct *)kmalloc(STACK_SIZE);
	assertx(task_pcb != 0, P008);

	/* 将栈低端结构信息初始化为 0 */
	bzero(task_pcb, sizeof(struct task_struct));

	task_pcb->level = level;
	task_pcb->state = TASK_RUNNABLE;
	task_pcb->pid = now_pid++;
	task_pcb->mem_size = 0;
	task_pcb->name = name;
	task_pcb->stack = current;
	task_pcb->program_break = (uint32_t)program_break;
	task_pcb->program_break_end = (uint32_t)program_break_end;
	task_pcb->pgd_dir = clone_directory(kernel_directory);

	for (int i = 0; i < 255; i++)task_pcb->file_table[i] = 0;

	task_pcb->exe_file = exefile;
	task_pcb->fpu_flag = 0;
	task_pcb->cpu_clock = 0;
	task_pcb->sche_time = 1;

	uint32_t *stack_top = (uint32_t *)((uint32_t)task_pcb + STACK_SIZE);

	*(--stack_top) = (uint32_t)arg;
	*(--stack_top) = (uint32_t)kthread_exit;
	*(--stack_top) = (uint32_t)fn;

	task_pcb->context.esp = (uint32_t)task_pcb + STACK_SIZE - sizeof(uint32_t) * 3;

	/* 设置新任务的标志寄存器未屏蔽中断,很重要 */
	task_pcb->context.eflags = 0x200;
	task_pcb->next = running_proc_head;

	/* 找到当前进任务队列，插入到末尾 */
	struct task_struct *tail = running_proc_head;
	assertx(tail != 0, P009);

	while (tail->next != running_proc_head) {
		tail = tail->next;
	}
	tail->next = task_pcb;

	return task_pcb->pid;
}

/* 内核进程创建 */
int32_t kernel_thread(int (*fn)(void **), void **arg, const char *name, int level)
{
	return new_task(fn, arg, name, level, 0);
}

/* ELF进程创建 */
int32_t execv_thread(const char* path, void **arg, const char *name, int level)
{
	vfs_node_t elfile = vfs_open(path);
	if (elfile == 0) return -1;

	uint8_t *data = kmalloc(elfile->size);
	if (vfs_read(elfile, data, 0, elfile->size) == -1) {
		vfs_close(elfile);
		return -1;
	}
	uint32_t _start = elf_load(elfile->size, data);
	kfree(data);

	return new_task((void *)_start, arg, name, level, elfile);
}

/* 获得当前进程 */
struct task_struct *get_current_proc(void)
{
	return current;
}

/* 进程退出函数 */
void kthread_exit(void)
{
	register uint32_t val __asm__("eax");
	printk("Task [PID: %d] exited with value %d\n", current->pid, val);
	task_kill(current->pid);
	while (1);
}

/* 打印当前的所有进程 */
int print_task(void)
{
	struct task_struct *pcb = running_proc_head;
	int p = 0;
	printk("PID NAME                     RAM(byte)  Level             Time\n");
	while (1) {
		p++;
		printk("%-3d %-24s %-10d %-17s %d\n", pcb->pid, pcb->name,
                                              pcb->program_break_end - pcb->program_break,
                                              pcb->level == KERNEL_TASK ? "Kernel Process" :
                                              pcb->level == SERVICE_TASK ? "Service Process" : "User Process",
                                              pcb->cpu_clock);
		pcb = pcb->next;
		if (pcb->next != running_proc_head) break;
	}
	return p;
}

/* 查找特定pid的结构体 */
static void found_task(int pid, struct task_struct *head, struct task_struct *base,
                       struct task_struct **argv, int first)
{
	struct task_struct *t = base;
	if (t == 0) {
		argv = 0;
		return;
	}
	if (t->pid == pid) {
		*argv = t;
		return;
	} else {
		if (!first)
			if (head->pid == t->pid) {
				argv = 0;
				return;
			}
		found_task(pid, head, t->next, argv, 0);
	}
}

/* 传回特定pid的结构体 */
struct task_struct *found_task_pid(int pid)
{
	struct task_struct *argv = 0;
	found_task(pid, running_proc_head, running_proc_head, &argv, 1);
	if (argv == 0) {
		return 0;
	}
	return argv;
}

/* 杀死指定进程 */
void task_kill(int pid)
{
	disable_intr();
	disable_scheduler();
	struct task_struct *argv = found_task_pid(pid);
	if (argv == 0) {
		printk("Cannot found task PID: %d\n", pid);
		enable_intr();
		enable_scheduler();
		return;
	} else if (argv->level == KERNEL_TASK) {
		printk("Taskkill cannot terminate kernel processes.\n");
		enable_intr();
		enable_scheduler();
		return;
	}
	for (int i = 0; i < 255; i++){
		cfile_t file = argv->file_table[i];
		if(file != 0){
			vfs_close(file->handle);
		}
	}
	vfs_close(argv->exe_file);
	put_directory(argv->pgd_dir);
	argv->state = TASK_DEATH;
	printk("Task [Name: %s][PID: %d] Stopped.\n", argv->name, argv->pid);
	struct task_struct *head = running_proc_head;
	struct task_struct *last = 0;
	while (1) {
		if (head->pid == argv->pid) {
			last->next = argv->next;
			kfree(argv);
			enable_intr();
			enable_scheduler();
			return;
		}
		last = head;
		head = head->next;
	}
}

/* 杀死全部进程 */
void kill_all_task(void)
{
	struct task_struct *head = running_proc_head;
	while (1) {
		head = head->next;
		if (head == 0 || head->pid == running_proc_head->pid) {
			return;
		}
		if (head->pid == current->pid) continue;
		task_kill(head->pid);
	}
}

/* 使进程陷入等待 */
void wait_task(struct task_struct *task)
{
	task->state = TASK_SLEEPING;
}

/* 使进程回归运行 */
void start_task(struct task_struct *task)
{
	task->state = TASK_RUNNABLE;
}
