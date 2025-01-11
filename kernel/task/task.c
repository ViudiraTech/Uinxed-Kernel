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

	for (int i = 0; i < 255; i++) task_pcb->file_table[i] = 0;

	task_pcb->fpu_flag = 0;
	task_pcb->cpu_clock = 0;
	task_pcb->sche_time = 1;

	task_pcb->arg = arg;
	task_pcb->context.jmp_buf[0] = 0; // EBP
	task_pcb->context.jmp_buf[2] = KERNEL_STACK_TOP; // ESP
	task_pcb->context.kill_eip = (uint32_t)kthread_exit;

	/* 插入到任务队列末尾 */
	ilist_insert_before(&running_proc_list, &(task_pcb->running_list));
	queue_task(task_pcb);

	return task_pcb;
}

static void kernel_thread_start(void)
{
	current->exit_status = current->fn(current->arg);
	kthread_exit();
}

/* 内核进程创建 */
struct task_struct *kernel_thread(int (*fn)(void *), void *arg, const char *name, int level)
{
	struct task_struct* task_pcb = new_task(arg, name, level);
	task_pcb->fn = fn;
	task_pcb->context.jmp_buf[1] = (uint32_t)kernel_thread_start;
	return task_pcb;
}

static void elf_thread_exit(void)
{
	vfs_close(current->exe_file);
	kthread_exit();
}

void elf_thread_start(void)
{
	vfs_node_t elfile = current->exe_file;
	uint8_t *data = kmalloc(elfile->size);
	if (vfs_read(elfile, data, 0, elfile->size) == -1) {
		kfree(data);
		current->exit_status = -1;
	} else {
		uint32_t _start = elf_load(elfile->size, data);
		kfree(data);
		int (*fn)(void *) = (int (*)(void*))_start;
		current->context.kill_eip = (uint32_t)elf_thread_exit;
		current->exit_status = fn(current->arg);
	}
	elf_thread_exit();
}

/* ELF进程创建 */
int32_t elf_thread(const char* path, void *arg, const char *name, int level)
{
	vfs_node_t elfile = vfs_open(path);
	if (elfile == 0) return -1;

	struct task_struct* task_pcb = new_task(arg, name, level);
	task_pcb->exe_file = elfile;
	task_pcb->context.jmp_buf[1] = (uintptr_t)elf_thread_start;
	return task_pcb->pid;
}

/* 获得当前进程 */
struct task_struct *get_current_proc(void)
{
	return current;
}

/* 进程退出函数 */
void kthread_exit()
{
	for (int i = 0; i < 255; i++){
		cfile_t file = current->file_table[i];
		if(file != 0){
			vfs_close(file->handle);
		}
	}
	current->state = TASK_ZOMBIE;
	printk("Task [PID: %d] exited with value %d\n", current->pid, current->exit_status);
	queue_task(current);
	yield();
}

/* 打印当前的所有进程 */
int print_task(void)
{
	printk("PID NAME                     Level             Time\n");
	int p = 0;
	ILIST_FOREACH(node, running_proc_list) {
		++p;
		struct task_struct *pcb = container_of(node, struct task_struct, running_list);
		printk("%-3d %-24s %-17s %d\n", pcb->pid, pcb->name,
                                              pcb->level == KERNEL_TASK ? "Kernel Process" :
                                              pcb->level == SERVICE_TASK ? "Service Process" : "User Process",
                                              pcb->cpu_clock);
	}
	return p;
}

/* 传回特定pid的结构体 */
struct task_struct *found_task_pid(int pid)
{
	ILIST_FOREACH(node, running_proc_list) {
		struct task_struct *pcb = container_of(node, struct task_struct, running_list);
		if (pcb->pid == pid)
			return pcb;
	}
	return NULL;
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
	argv->exit_status = -9;
	printk("Task [Name: %s][PID: %d] Stopped.\n", argv->name, argv->pid);
	if (argv == current) {
		__asm__("jmp *%0" : : "r"(argv->context.kill_eip));
	}
}

/* 杀死全部进程 */
void kill_all_task(void)
{
	ILIST_FOREACH(node, running_proc_list) {
		struct task_struct *pcb = container_of(node, struct task_struct, running_list);
		if (pcb == current) continue;
		task_kill(pcb->pid);
	}
	task_kill(current->pid);
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
