/*
 *
 *      kthread.h
 *      Kernel thread (kthreadd) API
 *
 *      2026/8/15 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_KTHREAD_H_
#define INCLUDE_KTHREAD_H_

#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <process/task.h>

/* Kernel-thread entry function (Linux `threadfn`): returns the exit code. */
typedef int (*kthread_entry_t)(void *arg);

/* Bootstrap record pushed onto a fresh kernel-thread stack. */
typedef struct {
        kthread_entry_t entry;
        void           *arg;
} kthread_bootstrap_t;

/* Creation request handed from kthread_create() to kthreadd. */
typedef struct {
        char            name[TASK_NAME_LEN];
        kthread_entry_t entry;
        void           *arg;
        uint32_t        cpu_id;
        bool            pinned;
        task_t         *result;
        int             error;
        wait_queue_t    done;
        bool            completed;
} kthread_create_info_t;

/* kthreadd's process bundle (PID 2).  Defined in kernel/process/process.c. */
extern process_t *kthreadd_process;

/*
 * kthreadd sleeps on this queue while it has no create requests or reaped
 * children to process.  process_exit() wakes it when a kthread child exits.
 */
extern wait_queue_t kthreadd_wait;

/* Bootstrap kthreadd (PID 2).  Must be called once, before any kthread_create. */
void kthreadd_init(void);

/* Create a kernel thread (returns a not-yet-runnable task). */
task_t *kthread_create(const char *name, kthread_entry_t entry, void *arg);

/* Create a kernel thread pinned to a specific CPU (not-yet-runnable). */
task_t *kthread_create_on_cpu(const char *name, kthread_entry_t entry, void *arg, uint32_t cpu_id);

/* Create a kernel thread and immediately make it runnable. */
task_t *kthread_run(const char *name, kthread_entry_t entry, void *arg);

/* Create a CPU-pinned kernel thread and immediately make it runnable. */
task_t *kthread_run_on_cpu(const char *name, kthread_entry_t entry, void *arg, uint32_t cpu_id);

/* Make a created (or blocked) task runnable. */
void wake_up_process(task_t *task);

/* Request a kernel thread to stop and wait for its exit code. */
int kthread_stop(task_t *task);

/* True if the current kernel thread has been asked to stop. */
bool kthread_should_stop(void);

/* Exit the current kernel thread with the given exit code.  Does not return. */
void kthread_exit(int exit_code);

/* Return the argument passed to the current kernel thread. */
void *kthread_data(void);

/*
 * A kernel worker registered by a subsystem during boot and created later by
 * kernel_workers_start() (called once from init/main.c) once kthreadd is live.
 */
typedef struct {
        const char     *name;
        kthread_entry_t entry;
        void           *arg;
        task_t        **slot; // store the created task here (may be NULL)
} kernel_worker_t;

/*
 * Register a kernel worker for creation.  Before kernel_workers_start() this
 * only queues the request; once the unified start has run (e.g. a late probe /
 * hot-plug), the worker is created immediately.  Returns 0, or a negative errno
 * on an immediate (late-registration) failure.
 */
int kernel_worker_register(const char *name, kthread_entry_t entry, void *arg, task_t **slot);

/* Create (kthread_run) every registered worker.  The single boot-time creation site. */
void kernel_workers_start(void);

#endif // INCLUDE_KTHREAD_H_
