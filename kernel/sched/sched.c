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
#include <apic.h>
#include <debug.h>
#include <heap.h>
#include <intrusive_list.h>
#include <page.h>
#include <printk.h>
#include <sched.h>
#include <smp.h>
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

typedef struct {
        task_t  *current;
        task_t  *idle;
        ilist_node_t ready_queue;
        uint64_t reschedule_ipis;
        uint8_t  online;
} cpu_scheduler_t;

static scheduler_t scheduler;
static task_t      boot_task;
static uint8_t     boot_stack_marker;
static cpu_scheduler_t *cpu_schedulers;
static uint32_t         cpu_scheduler_count;
static task_t           *ap_boot_tasks;
static uint32_t           next_task_cpu;

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

static task_t *local_current(void)
{ return cpu_schedulers[get_current_cpu_id()].current; }

static void task_name_copy(task_t *task, const char *name)
{
    const char *src = name ? name : "kthread";
    size_t      i   = 0;

    for (; i + 1 < TASK_NAME_LEN && src[i]; i++) task->name[i] = src[i];
    task->name[i] = '\0';
}

static void enqueue_task_on_cpu(task_t *task, uint32_t cpu_id)
{
    if (cpu_id >= cpu_scheduler_count) cpu_id = 0;
    task->state = TASK_READY;
    task->wake_tick = 0;
    task->wait_queue = NULL;
    task->cpu_id = cpu_id;
    ilist_insert_before(&cpu_schedulers[cpu_id].ready_queue, &task->run_node);
}

static void enqueue_task(task_t *task)
{ enqueue_task_on_cpu(task, task->cpu_id); }

static void wake_task_locked(task_t *task, int remove_linked_node)
{
    if (!task || task->state == TASK_READY || task->state == TASK_RUNNING || task->state == TASK_IDLE) return;

    if (remove_linked_node) ilist_remove(&task->run_node);
    task->time_slice = TASK_DEFAULT_SLICE;
    enqueue_task(task);
}

static void request_task_cpu(task_t *task)
{
    if (task && task->cpu_id != get_current_cpu_id() && scheduler.started)
        send_ipi_cpu(task->cpu_id, IPI_RESCHEDULE);
}

static void sleep_task(task_t *task, uint64_t wake_tick)
{
    task->state     = TASK_SLEEPING;
    task->wake_tick = wake_tick;
    task->wait_queue = NULL;
    ilist_insert_before(&scheduler.sleep_queue, &task->run_node);
}

static task_t *pick_next_task(uint32_t cpu_id)
{
    if (ilist_is_empty(&cpu_schedulers[cpu_id].ready_queue)) return cpu_schedulers[cpu_id].idle;

    ilist_node_t *node = cpu_schedulers[cpu_id].ready_queue.next;
    ilist_remove(node);
    return node_to_task(node);
}

static int has_ready_task(void)
{ return !ilist_is_empty(&cpu_schedulers[get_current_cpu_id()].ready_queue); }

