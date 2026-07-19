/*
 *
 *      sched.h
 *      Kernel scheduler header file
 *
 *      2026/7/19 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SCHED_H_
#define INCLUDE_SCHED_H_

#include <intrusive_list.h>
#include <page.h>
#include <stddef.h>
#include <stdint.h>

#define TASK_NAME_LEN      32
#define TASK_KERNEL_STACK  0x10000
#define TASK_DEFAULT_SLICE 5

typedef void (*kthread_entry_t)(void *arg);

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE,
    TASK_IDLE,
} task_state_t;

typedef struct {
        uint64_t rsp;
        uint64_t rbx;
        uint64_t rbp;
        uint64_t r12;
        uint64_t r13;
        uint64_t r14;
        uint64_t r15;
        uint64_t rflags;
        uint64_t rdi;
} task_context_t;

typedef struct task {
        uint64_t         pid;
        task_state_t     state;
        task_context_t   context;
        ilist_node_t     run_node;
        page_directory_t *page_directory;
        uint8_t         *kernel_stack;
        uint64_t         time_slice;
        char             name[TASK_NAME_LEN];
} task_t;

/* Initialize the boot CPU scheduler state. */
void sched_init(void);

/* Start the scheduler. This function does not return. */
void sched_start(void);

/* Create a kernel thread and put it into the ready queue. */
task_t *kthread_create(const char *name, kthread_entry_t entry, void *arg);

/* Yield the current CPU to another runnable task. */
void sched_yield(void);

/* Account one scheduler tick and preempt the current task if needed. */
void sched_tick(void);

/* Finish the current task. This function does not return. */
void task_exit(void);

/* Return the task running on the current CPU. */
task_t *current_task(void);

#endif // INCLUDE_SCHED_H_
