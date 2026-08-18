/*
 *
 *      kthread.c
 *      Kernel thread (kthreadd) subsystem
 *
 *      2026/8/14 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 */

#include <kernel/debug/debug.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/list/singly_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/heap.h>
#include <process/kthread.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <sync/spin_lock.h>

/* kthreadd sleeps here while it has no create requests or children to reap. */
wait_queue_t kthreadd_wait;

/* Pending creation requests (head = oldest).  Protected by kthread_create_lock. */
static slist_t    kthread_create_list;
static spinlock_t kthread_create_lock = {.lock = 0, .rflags = 0};

static void create_kthread(kthread_create_info_t *info);
static void kthread_reap_children(void);

/* First frame of a kernel thread: run its function, then exit with the return code. */
static void kthread_trampoline(kthread_bootstrap_t *bootstrap)
{
    kthread_entry_t entry = bootstrap->entry;
    void           *arg   = bootstrap->arg;

    free(bootstrap);
    int ret = entry(arg);
    kthread_exit(ret);
}

/*
 * Allocate a task's kernel stack and seed it so the trampoline runs with the
 * bootstrap record in rdi. Returns 0 on success, non-zero on OOM.
 */
static int setup_kthread_stack(task_t *task, kthread_bootstrap_t *bootstrap)
{
    task->kernel_stack = malloc(TASK_KERNEL_STACK);
    if (!task->kernel_stack) {
        plogk("kthread: %s: kernel stack allocation failed (%d bytes)\n", task->name, TASK_KERNEL_STACK);
        return 1;
    }

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

/*
 * kthreadd (PID 2) loop: drain create requests, reap exited kthread children,
 * then sleep on kthreadd_wait until either event recurs.
 */
static void kthreadd_main(void *unused)
{
    (void)unused;

    for (;;) {
        /* Drain the creation request list. */
        for (;;) {
            kthread_create_info_t *info = NULL;
            spin_lock(&kthread_create_lock);
            if (slist_size(&kthread_create_list) > 0) slist_remove_head(&kthread_create_list, (void **)&info);
            spin_unlock(&kthread_create_lock);
            if (!info) break;
            create_kthread(info);
        }

        /* Reap exited kernel-thread children. */
        kthread_reap_children();

        /*
         * Block until a create request (or a child exit) wakes us.  The
         * wait-queue lock is held across the condition check and prepare so a
         * concurrent kthread_create()/process_exit() wake cannot be lost: both
         * wakers publish their work first and then acquire kthreadd_wait.lock.
         */
        spin_lock(&kthreadd_wait.lock);
        spin_lock(&kthread_create_lock);
        bool idle = slist_size(&kthread_create_list) == 0;
        spin_unlock(&kthread_create_lock);

        if (!idle) {
            spin_unlock(&kthreadd_wait.lock);
            continue;
        }

        wait_queue_prepare(&kthreadd_wait);
        spin_unlock(&kthreadd_wait.lock);
        wait_queue_sleep();
    }
}

/* Reap every zombie kthread child, reusing the wait4 reap path. */
static void kthread_reap_children(void)
{
    for (;;) {
        pid_t reaped = 0;
        int   status = 0;
        int   ret    = process_wait_select(-1, &status, PROCESS_WAIT_NOHANG, &reaped);
        if (ret != EOK || reaped == 0) return;
    }
}

/*
 * Bootstrap kthreadd directly (not via kthread_create) as PID 2, the parent of
 * every subsequent kernel thread.
 */
void kthreadd_init(void)
{
    slist_init(&kthread_create_list);
    wait_queue_init(&kthreadd_wait);

    int     err  = EOK;
    task_t *task = task_alloc_status("kthreadd", &err);
    if (!task) panic("kthreadd: task allocation failed (%d)", err);
    if (task->pid != 2) panic("kthreadd: expected PID 2, got %llu", task->pid);

    task->kernel_stack = malloc(TASK_KERNEL_STACK);
    if (!task->kernel_stack) panic("kthreadd: kernel stack allocation failed.");

    uint64_t *stack      = (uint64_t *)ALIGN_DOWN((uint64_t)(task->kernel_stack + TASK_KERNEL_STACK), 16ULL);
    *(--stack)           = 0;
    *(--stack)           = (uint64_t)kthreadd_main;
    task->context.rsp    = (uint64_t)stack;
    task->context.rflags = 0x202;
    task->context.rdi    = 0;

    process_t *proc = process_create_kthread(task, "kthreadd");
    if (!proc) panic("kthreadd: process bundle allocation failed.");

    /* kthreadd is the root of the kernel-thread tree; it has no parent. */
    kthreadd_process = proc;

    spin_lock(&scheduler.lock);
    enqueue_task_initial(task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);
    plogk("kthread: kthreadd started (pid=2)\n");
}

/*
 * Enqueue a creation request for kthreadd and block until it has allocated the
 * task. Returns the (not-yet-runnable) task, or NULL with no errno on OOM.
 */
static task_t *__kthread_create(const char *name, kthread_entry_t entry, void *arg, uint32_t cpu_id, bool pinned)
{
    kthread_create_info_t *info = malloc(sizeof(kthread_create_info_t));
    if (!info) {
        plogk("kthread: %s: create request allocation failed.\n", name ? name : "unnamed");
        return NULL;
    }

    const char *src = name ? name : "kthread";
    size_t      i   = 0;
    for (; i + 1 < TASK_NAME_LEN && src[i]; i++) info->name[i] = src[i];
    for (; i < TASK_NAME_LEN; i++) info->name[i] = '\0';

    info->entry     = entry;
    info->arg       = arg;
    info->cpu_id    = cpu_id;
    info->pinned    = pinned;
    info->result    = NULL;
    info->error     = EOK;
    info->completed = false;
    wait_queue_init(&info->done);

    spin_lock(&kthread_create_lock);
    slist_insert_tail(&kthread_create_list, info);
    spin_unlock(&kthread_create_lock);

    spin_lock(&kthreadd_wait.lock);
    wait_queue_wake_one(&kthreadd_wait);
    spin_unlock(&kthreadd_wait.lock);

    for (;;) {
        spin_lock(&info->done.lock);
        if (info->completed) {
            spin_unlock(&info->done.lock);
            break;
        }
        wait_queue_prepare(&info->done);
        spin_unlock(&info->done.lock);
        wait_queue_sleep();
    }

    task_t *result = info->result;
    free(info);
    return result;
}

/*
 * Perform the allocation for one request and always complete it, waking the
 * waiter on both success and failure so kthread_create() never hangs.
 */
static void create_kthread(kthread_create_info_t *info)
{
    int     err  = EOK;
    task_t *task = task_alloc_status(info->name, &err);
    if (!task) {
        info->error = err;
        goto out;
    }

    if (info->pinned) {
        if (info->cpu_id >= cpu_scheduler_count) info->cpu_id = 0;
        task->cpu_id = info->cpu_id;
    } else {
        spin_lock(&scheduler.lock);
        task->cpu_id = choose_task_cpu_locked();
        spin_unlock(&scheduler.lock);
    }

    kthread_bootstrap_t *bootstrap = malloc(sizeof(kthread_bootstrap_t));
    if (!bootstrap) {
        plogk("kthread: %s: bootstrap allocation failed.\n", info->name);
        task_free(task);
        info->error = -ENOMEM;
        goto out;
    }
    bootstrap->entry = info->entry;
    bootstrap->arg   = info->arg;

    if (setup_kthread_stack(task, bootstrap)) {
        free(bootstrap);
        task_free(task);
        info->error = -ENOMEM;
        goto out;
    }

    process_t *proc = process_create_kthread(task, info->name);
    if (!proc) {
        plogk("kthread: %s: process bundle allocation failed.\n", info->name);
        free(bootstrap);
        task_free(task);
        info->error = -ENOMEM;
        goto out;
    }
    task->kthread.data = info->arg;
    task->state        = TASK_STOPPED; /* runnable only after kthread_run/wake_up_process */
    info->result       = task;
    info->error        = EOK;
out:
    spin_lock(&info->done.lock);
    info->completed = true;
    wait_queue_wake_all(&info->done);
    spin_unlock(&info->done.lock);
}

/* Create a kernel thread (returns a not-yet-runnable task). */
task_t *kthread_create(const char *name, kthread_entry_t entry, void *arg)
{
    if (!entry) return NULL;
    return __kthread_create(name, entry, arg, 0, false);
}

/* Create a kernel thread pinned to a specific CPU (not-yet-runnable). */
task_t *kthread_create_on_cpu(const char *name, kthread_entry_t entry, void *arg, uint32_t cpu_id)
{
    if (!entry) return NULL;
    return __kthread_create(name, entry, arg, cpu_id, true);
}

/* Create a kernel thread and immediately make it runnable. */
task_t *kthread_run(const char *name, kthread_entry_t entry, void *arg)
{
    task_t *task = kthread_create(name, entry, arg);
    if (!task) return NULL;
    wake_up_process(task);
    return task;
}

/* Create a CPU-pinned kernel thread and immediately make it runnable. */
task_t *kthread_run_on_cpu(const char *name, kthread_entry_t entry, void *arg, uint32_t cpu_id)
{
    task_t *task = kthread_create_on_cpu(name, entry, arg, cpu_id);
    if (!task) return NULL;
    wake_up_process(task);
    return task;
}

/* Make a created (or blocked) kernel thread runnable via its initial enqueue. */
void wake_up_process(task_t *task)
{
    if (!task) return;
    spin_lock(&scheduler.lock);
    enqueue_task_initial(task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);
}

/* Block until the kernel thread publishes its exit code, then return it. */
static int kthread_wait_exit(task_t *task)
{
    for (;;) {
        spin_lock(&task->kthread.exit_wait.lock);
        if (task->kthread.exited) {
            int code = task->kthread.exit_code;
            spin_unlock(&task->kthread.exit_wait.lock);
            return code;
        }
        wait_queue_prepare(&task->kthread.exit_wait);
        spin_unlock(&task->kthread.exit_wait.lock);
        wait_queue_sleep();
    }
}

/* Request a kernel thread to stop and wait for its exit code. */
int kthread_stop(task_t *task)
{
    if (!task) return -EINVAL;
    if (!(task->flags & PF_KTHREAD)) return -EINVAL;

    /*
     * Hold a reference on the process bundle so kthreadd cannot free the task
     * (and its exit code) while we wait for it to exit.
     */
    process_t *proc = process_find_get((pid_t)task->pid);
    if (!proc || proc->task != task) {
        if (proc) process_put(proc);
        return -ESRCH;
    }

    spin_lock(&task->kthread.exit_wait.lock);
    if (!task->kthread.exited) {
        task->kthread.should_stop = true;
        spin_unlock(&task->kthread.exit_wait.lock);

        /*
         * A kthread that was created but never woken (kthread_create without
         * kthread_run) is still TASK_STOPPED.  task_wakeup() rejects STOPPED
         * tasks, so it would silently fail and we would block forever in
         * kthread_wait_exit().  Give it its initial enqueue instead so the
         * trampoline runs, sees kthread_should_stop(), and exits.
         */
        if (task->state == TASK_STOPPED) {
            wake_up_process(task);
        } else {
            task_wakeup(task);
        }
    } else {
        spin_unlock(&task->kthread.exit_wait.lock);
    }
    int code = kthread_wait_exit(task);

    /*
     * The exit code is published before the worker switches off its stack.
     * Defer the final free until that switch has happened, matching the
     * busy-wait performed by kthread_reap_children().
     */
    while (__atomic_load_n(&task->on_cpu, __ATOMIC_ACQUIRE)) sched_yield();

    process_put(proc);
    return code;
}

/* True if the current kernel thread has been asked to stop. */
bool kthread_should_stop(void)
{
    task_t *task = current_task();
    return task && (task->flags & PF_KTHREAD) && task->kthread.should_stop;
}

/* Publish the exit code and tear down the current kernel thread.  Does not return. */
void kthread_exit(int exit_code)
{
    task_t *self = current_task();
    if (self && (self->flags & PF_KTHREAD)) {
        spin_lock(&self->kthread.exit_wait.lock);
        self->kthread.exited    = true;
        self->kthread.exit_code = exit_code;
        wait_queue_wake_all(&self->kthread.exit_wait);
        spin_unlock(&self->kthread.exit_wait.lock);
    }
    process_exit(exit_code); /* does not return */
}

/* Return the argument passed to the current kernel thread. */
void *kthread_data(void)
{
    task_t *task = current_task();
    return task ? task->kthread.data : NULL;
}

#define KERNEL_WORKER_MAX 64

static kernel_worker_t kernel_worker_table[KERNEL_WORKER_MAX];
static size_t          kernel_worker_count;
static bool            kernel_workers_started;

/* Register a kernel worker for unified creation (see kthread.h). */
int kernel_worker_register(const char *name, kthread_entry_t entry, void *arg, task_t **slot)
{
    if (!entry) return -EINVAL;

    /* A late registration (hot-plug after boot) creates the worker now. */
    if (kernel_workers_started) {
        task_t *task = kthread_run(name, entry, arg);
        if (slot) *slot = task;
        return task ? 0 : -ENOMEM;
    }

    if (kernel_worker_count >= KERNEL_WORKER_MAX) {
        plogk("kthread: worker registry full, dropping '%s'\n", name ? name : "unnamed");
        return -ENOMEM;
    }

    kernel_worker_t *worker = &kernel_worker_table[kernel_worker_count++];
    worker->name            = name;
    worker->entry           = entry;
    worker->arg             = arg;
    worker->slot            = slot;
    return 0;
}

/* Create every registered worker; the single boot-time kthread_run site. */
void kernel_workers_start(void)
{
    kernel_workers_started = true;
    for (size_t i = 0; i < kernel_worker_count; i++) {
        kernel_worker_t *worker = &kernel_worker_table[i];
        task_t          *task   = kthread_run(worker->name, worker->entry, worker->arg);
        if (worker->slot) *worker->slot = task;
        if (!task) plogk("kthread: worker '%s' failed to start.\n", worker->name ? worker->name : "unnamed");
    }
    kernel_worker_count = 0;
}