static void wake_sleeping_tasks(void)
{
    ilist_node_t *node = scheduler.sleep_queue.next;

    while (node != &scheduler.sleep_queue) {
        ilist_node_t *next = node->next;
        task_t       *task = node_to_task(node);

        if (task->wake_tick <= scheduler.ticks) {
            ilist_remove(node);
            wake_task_locked(task, 0);
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
    task->cpu_id         = next_task_cpu++ % cpu_scheduler_count;
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
    ilist_init(&scheduler.sleep_queue);
    scheduler.next_pid = 1;

    cpu_scheduler_count = get_cpu_count();
    if (!cpu_scheduler_count) cpu_scheduler_count = 1;
    cpu_schedulers = calloc(cpu_scheduler_count, sizeof(cpu_scheduler_t));
    ap_boot_tasks  = calloc(cpu_scheduler_count, sizeof(task_t));
    if (!cpu_schedulers || !ap_boot_tasks) panic("sched: Cannot allocate per-CPU scheduler state.");
    for (uint32_t i = 0; i < cpu_scheduler_count; i++) {
        ilist_init(&cpu_schedulers[i].ready_queue);
        cpu_schedulers[i].online = 1;
        plogk("sched: CPU %u scheduler slot initialized.\n", i);
    }
    next_task_cpu = 0;
    cpu_schedulers[0].current = &boot_task;

    memset(&boot_task, 0, sizeof(boot_task));
    boot_task.pid            = 0;
    boot_task.state          = TASK_RUNNING;
    boot_task.page_directory = get_kernel_pagedir();
    boot_task.kernel_stack   = &boot_stack_marker;
    boot_task.time_slice     = TASK_DEFAULT_SLICE;
    task_name_copy(&boot_task, "kernel");
    ilist_init(&boot_task.run_node);

    scheduler.current = &boot_task;
    for (uint32_t i = 0; i < cpu_scheduler_count; i++) {
        cpu_schedulers[i].idle = kthread_create("idle", idle_thread, NULL);
        if (!cpu_schedulers[i].idle) panic("sched: Cannot create idle task.");
        spin_lock(&scheduler.lock);
        ilist_remove(&cpu_schedulers[i].idle->run_node);
        spin_unlock(&scheduler.lock);
        cpu_schedulers[i].idle->state = TASK_IDLE;
        cpu_schedulers[i].idle->cpu_id = i;
    }
    scheduler.idle = cpu_schedulers[0].idle;
    for (uint32_t i = 1; i < cpu_scheduler_count; i++) {
        memset(&ap_boot_tasks[i], 0, sizeof(task_t));
        ap_boot_tasks[i].pid            = 0;
        ap_boot_tasks[i].state          = TASK_BLOCKED;
        ap_boot_tasks[i].page_directory = get_kernel_pagedir();
        ap_boot_tasks[i].cpu_id         = i;
        ilist_init(&ap_boot_tasks[i].run_node);
        cpu_schedulers[i].current = &ap_boot_tasks[i];
    }

    plogk("sched: Initialized kernel scheduler.\n");
}

void sched_ap_online(uint32_t cpu_id)
{
    if (!cpu_schedulers || cpu_id >= cpu_scheduler_count) return;

    cpu_schedulers[cpu_id].online = 1;
    plogk("sched: CPU %u scheduler online.\n", cpu_id);
}

void sched_ap_start(uint32_t cpu_id)
{
    while (!cpu_schedulers || !cpu_scheduler_count) __asm__ volatile("pause");
    if (cpu_id == 0 || cpu_id >= cpu_scheduler_count) krn_halt();

    while (!scheduler.started) __asm__ volatile("pause");
    cpu_schedulers[cpu_id].current = &ap_boot_tasks[cpu_id];
    sched_yield();
    panic("sched: AP scheduler exited.");
}

void sched_ipi_reschedule(void)
{
    uint32_t cpu_id = get_current_cpu_id();

    if (!cpu_schedulers || cpu_id >= cpu_scheduler_count) return;
    cpu_schedulers[cpu_id].reschedule_ipis++;
    if (scheduler.started && has_ready_task()) sched_yield();
}

uint32_t sched_cpu_count(void)
{ return cpu_scheduler_count; }

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
    request_task_cpu(task);

    plogk("sched: Created task %llu (%s) on CPU %u.\n", task->pid, task->name, task->cpu_id);
    return task;
}

void sched_yield(void)
{
    spin_lock(&scheduler.lock);

    uint32_t cpu_id = get_current_cpu_id();
    task_t  *prev   = cpu_schedulers[cpu_id].current;
    task_t  *next   = pick_next_task(cpu_id);

    if (prev == next) {
        spin_unlock(&scheduler.lock);
        return;
    }

    if (prev->state == TASK_RUNNING && prev != cpu_schedulers[cpu_id].idle) {
        prev->time_slice = TASK_DEFAULT_SLICE;
        enqueue_task(prev);
    }
    if (next->state != TASK_IDLE) next->state = TASK_RUNNING;
    cpu_schedulers[cpu_id].current = next;
    if (cpu_id == 0) scheduler.current = next;

    spin_unlock(&scheduler.lock);
    context_switch(&prev->context, &next->context);
}

void sched_start(void)
{
    disable_intr();
    local_current()->state = TASK_BLOCKED;
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
    sleep_task(local_current(), scheduler.ticks + ticks);
    spin_unlock(&scheduler.lock);

    sched_yield();
    enable_intr();
}

void task_block(void)
{
    disable_intr();
    spin_lock(&scheduler.lock);
    local_current()->state = TASK_BLOCKED;
    spin_unlock(&scheduler.lock);

    sched_yield();
    enable_intr();
}

int task_wakeup(task_t *task)
{
    if (!task) return 1;

    wait_queue_t *queue = task->wait_queue;

    if (queue) {
        spin_lock(&queue->lock);
        spin_lock(&scheduler.lock);
        if (task->wait_queue == queue) wake_task_locked(task, 1);
        spin_unlock(&scheduler.lock);
        spin_unlock(&queue->lock);
        request_task_cpu(task);
        return 0;
    }

    spin_lock(&scheduler.lock);
    if (task->state == TASK_SLEEPING) {
        wake_task_locked(task, 1);
    } else {
        wake_task_locked(task, 0);
    }
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);
    return 0;
}

