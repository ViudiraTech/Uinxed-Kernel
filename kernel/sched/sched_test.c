/*
 *
 *      sched_test.c
 *      Scheduler debug test threads
 *
 */

#include <printk.h>
#include <ps2.h>
#include <sched.h>
#include <sched_test.h>

#if SCHED_DEBUG_DEMO
static volatile uint64_t preempt_demo_sink;
static wait_queue_t      demo_wait_queue;
static wait_queue_t      migration_wait_queue;
static task_t           *migration_task;

static void scheduler_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    for (uint64_t i = 0; i < 8; i++) {
        plogk("sched: %s iteration %llu on task %llu cpu %u\n", name, i, current_task()->pid, current_task()->cpu_id);
        task_sleep_ticks(2);
    }
}

static void preempt_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    plogk("sched: %s busy loop start on task %llu cpu %u\n", name, current_task()->pid, current_task()->cpu_id);
    for (uint64_t chunk = 0; chunk < 3; chunk++) {
        for (uint64_t i = 0; i < 5000000; i++) preempt_demo_sink += i;
        plogk("sched: %s busy chunk %llu cpu %u\n", name, chunk, current_task()->cpu_id);
    }
    plogk("sched: %s busy loop done\n", name);
}

static void wait_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    plogk("sched: %s waiting at tick %llu\n", name, sched_ticks());
    wait_queue_wait(&demo_wait_queue);
    plogk("sched: %s woke at tick %llu on task %llu cpu %u\n", name, sched_ticks(), current_task()->pid, current_task()->cpu_id);
}

static void wake_demo_thread(void *arg)
{
    (void)arg;

    task_sleep_ticks(8);
    task_t *task = wait_queue_wake_one(&demo_wait_queue);
    plogk("sched: wait queue wake_one target task %llu\n", task ? task->pid : 0);
}

static void keyboard_wait_thread(void *arg)
{
    (void)arg;

    plogk("init: Keyboard waiter blocking for an input event.\n");
    ps2kbd_wait_events();
    plogk("init: Keyboard waiter received an input event.\n");
}

static void migration_wait_thread(void *arg)
{
    (void)arg;

    plogk("sched: migration waiter started on task %llu cpu %u\n", current_task()->pid, current_task()->cpu_id);
    wait_queue_wait(&migration_wait_queue);
    plogk("sched: migration waiter woke on task %llu cpu %u\n", current_task()->pid, current_task()->cpu_id);
}

static void migration_wake_thread(void *arg)
{
    (void)arg;

    task_sleep_ticks(12);
    if (migration_task && sched_cpu_count() > 1) {
        int status = task_set_cpu(migration_task, 1);
        plogk("sched: migration target task %llu to cpu 1 status %d\n", migration_task->pid, status);
    }
    task_t *task = wait_queue_wake_one(&migration_wait_queue);
    plogk("sched: migration wake target task %llu\n", task ? task->pid : 0);
}

static void balance_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    for (uint64_t chunk = 0; chunk < 4; chunk++) {
        for (uint64_t i = 0; i < 2500000; i++) preempt_demo_sink += i;
        plogk("sched: %s balance chunk %llu task %llu cpu %u\n", name, chunk, current_task()->pid, current_task()->cpu_id);
        sched_yield();
    }
}

static void kernel_init_thread(void *arg)
{
    (void)arg;

    plogk("init: Kernel init thread started as task %llu cpu %u\n", current_task()->pid, current_task()->cpu_id);
    wait_queue_init(&demo_wait_queue);
    wait_queue_init(&migration_wait_queue);
    kthread_create("preempt-demo", preempt_demo_thread, "preempt-demo");
    kthread_create("demo-a", scheduler_demo_thread, "demo-a");
    kthread_create("demo-b", scheduler_demo_thread, "demo-b");
    kthread_create("wait-demo", wait_demo_thread, "wait-demo");
    kthread_create("wake-demo", wake_demo_thread, NULL);
    kthread_create("keyboard-wait", keyboard_wait_thread, NULL);
    migration_task = kthread_create_on_cpu("migration-wait", migration_wait_thread, NULL, 0);
    kthread_create("migration-wake", migration_wake_thread, NULL);
    if (sched_cpu_count() > 1) {
        kthread_create_on_cpu("balance-a", balance_demo_thread, "balance-a", 0);
        kthread_create_on_cpu("balance-b", balance_demo_thread, "balance-b", 0);
        kthread_create_on_cpu("balance-c", balance_demo_thread, "balance-c", 0);
        kthread_create_on_cpu("balance-d", balance_demo_thread, "balance-d", 0);
    }

    while (1) task_sleep_ticks(250);
}
#endif

void sched_test_init(void)
{
#if SCHED_DEBUG_DEMO
    kthread_create("kernel-init", kernel_init_thread, NULL);
#endif
}
