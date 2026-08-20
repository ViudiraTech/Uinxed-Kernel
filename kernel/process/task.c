/*
 *
 *      task.c
 *      Task (thread/process) management
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/fpu.h>
#include <cgroup/cgroup.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/heap.h>
#include <process/sched.h>
#include <process/task.h>
#include <security/seccomp.h>

#define PID_HASH_BITS 8
#define PID_HASH_SIZE (1 << PID_HASH_BITS)
#define PID_HASH_MASK (PID_HASH_SIZE - 1)

typedef struct pid_entry {
        task_t           *task;
        struct pid_entry *next;
} pid_entry_t;

static pid_entry_t *pid_hash[PID_HASH_SIZE] = {NULL};
static spinlock_t   pid_hash_lock           = {.lock = 0, .rflags = 0};

/* Return the PID hash bucket for a PID */
static uint32_t pid_hash_index(uint64_t pid)
{
    return (uint32_t)(pid & PID_HASH_MASK);
}

/* Register a task for PID-based lookup. */
static void pid_hash_add(task_t *task, pid_entry_t *entry)
{
    uint32_t idx = pid_hash_index(task->pid);

    entry->task   = task;
    entry->next   = pid_hash[idx];
    pid_hash[idx] = entry;
}

/* Remove a task from the PID hash table. */
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

/* Find a task by PID. */
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

/* Test whether a PID is still in use (called with pid_hash_lock held). */
static bool pid_hash_contains_locked(uint64_t pid)
{
    for (pid_entry_t *entry = pid_hash[pid_hash_index(pid)]; entry; entry = entry->next)
        if (entry->task && entry->task->pid == pid) return true;
    return false;
}

/*
 * Allocate a PID in [1, TASK_PID_MAX), reusing freed PIDs once the monotonically
 * increasing next_pid wraps.  A task's PID is removed from the hash in task_free(),
 * so the hash is the authoritative "in use" set.  PIDs 1 (init) and 2 (kthreadd)
 * are naturally reserved because their tasks never leave the hash.
 */
static uint64_t alloc_pid_locked(void)
{
    uint64_t start = scheduler.next_pid;
    if (start < 1 || start >= TASK_PID_MAX) start = 1;

    for (uint64_t i = 0; i < TASK_PID_MAX; i++) {
        uint64_t candidate = start + i;
        if (candidate >= TASK_PID_MAX) candidate -= (TASK_PID_MAX - 1);

        if (!pid_hash_contains_locked(candidate)) {
            scheduler.next_pid = candidate + 1;
            if (scheduler.next_pid >= TASK_PID_MAX) scheduler.next_pid = 1;
            return candidate;
        }
    }
    return 0; // exhausted
}

/* Copy a name into a task's fixed-width name field */
void task_name_copy(task_t *task, const char *name)
{
    if (!task) return;
    const char *src = name ? name : "kthread";
    size_t      i   = 0;

    for (; i + 1 < TASK_NAME_LEN && src[i]; i++) task->name[i] = src[i];

    /*
     * Clear the unused suffix too: task names are copied to fixed-width ABI
     * fields by procfs/prctl, and must never expose a previous exec name.
     */
    for (; i < TASK_NAME_LEN; i++) task->name[i] = '\0';
}

/* Allocate and initialize a task, reporting allocation errors */
task_t *task_alloc_status(const char *name, int *error)
{
    task_t *parent = NULL;
    task_t *task   = calloc(1, sizeof(task_t));
    if (error) *error = EOK;
    if (!task) {
        plogk("task: %s: task control block allocation failed.\n", name ? name : "unnamed");
        if (error) *error = -ENOMEM;
        return NULL;
    }

    pid_entry_t *pid_entry = malloc(sizeof(pid_entry_t));
    if (!pid_entry) {
        plogk("task: %s: PID table entry allocation failed.\n", name ? name : "unnamed");
        free(task);
        if (error) *error = -ENOMEM;
        return NULL;
    }

    if (fpu_task_init(task)) {
        plogk("task: %s: FPU state allocation failed.\n", name ? name : "unnamed");
        free(pid_entry);
        free(task);
        if (error) *error = -ENOMEM;
        return NULL;
    }

    task->page_directory    = get_kernel_pagedir();
    task->time_slice        = TASK_DEFAULT_SLICE;
    task->cpu_id            = 0;
    task->last_cpu          = UINT32_MAX;
    task->last_wake_tick    = 0;
    task->last_migrate_tick = 0;
    task->migration_count   = 0;
    task->start_tick        = sched_ticks();
    sigemptyset(&task->signal_blocked);
    sigemptyset(&task->signal_saved_mask);
    sigemptyset(&task->signal_pending);
    task->signal_restore_mask      = false;
    task->signal_altstack.ss_sp    = NULL;
    task->signal_altstack.ss_size  = 0;
    task->signal_altstack.ss_flags = SS_DISABLE;
    task->process                  = NULL;
    task->weight                   = SCHED_NICE_0_LOAD;
    task->base_weight              = SCHED_NICE_0_LOAD;
    task->pi_weight                = SCHED_NICE_0_LOAD;
    task->blocked_on               = NULL;
    task->thread.fs_base           = 0;
    task->thread.gs_base           = 0;
    ptrace_state_init(&task->ptrace);
    task_name_copy(task, name);
    ilist_init(&task->sched_node);
    ilist_init(&task->timer_node);
    ilist_init(&task->thread_node);
    ilist_init(&task->cgroup_node);
    wait_queue_init(&task->kthread.exit_wait);

    if (cgroup_root()) parent = current_task();
    int status = cgroup_task_fork(task, parent);
    if (status != EOK) {
        plogk("task: %s: cgroup fork failed (%d)\n", name ? name : "unnamed", status);
        free(pid_entry);
        fpu_task_destroy(task);
        free(task);
        if (error) *error = status;
        return NULL;
    }

    spin_lock(&pid_hash_lock);
    task->pid = alloc_pid_locked();
    if (!task->pid) {
        spin_unlock(&pid_hash_lock);
        plogk("task: %s: PID space exhausted.\n", name ? name : "unnamed");
        cgroup_task_exit(task);
        free(pid_entry);
        fpu_task_destroy(task);
        free(task);
        if (error) *error = -EAGAIN;
        return NULL;
    }
    task->tgid = task->pid;
    pid_hash_add(task, pid_entry);
    __atomic_add_fetch(&scheduler.tasks_created, 1, __ATOMIC_RELAXED);
    spin_unlock(&pid_hash_lock);
    return task;
}

/* Allocate and initialize a task */
task_t *task_alloc(const char *name)
{
    return task_alloc_status(name, NULL);
}

/* Destroy a task, releasing its PID entry, kernel stack and FPU state */
void task_free(task_t *task)
{
    if (!task) return;

    seccomp_task_release(task);
    cgroup_task_exit(task);

    spin_lock(&pid_hash_lock);
    pid_entry_t *pid_entry = pid_hash_remove(task);
    spin_unlock(&pid_hash_lock);
    free(pid_entry);
    free(task->kernel_stack);
    fpu_task_destroy(task);
    free(task);
}