void wait_queue_init(wait_queue_t *queue)
{
    if (!queue) return;

    ilist_init(&queue->tasks);
    queue->lock.lock   = 0;
    queue->lock.rflags = 0;
}

void wait_queue_wait(wait_queue_t *queue)
{
    if (!queue) {
        task_block();
        return;
    }

    disable_intr();
    spin_lock(&queue->lock);
    spin_lock(&scheduler.lock);
    local_current()->state = TASK_BLOCKED;
    local_current()->wake_tick = 0;
    local_current()->wait_queue = queue;
    ilist_insert_before(&queue->tasks, &local_current()->run_node);
    spin_unlock(&scheduler.lock);
    spin_unlock(&queue->lock);

    sched_yield();
    enable_intr();
}

task_t *wait_queue_wake_one(wait_queue_t *queue)
{
    if (!queue) return NULL;

    spin_lock(&queue->lock);
    if (ilist_is_empty(&queue->tasks)) {
        spin_unlock(&queue->lock);
        return NULL;
    }

    ilist_node_t *node = queue->tasks.next;
    task_t       *task = node_to_task(node);

    ilist_remove(node);
    spin_lock(&scheduler.lock);
    wake_task_locked(task, 0);
    spin_unlock(&scheduler.lock);
    spin_unlock(&queue->lock);
    request_task_cpu(task);
    return task;
}

uint64_t wait_queue_wake_all(wait_queue_t *queue)
{
    if (!queue) return 0;

    uint64_t count = 0;
    task_t  *woken[16];
    size_t   woken_count = 0;

    spin_lock(&queue->lock);
    spin_lock(&scheduler.lock);
    while (!ilist_is_empty(&queue->tasks)) {
        ilist_node_t *node = queue->tasks.next;
        task_t       *task = node_to_task(node);

        ilist_remove(node);
        wake_task_locked(task, 0);
        if (woken_count < sizeof(woken) / sizeof(woken[0])) woken[woken_count++] = task;
        count++;
    }
    spin_unlock(&scheduler.lock);
    spin_unlock(&queue->lock);
    for (size_t i = 0; i < woken_count; i++) request_task_cpu(woken[i]);
    return count;
}

void sched_tick(void)
{
    if (!scheduler.started || !cpu_schedulers) return;

    uint32_t cpu_id = get_current_cpu_id();

    if (cpu_id == 0) {
        spin_lock(&scheduler.lock);
        scheduler.ticks++;
        wake_sleeping_tasks();
        spin_unlock(&scheduler.lock);
    }

    task_t   *current = cpu_schedulers[cpu_id].current;
    if (current == cpu_schedulers[cpu_id].idle) {
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
    local_current()->state = TASK_ZOMBIE;
    spin_unlock(&scheduler.lock);

    sched_yield();
    panic("sched: zombie task resumed.");
}

task_t *current_task(void)
{ return cpu_schedulers[get_current_cpu_id()].current; }
