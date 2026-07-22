/*
 *
 *      task.h
 *      Task (thread/process) management header file
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TASK_H_
#define INCLUDE_TASK_H_

#include <libs/data/rbtree.h>
#include <libs/glist/intrusive_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

typedef struct process process_t;

#define TASK_NAME_LEN      32
#define TASK_KERNEL_STACK  0x10000
#define TASK_DEFAULT_SLICE 5

typedef void (*kthread_entry_t)(void *arg);

typedef struct wait_queue {
        ilist_node_t tasks;
        spinlock_t   lock;
} wait_queue_t;

typedef struct task task_t;

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_IDLE,
} task_state_t;

typedef struct {
        uint64_t fs_base;
        uint64_t gs_base;
} thread_struct_t;

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

struct task {
        uint64_t          pid;
        task_state_t      state;
        task_context_t    context;
        thread_struct_t   thread;     /* per-thread arch state (fs_base, gs_base) */
        rb_node_t         run_node;   /* EEVDF red-black tree node */
        ilist_node_t      sched_node; /* sleep / wait_queue linkage */
        ilist_node_t      timer_node; /* timed-waiter linkage (scheduler.timer_queue) */
        page_directory_t *page_directory;
        uint8_t          *kernel_stack;
        uint64_t          time_slice;
        uint64_t          wake_tick;
        wait_queue_t     *wait_queue;
        uint32_t          cpu_id;
        char              name[TASK_NAME_LEN];
        process_t        *process;
        /* ---- EEVDF scheduling fields ---- */
        uint64_t vruntime; /* virtual runtime */
        uint64_t deadline; /* virtual deadline */
        int64_t  vlag;     /* virtual lag for placement */
        uint32_t weight;   /* scheduling weight (NICE_0_LOAD = 1024) */
};

/* Initialize a wait queue */
void wait_queue_init(wait_queue_t *queue);

/* Block the current task on a wait queue */
void wait_queue_wait(wait_queue_t *queue);

/*
 * Two-phase wait: prepare adds the current task to the wait queue
 * (caller must hold the external lock that protects the condition),
 * sleep actually blocks.  This eliminates the lost-wakeup window.
 *
 * Usage:
 *   spin_lock(&external_lock);
 *   // ... check condition, create entry ...
 *   wait_queue_prepare(&wq);   // add task to queue under lock
 *   spin_unlock(&external_lock);
 *   wait_queue_sleep();        // block
 *   // ... after wakeup, re-check condition ...
 */
void wait_queue_prepare(wait_queue_t *queue);
void wait_queue_sleep(void);

/*
 * Two-phase wait with timeout: prepare under lock, then sleep with
 * a deadline.  The scheduler will wake the task when either
 * wait_queue_wake_one() is called or the deadline (in ticks) expires.
 *
 * Returns 0 if woken normally, -ETIMEDOUT if the deadline expired.
 * The caller must re-check the condition under the external lock after
 * this function returns.
 */
int wait_queue_wait_timed(wait_queue_t *queue, uint64_t deadline_ticks);

/* Wake one task from a wait queue */
task_t *wait_queue_wake_one(wait_queue_t *queue);

/* Wake every task from a wait queue */
uint64_t wait_queue_wake_all(wait_queue_t *queue);

/* Allocate a task structure */
task_t *task_alloc(const char *name);

/* Free a task structure */
void task_free(task_t *task);

/* Create a kernel thread and put it into the ready queue */
task_t *kthread_create(const char *name, kthread_entry_t entry, void *arg);

/* Create a kernel thread on a specific CPU */
task_t *kthread_create_on_cpu(const char *name, kthread_entry_t entry, void *arg, uint32_t cpu_id);

/* Copy a name into a task's name field */
void task_name_copy(task_t *task, const char *name);

#endif /* INCLUDE_TASK_H_ */