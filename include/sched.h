/*
 *
 *      sched.h
 *      Kernel scheduler header file
 *
 *      2026/7/19 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SCHED_H_
#define INCLUDE_SCHED_H_

#include <task.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
        task_t      *current;
        task_t      *idle;
        ilist_node_t ready_queue;
        ilist_node_t sleep_queue;
        spinlock_t   lock;
        uint64_t     next_pid;
        uint64_t     ticks;
        int          started;
} scheduler_t;

typedef struct {
        task_t      *current;
        task_t      *idle;
        ilist_node_t ready_queue;
        uint64_t     ready_count;
        uint64_t     reschedule_ipis;
        uint8_t      online;
} cpu_scheduler_t;

/* Scheduler state (used by task subsystem) */
extern scheduler_t      scheduler;
extern cpu_scheduler_t *cpu_schedulers;
extern uint32_t         cpu_scheduler_count;

/* Enqueue a task onto its assigned CPU's ready queue */
void enqueue_task(task_t *task);

/* Request a reschedule IPI for the target task's CPU */
void request_task_cpu(task_t *task);

/* Move a non-running task to another CPU */
int task_set_cpu(task_t *task, uint32_t cpu_id);

/* Yield the current CPU to another runnable task */
void sched_yield(void);

/* Sleep the current task for at least the specified number of scheduler ticks */
void task_sleep_ticks(uint64_t ticks);

/* Block the current task until another kernel path wakes it */
void task_block(void);

/* Wake a blocked or sleeping task and put it back in the ready queue */
int task_wakeup(task_t *task);

/* Account one scheduler tick and preempt the current task if needed */
void sched_tick(void);

/* Return the scheduler tick count */
uint64_t sched_ticks(void);

/* Finish the current task. This function does not return */
void task_exit(void);

/* Return the task running on the current CPU */
task_t *current_task(void);

/* Initialize the boot CPU scheduler state */
void sched_init(void);

/* Mark an application processor as ready for scheduler participation */
void sched_ap_online(uint32_t cpu_id);

/* Start scheduling on an application processor. This function does not return */
void sched_ap_start(uint32_t cpu_id);

/* Handle a reschedule request delivered by the local APIC */
void sched_ipi_reschedule(void);

/* Return the number of scheduler CPU slots initialized */
uint32_t sched_cpu_count(void);

/* Start the scheduler. This function does not return */
void sched_start(void);

/* Choose the best CPU for a new task (used by task subsystem) */
uint32_t choose_task_cpu_locked(void);

#endif // INCLUDE_SCHED_H_
