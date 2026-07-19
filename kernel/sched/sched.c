/*
 *
 *      sched.c
 *      Kernel scheduler
 *
 *      2026/7/19 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <debug.h>
#include <heap.h>
#include <intrusive_list.h>
#include <page.h>
#include <printk.h>
#include <sched.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
        kthread_entry_t entry;
        void           *arg;
} kthread_bootstrap_t;

static scheduler_t scheduler;
static task_t      boot_task;
static uint8_t     boot_stack_marker;

void context_switch(task_context_t *prev, task_context_t *next);

__attribute__((naked)) void context_switch(task_context_t *prev, task_context_t *next)
{
    (void)prev;
    (void)next;
    __asm__ volatile("movq %rsp, 0(%rdi)\n\t"
                     "movq %rbx, 8(%rdi)\n\t"
                     "movq %rbp, 16(%rdi)\n\t"
                     "movq %r12, 24(%rdi)\n\t"
                     "movq %r13, 32(%rdi)\n\t"
                     "movq %r14, 40(%rdi)\n\t"
                     "movq %r15, 48(%rdi)\n\t"
                     "pushfq\n\t"
                     "popq 56(%rdi)\n\t"
                     "movq 8(%rsi), %rbx\n\t"
                     "movq 16(%rsi), %rbp\n\t"
                     "movq 24(%rsi), %r12\n\t"
                     "movq 32(%rsi), %r13\n\t"
                     "movq 40(%rsi), %r14\n\t"
                     "movq 48(%rsi), %r15\n\t"
                     "pushq 56(%rsi)\n\t"
                     "popfq\n\t"
                     "movq 64(%rsi), %rdi\n\t"
                     "movq 0(%rsi), %rsp\n\t"
                     "ret\n\t");
}

static task_t *node_to_task(ilist_node_t *node)
{ return (task_t *)((uint8_t *)node - offsetof(task_t, run_node)); }

static void task_name_copy(task_t *task, const char *name)
{
    const char *src = name ? name : "kthread";
    size_t      i   = 0;

    for (; i + 1 < TASK_NAME_LEN && src[i]; i++) task->name[i] = src[i];
    task->name[i] = '\0';
}

static void enqueue_task(task_t *task)
{
    task->state = TASK_READY;
    task->wake_tick = 0;
    ilist_insert_before(&scheduler.ready_queue, &task->run_node);
}

static void sleep_task(task_t *task, uint64_t wake_tick)
{
    task->state     = TASK_SLEEPING;
    task->wake_tick = wake_tick;
    ilist_insert_before(&scheduler.sleep_queue, &task->run_node);
}

static task_t *pick_next_task(void)
{
    if (ilist_is_empty(&scheduler.ready_queue)) return scheduler.idle;

    ilist_node_t *node = scheduler.ready_queue.next;
    ilist_remove(node);
    return node_to_task(node);
}

static int has_ready_task(void)
{ return !ilist_is_empty(&scheduler.ready_queue); }

static void wake_sleeping_tasks(void)
{
    ilist_node_t *node = scheduler.sleep_queue.next;

    while (node != &scheduler.sleep_queue) {
        ilist_node_t *next = node->next;
        task_t       *task = node_to_task(node);

        if (task->wake_tick <= scheduler.ticks) {
            ilist_remove(node);
            enqueue_task(task);
        }
        node = next;
    }
}

static void kthread_trampoline(kthread_bootstrap_t *bootstrap)
{
    kthread_entry_t entry = bootstrap->entry;
    void           *arg   = bootstrap->arg;

    free(bootstrap);
    entry(arg);
    task_exit();
}

static task_t *task_alloc(const char *name)
{
    task_t *task = calloc(1, sizeof(task_t));
    if (!task) return NULL;

    task->pid            = scheduler.next_pid++;
    task->page_directory = get_kernel_pagedir();
    task->time_slice     = TASK_DEFAULT_SLICE;
    task_name_copy(task, name);
    ilist_init(&task->run_node);
    return task;
}

static int setup_kernel_stack(task_t *task, kthread_bootstrap_t *bootstrap)
{
    task->kernel_stack = malloc(TASK_KERNEL_STACK);
    if (!task->kernel_stack) return 1;

    uint64_t *stack = (uint64_t *)ALIGN_DOWN((uint64_t)(task->kernel_stack + TASK_KERNEL_STACK), 16ULL);
    *(--stack)      = 0;
    *(--stack)      = (uint64_t)kthread_trampoline;

    task->context.rsp = (uint64_t)stack;
    task->context.rbx = 0;
    task->context.rbp = 0;
    task->context.r12 = (uint64_t)bootstrap;
    task->context.r13 = 0;
    task->context.r14 = 0;
    task->context.r15 = 0;
    task->context.rflags = 0x202;
    task->context.rdi = (uint64_t)bootstrap;
    return 0;
}

static void idle_thread(void *arg)
{
    (void)arg;
    while (1) {
        enable_intr();
        __asm__ volatile("hlt");
        disable_intr();
        sched_yield();
    }
}

void sched_init(void)
{
    memset(&scheduler, 0, sizeof(scheduler));
    ilist_init(&scheduler.ready_queue);
    ilist_init(&scheduler.sleep_queue);
    scheduler.next_pid = 1;

    memset(&boot_task, 0, sizeof(boot_task));
    boot_task.pid            = 0;
    boot_task.state          = TASK_RUNNING;
    boot_task.page_directory = get_kernel_pagedir();
    boot_task.kernel_stack   = &boot_stack_marker;
    boot_task.time_slice     = TASK_DEFAULT_SLICE;
    task_name_copy(&boot_task, "kernel");
    ilist_init(&boot_task.run_node);

    scheduler.current = &boot_task;
    scheduler.idle    = kthread_create("idle", idle_thread, NULL);
    if (!scheduler.idle) panic("sched: Cannot create idle task.");
    spin_lock(&scheduler.lock);
    ilist_remove(&scheduler.idle->run_node);
    spin_unlock(&scheduler.lock);
    scheduler.idle->state = TASK_IDLE;

    plogk("sched: Initialized kernel scheduler.\n");
}

task_t *kthread_create(const char *name, kthread_entry_t entry, void *arg)
{
    if (!entry) return NULL;

    kthread_bootstrap_t *bootstrap = malloc(sizeof(kthread_bootstrap_t));
    if (!bootstrap) return NULL;

    task_t *task = task_alloc(name);
    if (!task) {
        free(bootstrap);
        return NULL;
    }

    bootstrap->entry = entry;
    bootstrap->arg   = arg;

    if (setup_kernel_stack(task, bootstrap)) {
        free(bootstrap);
        free(task);
        return NULL;
    }

    spin_lock(&scheduler.lock);
    enqueue_task(task);
    spin_unlock(&scheduler.lock);

    plogk("sched: Created task %llu (%s).\n", task->pid, task->name);
    return task;
}

void sched_yield(void)
{
    spin_lock(&scheduler.lock);

    task_t *prev = scheduler.current;
    task_t *next = pick_next_task();

    if (prev == next) {
        spin_unlock(&scheduler.lock);
        return;
    }

    if (prev->state == TASK_RUNNING && prev != scheduler.idle) {
        prev->time_slice = TASK_DEFAULT_SLICE;
        enqueue_task(prev);
    }
    if (next->state != TASK_IDLE) next->state = TASK_RUNNING;
    scheduler.current = next;

    spin_unlock(&scheduler.lock);
    context_switch(&prev->context, &next->context);
}

void sched_start(void)
{
    disable_intr();
    scheduler.current->state = TASK_BLOCKED;
    scheduler.started        = 1;
    enable_intr();
    sched_yield();
    panic("sched: bootstrap task resumed.");
}

void task_sleep_ticks(uint64_t ticks)
{
    if (!ticks) {
        sched_yield();
        return;
    }

    disable_intr();
    spin_lock(&scheduler.lock);
    sleep_task(scheduler.current, scheduler.ticks + ticks);
    spin_unlock(&scheduler.lock);

    sched_yield();
    enable_intr();
}

void sched_tick(void)
{
    if (!scheduler.started || !scheduler.current) return;

    spin_lock(&scheduler.lock);
    scheduler.ticks++;
    wake_sleeping_tasks();
    spin_unlock(&scheduler.lock);

    task_t *current = scheduler.current;
    if (current == scheduler.idle) {
        if (has_ready_task()) sched_yield();
        return;
    }

    if (current->state != TASK_RUNNING) return;
    if (current->time_slice) current->time_slice--;
    if (current->time_slice) return;

    current->time_slice = TASK_DEFAULT_SLICE;
    if (has_ready_task()) sched_yield();
}

uint64_t sched_ticks(void)
{ return scheduler.ticks; }

void task_exit(void)
{
    disable_intr();
    spin_lock(&scheduler.lock);
    scheduler.current->state = TASK_ZOMBIE;
    spin_unlock(&scheduler.lock);

    sched_yield();
    panic("sched: zombie task resumed.");
}

task_t *current_task(void)
{ return scheduler.current; }
