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
static struct task_struct *new_task(void *arg, const char *name, int level)
{
	struct task_struct *task_pcb = (struct task_struct *)kmalloc(sizeof(struct task_struct));
	assertx(task_pcb != NULL, P008);
	bzero(task_pcb, sizeof(struct task_struct));

	task_pcb->level = level;
	task_pcb->state = TASK_RUNNABLE;
	task_pcb->pid = now_pid++;
	task_pcb->mem_size = 0;
	task_pcb->name = name;
	task_pcb->pgd_dir = create_directory();
	task_pcb->exit_status = -1;

	for (int i = 0; i < 255; i++) task_pcb->file_table[i] = 0;

	task_pcb->fpu_flag = 0;
	task_pcb->cpu_clock = 0;
	task_pcb->sche_time = 1;

	task_pcb->arg = arg;
	task_pcb->jmp_buf[0] = 0; // EBP
	task_pcb->jmp_buf[2] = KERNEL_STACK_TOP; // ESP

	task_pcb->next = running_proc_head;

	/* 找到当前进任务队列，插入到末尾 */
	struct task_struct *tail = running_proc_head;
	assertx(tail != 0, P009);

	while (tail->next != running_proc_head) {
		tail = tail->next;
	}
	tail->next = task_pcb;

	return task_pcb;
}

/* 进程退出函数 */
void kthread_exit(void)
{
	current->state = TASK_ZOMBIE;
	printk("Task [PID: %d] exited with value %d\n", current->pid, current->exit_status);
	schedule();
}

static void kernel_thread_start(void)
{
	current->exit_status = current->fn(current->arg);
	kthread_exit();
}

/* 内核进程创建 */
int32_t kernel_thread(int (*fn)(void *), void *arg, const char *name, int level)
{
	struct task_struct *task_pcb = new_task(arg, name, level);
	task_pcb->fn = fn;
	task_pcb->jmp_buf[1] = (uintptr_t)kernel_thread_start; // EIP
	task_pcb->kill_eip = (uintptr_t)kthread_exit;
	return task_pcb->pid;
}

static void elf_thread_exit(void)
{
	vfs_close(current->exe_file);
	kthread_exit();
}

static void elf_thread_start(void)
{
	vfs_node_t elfile = current->exe_file;
	uint8_t *data = kmalloc(elfile->size);
	if (vfs_read(elfile, data, 0, elfile->size) != -1) {
		int (*fn)(void *) = (int (*)(void *))elf_load(elfile->size, data);
		kfree(data);
		current->exit_status = fn(current->arg);
	} else {
		kfree(data);
	}
	elf_thread_exit();
}

/* ELF进程创建 */
int32_t elf_thread(const char* path, void *arg, const char *name, int level)
{
	vfs_node_t elfile = vfs_open(path);
	if (elfile == 0) return -1;

	struct task_struct *task_pcb = new_task(arg, name, level);
	task_pcb->exe_file = elfile;
	task_pcb->jmp_buf[1] = (uintptr_t)elf_thread_start; // EIP
	task_pcb->kill_eip = (uintptr_t)elf_thread_exit;

	return task_pcb->pid;
}

/* 获得当前进程 */
struct task_struct *get_current_proc(void)
{
	return current;
}

/* 打印当前的所有进程 */
int print_task(void)
{
	struct task_struct *pcb = running_proc_head;
	int p = 0;
	printk("PID NAME                     RAM(byte)  Level             Time\n");
	while (1) {
		p++;
		printk("%-3d %-24s %-17s %d\n", pcb->pid, pcb->name,
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
	struct task_struct *argv = found_task_pid(pid);
	if (argv == 0) {
		printk("Cannot found task PID: %d\n", pid);
		return;
	} else if (argv->level == KERNEL_TASK) {
		printk("Taskkill cannot terminate kernel processes.\n");
		return;
	}

	argv->jmp_buf[1] = argv->kill_eip;
	for (int i = 0; i < 255; i++){
		cfile_t file = argv->file_table[i];
		if(file != 0){
			vfs_close(file->handle);
		}
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
