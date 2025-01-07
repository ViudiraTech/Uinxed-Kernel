/*
 *
 *		task.h
 *		多任务处理程序头文件
 *
 *		2024/9/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_TASK_H_
#define INCLUDE_TASK_H_

#include "types.h"
#include "memory.h"
#include "syscall.h"
#include "vfs.h"

/* 进程状态描述 */
typedef
enum task_state {
	TASK_UNINIT = 0,	// 未初始化
	TASK_SLEEPING = 1,	// 睡眠中
	TASK_RUNNABLE = 2,	// 可运行(也许正在运行)
	TASK_ZOMBIE = 3,	// 僵尸状态
	TASK_DEATH = 4		// 死亡状态
} task_state;

/* 进程级别描述 */
typedef
enum task_level {
	KERNEL_TASK = 0,	// 内核进程
	SERVICE_TASK= 1,	// 服务进程
	USER_TASK = 2		// 用户进程
} task_level;

/* 浮点单元的寄存器状态 */
typedef struct __attribute__((packed)) fpu_regs {
	uint16_t	control;
	uint16_t	RESERVED1;
	uint16_t	status;
	uint16_t	RESERVED2;
	uint16_t	tag;
	uint16_t	RESERVED3;
	uint32_t	fip0;
	uint32_t	fop0;
	uint32_t	fdp0;
	uint32_t	fdp1;
	uint8_t		regs[80];
} fpu_regs_t;

/* 进程控制块 PCB */
struct task_struct {
	int level;					// 进程级别（0-内核进程 1-服务进程 2-用户进程）
	volatile task_state state;	// 进程当前状态
	int pid;					// 进程标识符
	int mem_size;				// 内存利用率
	const char *name;			// 进程名
	uintptr_t pgd_dir;	// 进程页表
	cfile_t file_table[255];	// 进程文件句柄表
	union {
		vfs_node_t exe_file;		// 可执行文件
		int (*fn)(void *);
	};
	void *arg;
	int fpu_flag;				// 是否使用 FPU
	uint32_t cpu_clock;			// CPU运行时间片
	uint32_t sche_time;			// 进程剩余的可运行时间片c
	uintptr_t jmp_buf[5];		// 进程切换需要的上下文信息
	uintptr_t kill_eip;		// 杀进程切换到这个eip
	int exit_status;		// 进程退出状态码
	struct task_struct *next;	// 链表指针
};

/* 全局 pid 值 */
extern int now_pid;

/* 内核进程创建 */
int32_t kernel_thread(int (*fn)(void *), void *arg, const char *name, int level);

/* ELF进程创建 */
int32_t elf_thread(const char* path, void *arg, const char *name, int level);

/* 获得当前进程 */
struct task_struct *get_current_proc(void);

/* 进程退出函数 */
void kthread_exit(void);

/* 打印当前的所有进程 */
int print_task(void);

/* 传回特定pid的结构体 */
struct task_struct *found_task_pid(int pid);

/* 杀死指定进程 */
void task_kill(int pid);

/* 杀死全部进程 */
void kill_all_task(void);

/* 使进程陷入等待 */
void wait_task(struct task_struct *task);

/* 使进程回归运行 */
void start_task(struct task_struct *task);

#endif // INCLUDE_TASK_H_
