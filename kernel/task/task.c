/*
 *
 *      task.c
 *      Task (thread/process) management
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <task.h>
#include <sched.h>
#include <heap.h>
#include <printk.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
        kthread_entry_t entry;
        void           *arg;
} kthread_bootstrap_t;

static void kthread_trampoline(kthread_bootstrap_t *bootstrap)
{
    kthread_entry_t entry = bootstrap->entry;
    void           *arg   = bootstrap->arg;

    free(bootstrap);
    entry(arg);
    task_exit();
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

void task_name_copy(task_t *task, const char *name)
{
    const char *src = name ? name : "kthread";
    size_t      i   = 0;

    for (; i + 1 < TASK_NAME_LEN && src[i]; i++) task->name[i] = src[i];
    task->name[i] = '\0';
}

task_t *task_alloc(const char *name)
{
    task_t *task = calloc(1, sizeof(task_t));
    if (!task) return NULL;

    task->pid            = scheduler.next_pid++;
    task->page_directory = get_kernel_pagedir();
    task->time_slice     = TASK_DEFAULT_SLICE;
    task->cpu_id         = 0;
    task->process        = NULL;
    task_name_copy(task, name);
    ilist_init(&task->run_node);
    return task;
}

void task_free(task_t *task)
{
    if (!task) return;
    free(task->kernel_stack);
    free(task);
}

task_t *kthread_create_on_cpu(const char *name, kthread_entry_t entry, void *arg, uint32_t cpu_id)
{
    if (!entry) return NULL;
    if (cpu_id >= cpu_scheduler_count) cpu_id = 0;

    kthread_bootstrap_t *bootstrap = malloc(sizeof(kthread_bootstrap_t));
    if (!bootstrap) return NULL;

    task_t *task = task_alloc(name);
    if (!task) {
        free(bootstrap);
        return NULL;
    }
    task->cpu_id = cpu_id;

    bootstrap->entry = entry;
    bootstrap->arg   = arg;

    if (setup_kernel_stack(task, bootstrap)) {
        free(bootstrap);
        task_free(task);
        return NULL;
    }

    spin_lock(&scheduler.lock);
    enqueue_task(task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);

    plogk("task: Created task %llu (%s) on CPU %u.\n", task->pid, task->name, task->cpu_id);
    return task;
}

task_t *kthread_create(const char *name, kthread_entry_t entry, void *arg)
{
    uint32_t cpu_id;

    spin_lock(&scheduler.lock);
    cpu_id = choose_task_cpu_locked();
    spin_unlock(&scheduler.lock);
    return kthread_create_on_cpu(name, entry, arg, cpu_id);
}
