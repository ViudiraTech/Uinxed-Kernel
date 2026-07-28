/*
 *
 *      task.c
 *      Task (thread/process) management
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cgroup/cgroup.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/sched.h>
#include <proc/task.h>

#define PID_HASH_BITS 8
#define PID_HASH_SIZE (1 << PID_HASH_BITS)
#define PID_HASH_MASK (PID_HASH_SIZE - 1)

typedef struct pid_entry {
        task_t           *task;
        struct pid_entry *next;
} pid_entry_t;

static pid_entry_t *pid_hash[PID_HASH_SIZE] = {NULL};
static spinlock_t   pid_hash_lock           = {.lock = 0, .rflags = 0};

static uint32_t pid_hash_index(uint64_t pid)
{
    return (uint32_t)(pid & PID_HASH_MASK);
}

/*
 * Register a task for PID-based lookup.
 */
static void pid_hash_add(task_t *task, pid_entry_t *entry)
{
    uint32_t idx = pid_hash_index(task->pid);

    entry->task   = task;
    entry->next   = pid_hash[idx];
    pid_hash[idx] = entry;
}

/*
 * Remove a task from the PID hash table.
 */
static pid_entry_t *pid_hash_remove(task_t *task)
{
    uint32_t      idx      = pid_hash_index(task->pid);
    pid_entry_t **indirect = &pid_hash[idx];

    while (*indirect) {
        pid_entry_t *cur = *indirect;
        if (cur->task == task) {
            *indirect = cur->next;
            return cur;
        }
        indirect = &cur->next;
    }
    return NULL;
}

/*
 * Find a task by PID.
 */
task_t *pid_find_task(uint64_t pid)
{
    uint32_t idx  = pid_hash_index(pid);
    task_t  *task = NULL;

    spin_lock(&pid_hash_lock);
    for (pid_entry_t *entry = pid_hash[idx]; entry; entry = entry->next) {
        if (entry->task->pid == pid) {
            task = entry->task;
            break;
        }
    }
    spin_unlock(&pid_hash_lock);
    return task;
}

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

    task->context.rsp    = (uint64_t)stack;
    task->context.rbx    = 0;
    task->context.rbp    = 0;
    task->context.r12    = (uint64_t)bootstrap;
    task->context.r13    = 0;
    task->context.r14    = 0;
    task->context.r15    = 0;
    task->context.rflags = 0x202;
    task->context.rdi    = (uint64_t)bootstrap;
    return 0;
}

void task_name_copy(task_t *task, const char *name)
{
    const char *src = name ? name : "kthread";
    size_t      i   = 0;

    for (; i + 1 < TASK_NAME_LEN && src[i]; i++) task->name[i] = src[i];
    task->name[i] = '\0';
}

task_t *task_alloc_status(const char *name, int *error)
{
    task_t *parent = NULL;
    task_t *task   = calloc(1, sizeof(task_t));
    if (error) *error = EOK;
    if (!task) {
        if (error) *error = -ENOMEM;
        return NULL;
    }

    pid_entry_t *pid_entry = malloc(sizeof(pid_entry_t));
    if (!pid_entry) {
        free(task);
        if (error) *error = -ENOMEM;
        return NULL;
    }

    task->page_directory = get_kernel_pagedir();
    task->time_slice     = TASK_DEFAULT_SLICE;
    task->cpu_id         = 0;
    task->process        = NULL;
    task->weight         = SCHED_NICE_0_LOAD;
    task->base_weight    = SCHED_NICE_0_LOAD;
    task->pi_weight      = SCHED_NICE_0_LOAD;
    task->blocked_on     = NULL;
    task->thread.fs_base = 0;
    task->thread.gs_base = 0;
    task_name_copy(task, name);
    ilist_init(&task->sched_node);
    ilist_init(&task->timer_node);
    ilist_init(&task->thread_node);
    ilist_init(&task->cgroup_node);

    if (cgroup_root()) parent = current_task();
    int status = cgroup_task_fork(task, parent);
    if (status != EOK) {
        free(pid_entry);
        free(task);
        if (error) *error = status;
        return NULL;
    }

    spin_lock(&pid_hash_lock);
    task->pid = scheduler.next_pid++;
    task->tgid = task->pid;
    pid_hash_add(task, pid_entry);
    spin_unlock(&pid_hash_lock);
    return task;
}

task_t *task_alloc(const char *name)
{
    return task_alloc_status(name, NULL);
}

void task_free(task_t *task)
{
    if (!task) return;

    cgroup_task_exit(task);

    spin_lock(&pid_hash_lock);
    pid_entry_t *pid_entry = pid_hash_remove(task);
    spin_unlock(&pid_hash_lock);
    free(pid_entry);
    free(task->kernel_stack);
    free(task);
}

uint64_t task_next_pid(void)
{
    spin_lock(&pid_hash_lock);
    uint64_t pid = scheduler.next_pid;
    spin_unlock(&pid_hash_lock);
    return pid;
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
    enqueue_task_initial(task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);

    plogk("task: Created task %llu (%s) on CPU %u\n", task->pid, task->name, task->cpu_id);
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
