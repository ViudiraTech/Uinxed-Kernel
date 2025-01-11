/*
 *
 *		sched.h
 *		进程调度程序头文件
 *
 *		2024/9/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_SCHED_H_
#define INCLUDE_SCHED_H_

#include "task.h"
#include "idt.h"
#include "ilist.h"

/* 可调度进程链表 */
extern struct ilist_node running_proc_list;

/* 等待进程链表 */
extern struct ilist_node wait_proc_list;

/* 当前运行的任务 */
extern struct task_struct *current;

/* 初始化任务调度 */
void init_sched(void);

/* 允许进程调度 */
void enable_scheduler(void);

/* 停止进程调度 */
void disable_scheduler(void);

/* 任务调度 */
struct task_struct *schedule(void);

/* 任务切换 */
void change_task_to(struct task_struct *next);

void yield(void);

void queue_task(struct task_struct *pcb);

void queue_task_list(struct ilist_node *list);

#endif // INCLUDE_SCHED_H_
