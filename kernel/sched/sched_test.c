/*
 *
 *      sched_test.c
 *      Scheduler debug test threads
 *
 *      2026/7/19 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/input/ps2/ps2.h>
#include <kernel/printk.h>
#include <process/sched.h>
#include <process/sched_test.h>

#if SCHED_DEBUG_DEMO
static volatile uint64_t preempt_demo_sink;
static wait_queue_t      demo_wait_queue;
static wait_queue_t      migration_wait_queue;
static wait_queue_t      wake_all_wait_queue;
static task_t           *migration_task;
static volatile uint32_t wake_all_ready;
static volatile uint32_t wake_all_woken;

#    define WAKE_ALL_TEST_WAITERS 24

/* Demo thread that logs a few sleep-ticks iterations */
static void scheduler_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    for (uint64_t i = 0; i < 8; i++) {
        plogk("sched: %s iteration %llu on task %llu cpu %u\n", name, i, current_task()->pid, current_task()->cpu_id);
        task_sleep_ticks(2);
    }
}

/* Demo thread that burns CPU in chunks to exercise preemption */
static void preempt_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    plogk("sched: %s busy loop start on task %llu cpu %u\n", name, current_task()->pid, current_task()->cpu_id);
    for (uint64_t chunk = 0; chunk < 3; chunk++) {
        for (uint64_t i = 0; i < 5000000; i++) preempt_demo_sink += i;
        plogk("sched: %s busy chunk %llu cpu %u\n", name, chunk, current_task()->cpu_id);
    }
    plogk("sched: %s busy loop done.\n", name);
}

/* Demo thread that blocks on the shared wait queue */
static void wait_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    plogk("sched: %s waiting at tick %llu\n", name, sched_ticks());
    wait_queue_wait(&demo_wait_queue);
    plogk("sched: %s woke at tick %llu on task %llu cpu %u\n", name, sched_ticks(), current_task()->pid, current_task()->cpu_id);
}

/* Demo thread that wakes one task off the shared wait queue */
static void wake_demo_thread(void *arg)
{
    (void)arg;

    task_sleep_ticks(8);
    task_t *task = wait_queue_wake_one(&demo_wait_queue);
    plogk("sched: Wait queue wake_one target task %llu\n", task ? task->pid : 0);
}

/* Demo thread that blocks waiting for a keyboard input event */
static void keyboard_wait_thread(void *arg)
{
    (void)arg;

    plogk("init: Keyboard waiter blocking for an input event.\n");
    ps2kbd_wait_events();
    plogk("init: Keyboard waiter received an input event.\n");
}

/* Demo thread that blocks on the migration wait queue */
static void migration_wait_thread(void *arg)
{
    (void)arg;

    plogk("sched: Migration waiter started on task %llu cpu %u\n", current_task()->pid, current_task()->cpu_id);
    wait_queue_wait(&migration_wait_queue);
    plogk("sched: Migration waiter woke on task %llu cpu %u\n", current_task()->pid, current_task()->cpu_id);
}

/* Demo thread that migrates the waiter to CPU 1 and wakes it */
static void migration_wake_thread(void *arg)
{
    (void)arg;

    task_sleep_ticks(12);
    if (migration_task && sched_cpu_count() > 1) {
        int status = task_set_cpu(migration_task, 1);
        plogk("sched: Migration target task %llu to cpu 1 status %d\n", migration_task->pid, status);
    }
    task_t *task = wait_queue_wake_one(&migration_wait_queue);
    plogk("sched: Migration wake target task %llu\n", task ? task->pid : 0);
}

/* One waiter among many that prepares on the shared wake-all queue */
static void wake_all_wait_thread(void *arg)
{
    (void)arg;

    wait_queue_prepare(&wake_all_wait_queue);
    __atomic_add_fetch(&wake_all_ready, 1, __ATOMIC_RELEASE);
    wait_queue_sleep();
    __atomic_add_fetch(&wake_all_woken, 1, __ATOMIC_RELEASE);
}

/* Demo thread that wakes all waiters and reports pass/fail */
static void wake_all_test_thread(void *arg)
{
    (void)arg;

    while (__atomic_load_n(&wake_all_ready, __ATOMIC_ACQUIRE) < WAKE_ALL_TEST_WAITERS) sched_yield();

    uint64_t count = wait_queue_wake_all(&wake_all_wait_queue);

    while (__atomic_load_n(&wake_all_woken, __ATOMIC_ACQUIRE) < WAKE_ALL_TEST_WAITERS) sched_yield();
    plogk("sched: wake_all test %s (%llu/%u waiters)\n", count == WAKE_ALL_TEST_WAITERS ? "passed" : "failed", count, WAKE_ALL_TEST_WAITERS);
}

/* Demo thread that burns CPU and yields to exercise load balancing */
static void balance_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    for (uint64_t chunk = 0; chunk < 4; chunk++) {
        for (uint64_t i = 0; i < 2500000; i++) preempt_demo_sink += i;
        plogk("sched: %s balance chunk %llu task %llu cpu %u\n", name, chunk, current_task()->pid, current_task()->cpu_id);
        sched_yield();
    }
}

/* Spawn the scheduler demo threads and keep the init thread alive */
static void kernel_init_thread(void *arg)
{
    (void)arg;

    plogk("init: Kernel init thread started as task %llu cpu %u\n", current_task()->pid, current_task()->cpu_id);
    wait_queue_init(&demo_wait_queue);
    wait_queue_init(&migration_wait_queue);
    wait_queue_init(&wake_all_wait_queue);
    kthread_create("preempt-demo", preempt_demo_thread, "preempt-demo");
    kthread_create("demo-a", scheduler_demo_thread, "demo-a");
    kthread_create("demo-b", scheduler_demo_thread, "demo-b");
    kthread_create("wait-demo", wait_demo_thread, "wait-demo");
    kthread_create("wake-demo", wake_demo_thread, NULL);
    kthread_create("keyboard-wait", keyboard_wait_thread, NULL);
    migration_task = kthread_create_on_cpu("migration-wait", migration_wait_thread, NULL, 0);
    kthread_create("migration-wake", migration_wake_thread, NULL);
    for (uint32_t i = 0; i < WAKE_ALL_TEST_WAITERS; i++) kthread_create("wake-all-wait", wake_all_wait_thread, NULL);
    kthread_create("wake-all-test", wake_all_test_thread, NULL);
    if (sched_cpu_count() > 1) {
        kthread_create_on_cpu("balance-a", balance_demo_thread, "balance-a", 0);
        kthread_create_on_cpu("balance-b", balance_demo_thread, "balance-b", 0);
        kthread_create_on_cpu("balance-c", balance_demo_thread, "balance-c", 0);
        kthread_create_on_cpu("balance-d", balance_demo_thread, "balance-d", 0);
    }

    while (1) task_sleep_ticks(250);
}
#endif

/* Start the scheduler debug demos when SCHED_DEBUG_DEMO is enabled */
void sched_test_init(void)
{
#if SCHED_DEBUG_DEMO
    kthread_create("kernel-init", kernel_init_thread, NULL);
#endif
}
