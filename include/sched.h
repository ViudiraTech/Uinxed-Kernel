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
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>

#define TASK_NAME_LEN      32
#define TASK_KERNEL_STACK  0x10000
#define TASK_DEFAULT_SLICE 5

typedef void (*kthread_entry_t)(void *arg);
typedef struct wait_queue wait_queue_t;

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
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
        uint64_t         wake_tick;
        wait_queue_t    *wait_queue;
        char             name[TASK_NAME_LEN];
} task_t;

struct wait_queue {
        ilist_node_t tasks;
        spinlock_t   lock;
};

/* Initialize the boot CPU scheduler state. */
void sched_init(void);

/* Start the scheduler. This function does not return. */
void sched_start(void);

/* Create a kernel thread and put it into the ready queue. */
task_t *kthread_create(const char *name, kthread_entry_t entry, void *arg);

/* Yield the current CPU to another runnable task. */
void sched_yield(void);

/* Sleep the current task for at least the specified number of scheduler ticks. */
void task_sleep_ticks(uint64_t ticks);

/* Block the current task until another kernel path wakes it. */
void task_block(void);

/* Wake a blocked or sleeping task and put it back in the ready queue. */
int task_wakeup(task_t *task);

/* Initialize a wait queue. */
void wait_queue_init(wait_queue_t *queue);

/* Block the current task on a wait queue. */
void wait_queue_wait(wait_queue_t *queue);

/* Wake one task from a wait queue. */
task_t *wait_queue_wake_one(wait_queue_t *queue);

/* Wake every task from a wait queue. */
uint64_t wait_queue_wake_all(wait_queue_t *queue);

/* Account one scheduler tick and preempt the current task if needed. */
void sched_tick(void);

/* Return the scheduler tick count. */
uint64_t sched_ticks(void);

/* Finish the current task. This function does not return. */
void task_exit(void);

/* Return the task running on the current CPU. */
task_t *current_task(void);

#endif // INCLUDE_SCHED_H_
