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

/* 内核进程的上下文切换保存的信息 */
struct context {
	uint32_t	esp;
	uint32_t	ebp;
	uint32_t	ebx;
	uint32_t	esi;
	uint32_t	edi;
	uint32_t	eflags;
	fpu_regs_t	fpu_regs;
};

/* 进程控制块 PCB */
struct task_struct {
	int level;					// 进程级别（0-内核进程 1-服务进程 2-用户进程）
	volatile task_state state;	// 进程当前状态
	int pid;					// 进程标识符
	int mem_size;				// 内存利用率
	const char *name;			// 进程名
	void *stack;				// 进程的内核栈地址
	uint32_t program_break;		// 进程堆基址
	uint32_t program_break_end;	// 进程堆尾
	page_directory_t *pgd_dir;	// 进程页表
	bool fpu_flag;				// 是否使用 FPU
	struct context context;		// 进程切换需要的上下文信息
	struct task_struct *next;	// 链表指针
};

/* 全局 pid 值 */
extern int now_pid;

/* 内核进程创建 */
int32_t kernel_thread(int (*fn)(void *), void *arg, const char *name, int level);

/* 进程退出函数 */
void kthread_exit(void);

/* 打印当前的所有进程 */
int print_task(struct task_struct *base, struct task_struct *cur, int count);

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
