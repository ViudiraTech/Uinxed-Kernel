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

typedef uint32_t pgd_t;
typedef uint32_t pid_t;

/* 进程内存地址结构 */
struct mm_struct {
	pgd_t *pgd_dir; // 进程页表
};

/* 进程控制块 PCB */
struct task_struct {
	volatile task_state state;	// 进程当前状态
	pid_t pid;					// 进程标识符
	char *name;					// 进程名
	void *stack;				// 进程的内核栈地址
	bool fpu_flag;				// 是否使用 FPU
	struct mm_struct *mm;		// 当前进程的内存地址映像
	struct context context;		// 进程切换需要的上下文信息
	struct task_struct *next;	// 链表指针
};

/* 全局 pid 值 */
extern pid_t now_pid;

/* 内核进程创建 */
int32_t kernel_thread(int (*fn)(void *), void *arg, char *name);

/* 进程退出函数 */
void kthread_exit(void);

/* 打印当前的所有进程 */
int print_task(struct task_struct *base, struct task_struct *cur, int count);

/* 传回特定pid的结构体 */
struct task_struct *found_task_pid(int pid);

/* 杀死指定进程 */
void task_kill(int pid);

#endif // INCLUDE_TASK_H_
