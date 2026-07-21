/*
 *
 *      sched.c
 *      Kernel EEVDF scheduler
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <apic.h>
#include <common.h>
#include <debug.h>
#include <gdt.h>
#include <heap.h>
#include <intrusive_list.h>
#include <page.h>
#include <printk.h>
#include <process.h>
#include <rbtree.h>
#include <sched.h>
#include <smp.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <task.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define SCHED_LOAD_BALANCE_INTERVAL 16
#define SCHED_BASE_SLICE            4ULL /* Linux sysctl_sched_base_slice ≈ 3ms */
#define SCHED_LATENCY               8ULL /* target scheduling latency (ticks)    */
#define SCHED_MIN_GRANULARITY       1ULL /* minimum preemption granularity       */
#define SCHED_WAKEUP_GRANULARITY    1ULL /* wakeup preemption threshold          */

/* ------------------------------------------------------------------ */
/*  Global state                                                        */
/* ------------------------------------------------------------------ */

scheduler_t     scheduler;
eevdf_rq_t     *cpu_rqs;
uint32_t        cpu_scheduler_count;
static task_t   boot_task;
static uint8_t  boot_stack_marker;
static task_t  *ap_boot_tasks;
static uint32_t next_task_cpu;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                                */
/* ------------------------------------------------------------------ */

void context_switch(task_context_t *prev, task_context_t *next);

/* ------------------------------------------------------------------ */
/*  Context switch (naked assembly)                                     */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Helpers: container_of variants                                      */
/* ------------------------------------------------------------------ */

static task_t *sched_node_to_task(ilist_node_t *node)
{
    return (task_t *)((uint8_t *)node - offsetof(task_t, sched_node));
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: virtual-time arithmetic                                 */
/* ------------------------------------------------------------------ */

/* Convert wall-clock delta to virtual-time delta for a given weight */
static uint64_t calc_delta_fair(uint64_t delta, task_t *task)
{
    if (task->weight == SCHED_NICE_0_LOAD) return delta;
    return delta * SCHED_NICE_0_LOAD / task->weight;
}

/* Compute the weighted-average vruntime of a runqueue */
static uint64_t avg_vruntime(eevdf_rq_t *rq)
{
    if (!rq->avg_load) return rq->min_vruntime;
    return rq->min_vruntime + (uint64_t)(rq->avg_vruntime / (int64_t)rq->avg_load);
}

/* Scale the base slice by the number of runnable tasks to stay within
 * the scheduling latency period.  Ensures each task gets at least
 * min_granularity. */
static uint64_t calc_effective_slice(eevdf_rq_t *rq)
{
    uint64_t nr = rq->nr_running;

    if (rq->curr && rq->curr->state == TASK_RUNNING && rq->curr != rq->idle) nr++;
    if (nr <= 1) return SCHED_BASE_SLICE;

    uint64_t slice = SCHED_LATENCY / nr;
    if (slice < SCHED_MIN_GRANULARITY) slice = SCHED_MIN_GRANULARITY;
    if (slice > SCHED_BASE_SLICE) slice = SCHED_BASE_SLICE;
    return slice;
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: eligibility check                                       */
/*                                                                      */
/*  A task is eligible iff lag >= 0, i.e. its ideal service time       */
/*  exceeds its actual service time.  In discrete form:                 */
/*    avg_vruntime >= (vruntime - min_vruntime) * avg_load              */
/* ------------------------------------------------------------------ */

static int entity_eligible_vruntime(eevdf_rq_t *rq, uint64_t vruntime)
{
    int64_t  avg  = rq->avg_vruntime;
    uint64_t load = rq->avg_load;

    if (rq->curr && rq->curr->state == TASK_RUNNING) {
        uint64_t weight = rq->curr->weight;
        avg += (int64_t)(rq->curr->vruntime - rq->min_vruntime) * (int64_t)weight;
        load += weight;
    }

    return avg >= (int64_t)(vruntime - rq->min_vruntime) * (int64_t)load;
}

static int entity_eligible(eevdf_rq_t *rq, task_t *task)
{
    return entity_eligible_vruntime(rq, task->vruntime);
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: RB-tree comparison and augmentation                     */
/* ------------------------------------------------------------------ */

/* Compare two rb_nodes by their task's deadline (tiebreak: vruntime) */
static int entity_less(const rb_node_t *a, const rb_node_t *b)
{
    const task_t *ta = rb_entry(a, task_t, run_node);
    const task_t *tb = rb_entry(b, task_t, run_node);

    if (ta->deadline != tb->deadline) return (int64_t)(ta->deadline - tb->deadline) < 0;
    return (int64_t)(ta->vruntime - tb->vruntime) < 0;
}

/* Check whether candidate is "significantly" better than curr.
 * Uses wakeup_granularity to prevent preemption ping-pong. */
static int entity_before(task_t *cand, task_t *curr)
{
    if (entity_less(&cand->run_node, &curr->run_node)) {
        uint64_t gran = calc_delta_fair(SCHED_WAKEUP_GRANULARITY, cand);
        if ((int64_t)(cand->deadline + gran) < (int64_t)curr->deadline) return 1;
    }
    return 0;
}

/* Augmentation callback: recompute min_vruntime for the subtree */
static void update_min_vruntime(rb_node_t *node, void *data)
{
    (void)data;

    task_t  *task   = rb_entry(node, task_t, run_node);
    uint64_t min_vr = task->vruntime;

    if (node->left && node->left->min_vruntime < min_vr) min_vr = node->left->min_vruntime;
    if (node->right && node->right->min_vruntime < min_vr) min_vr = node->right->min_vruntime;

    node->min_vruntime = min_vr;
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: avg_vruntime / avg_load bookkeeping                     */
/* ------------------------------------------------------------------ */

static void avg_vruntime_add(eevdf_rq_t *rq, task_t *task)
{
    int64_t delta = (int64_t)(task->vruntime - rq->min_vruntime) * (int64_t)task->weight;

    rq->avg_vruntime += delta;
    rq->avg_load += task->weight;
}

static void avg_vruntime_sub(eevdf_rq_t *rq, task_t *task)
{
    int64_t delta = (int64_t)(task->vruntime - rq->min_vruntime) * (int64_t)task->weight;

    rq->avg_vruntime -= delta;
    rq->avg_load -= task->weight;
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: update_curr — advance vruntime of the running task      */
/* ------------------------------------------------------------------ */

static void update_curr(eevdf_rq_t *rq, uint64_t delta_ticks)
{
    task_t *curr = rq->curr;

    if (!curr || curr == rq->idle) return;

    curr->vruntime += calc_delta_fair(delta_ticks, curr);

    /* Update min_vruntime */
    if ((int64_t)(curr->vruntime - rq->min_vruntime) > 0) rq->min_vruntime = curr->vruntime;
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: update_deadline — assign a new deadline slice           */
/* ------------------------------------------------------------------ */

static void update_deadline(eevdf_rq_t *rq, task_t *task)
{
    /* Only update if the task has consumed its current slice */
    if ((int64_t)(task->vruntime - task->deadline) < 0) return;

    uint64_t slice = calc_effective_slice(rq);

    task->deadline = task->vruntime + calc_delta_fair(slice, task);
    task->vlag     = (int64_t)(avg_vruntime(rq) - task->vruntime);
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: place_entity — set vruntime/deadline on enqueue         */
/* ------------------------------------------------------------------ */

static void place_entity(eevdf_rq_t *rq, task_t *task, int initial)
{
    uint64_t vruntime = avg_vruntime(rq);
    int64_t  lag      = 0;

    uint64_t slice  = calc_effective_slice(rq);
    uint64_t vslice = calc_delta_fair(slice, task);

    /* PLACE_LAG: adjust vruntime based on stored vlag.
	 * Scale the stored lag to account for the changed load. */
    if (rq->nr_running > 0) {
        uint64_t load = rq->avg_load;
        uint64_t new_load;

        if (rq->curr && rq->curr->state == TASK_RUNNING && rq->curr != rq->idle) load += rq->curr->weight;
        new_load = load + task->weight;

        lag = task->vlag;
        if (load && new_load > load) lag = lag * (int64_t)new_load / (int64_t)load;
    }

    task->vruntime = vruntime - (uint64_t)lag;

    /* PLACE_DEADLINE_INITIAL: new tasks start with half a slice */
    if (initial) vslice /= 2;

    task->deadline = task->vruntime + vslice;
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: enqueue_entity / dequeue_entity                         */
/* ------------------------------------------------------------------ */

static void enqueue_entity(eevdf_rq_t *rq, task_t *task)
{
    avg_vruntime_add(rq, task);
    rb_insert_augmented(&rq->timeline, &task->run_node, entity_less, update_min_vruntime, NULL);
    rq->nr_running++;
}

static void dequeue_entity(eevdf_rq_t *rq, task_t *task)
{
    rb_erase_augmented(&rq->timeline, &task->run_node, update_min_vruntime, NULL);
    avg_vruntime_sub(rq, task);
    if (rq->nr_running) rq->nr_running--;
}

/* ------------------------------------------------------------------ */
/*  EEVDF core: pick_eevdf — select the next task to run                */
/*                                                                      */
/*  Strategy:                                                           */
/*    1. If only one task is runnable, return it directly.              */
/*    2. Check the cached leftmost (earliest deadline).                 */
/*       If eligible, it wins.                                          */
/*    3. Otherwise, traverse the rbtree, using the min_vruntime         */
/*       augmentation to skip subtrees that contain no eligible         */
/*       entities.                                                      */
/* ------------------------------------------------------------------ */

static task_t *pick_eevdf(eevdf_rq_t *rq)
{
    task_t    *curr = rq->curr;
    rb_node_t *node;

    /* Fast path: empty runqueue */
    if (rq->nr_running == 0) return rq->idle;

    /* Fast path: only one runnable entity */
    if (rq->nr_running == 1) {
        if (curr && curr->state == TASK_RUNNING && curr != rq->idle) return curr;
        if (rq->timeline.leftmost) return rb_entry(rq->timeline.leftmost, task_t, run_node);
        return rq->idle;
    }

    /* Current task is only kept if it is still eligible */
    if (curr && (curr->state != TASK_RUNNING || !entity_eligible(rq, curr))) curr = NULL;

    /* Check the leftmost (earliest deadline) */
    if (rq->timeline.leftmost) {
        task_t *leftmost = rb_entry(rq->timeline.leftmost, task_t, run_node);

        if (entity_eligible(rq, leftmost)) {
            if (!curr || entity_before(leftmost, curr)) return leftmost;
            return curr;
        }
    }

    /* Heap-search: traverse the tree, pruning ineligible subtrees */
    node = rq->timeline.root;
    while (node) {
        rb_node_t *left = node->left;

        /* If the left subtree has an eligible entity, go left */
        if (left && entity_eligible_vruntime(rq, left->min_vruntime)) {
            node = left;
            continue;
        }

        /* Check the current node */
        task_t *se = rb_entry(node, task_t, run_node);
        if (entity_eligible(rq, se)) {
            if (!curr || entity_before(se, curr)) return se;
            return curr;
        }

        /* Neither left nor current is eligible — go right */
        node = node->right;
    }

    /* No eligible entity found; keep current if it is still runnable */
    if (curr && curr->state == TASK_RUNNING) return curr;
    return rq->idle;
}

/* ------------------------------------------------------------------ */
/*  Per-CPU helpers                                                     */
/* ------------------------------------------------------------------ */

static eevdf_rq_t *local_rq(void)
{
    return &cpu_rqs[get_current_cpu_id()];
}

static task_t *local_current(void)
{
    return local_rq()->curr;
}

static void update_tss_stack(task_t *task)
{
    if (!task) return;

    if (task->process && task->process->kernel_stack) {
        set_kernel_stack((uint64_t)(task->process->kernel_stack + PROCESS_KERNEL_STACK));
    } else if (task->kernel_stack) {
        set_kernel_stack((uint64_t)(task->kernel_stack + TASK_KERNEL_STACK));
    }
}

static void enqueue_task_on_cpu(task_t *task, uint32_t cpu_id, int initial)
{
    if (cpu_id >= cpu_scheduler_count) cpu_id = 0;

    eevdf_rq_t *rq = &cpu_rqs[cpu_id];

    place_entity(rq, task, initial);
    task->state      = TASK_READY;
    task->wake_tick  = 0;
    task->wait_queue = NULL;
    task->cpu_id     = cpu_id;

    enqueue_entity(rq, task);
}

/* ------------------------------------------------------------------ */
/*  Public API: enqueue_task                                            */
/* ------------------------------------------------------------------ */

void enqueue_task(task_t *task)
{
    enqueue_task_on_cpu(task, task->cpu_id, 0);
}

void enqueue_task_initial(task_t *task)
{
    enqueue_task_on_cpu(task, task->cpu_id, 1);
}

/* ------------------------------------------------------------------ */
/*  Wake / sleep helpers                                                */
/* ------------------------------------------------------------------ */

static void wake_task_locked(task_t *task, int remove_linked_node)
{
    if (!task || task->state == TASK_READY || task->state == TASK_RUNNING || task->state == TASK_IDLE) return;

    if (remove_linked_node) ilist_remove(&task->sched_node);
    enqueue_task(task);
}

void request_task_cpu(task_t *task)
{
    if (task && task->cpu_id != get_current_cpu_id() && scheduler.started) send_ipi_cpu(task->cpu_id, IPI_RESCHEDULE);
}

static void sleep_task(task_t *task, uint64_t wake_tick)
{
    task->state      = TASK_SLEEPING;
    task->wake_tick  = wake_tick;
    task->wait_queue = NULL;
    ilist_insert_before(&scheduler.sleep_queue, &task->sched_node);
}

/* ------------------------------------------------------------------ */
/*  Load balancing                                                      */
/* ------------------------------------------------------------------ */

uint32_t choose_task_cpu_locked(void)
{
    uint32_t best = next_task_cpu++ % cpu_scheduler_count;

    for (uint32_t i = 0; i < cpu_scheduler_count; i++) {
        if (cpu_rqs[i].nr_running < cpu_rqs[best].nr_running) best = i;
    }
    return best;
}

static int has_ready_task(void)
{
    return local_rq()->nr_running > 0;
}

static task_t *balance_ready_queues_locked(void)
{
    if (cpu_scheduler_count < 2) return NULL;

    uint32_t busiest = 0;
    uint32_t idlest  = 0;

    for (uint32_t i = 1; i < cpu_scheduler_count; i++) {
        if (cpu_rqs[i].nr_running > cpu_rqs[busiest].nr_running) busiest = i;
        if (cpu_rqs[i].nr_running < cpu_rqs[idlest].nr_running) idlest = i;
    }

    if (busiest == idlest || cpu_rqs[busiest].nr_running <= cpu_rqs[idlest].nr_running + 1) return NULL;

    /* Steal the task with the largest deadline (least urgent) from busiest */
    rb_node_t *node = cpu_rqs[busiest].timeline.root;
    if (!node) return NULL;

    /* Find the rightmost node (largest deadline) */
    while (node->right) node = node->right;

    task_t *task = rb_entry(node, task_t, run_node);

    dequeue_entity(&cpu_rqs[busiest], task);
    enqueue_task_on_cpu(task, idlest, 0);
    return task;
}

static void wake_sleeping_tasks(void)
{
    ilist_node_t *node = scheduler.sleep_queue.next;

    while (node != &scheduler.sleep_queue) {
        ilist_node_t *next = node->next;
        task_t       *task = sched_node_to_task(node);

        if (task->wake_tick <= scheduler.ticks) {
            ilist_remove(node);
            wake_task_locked(task, 0);
        }
        node = next;
    }
}

/* ------------------------------------------------------------------ */
/*  Idle thread                                                         */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  sched_init — bootstrap the scheduler                                */
/* ------------------------------------------------------------------ */

void sched_init(void)
{
    memset(&scheduler, 0, sizeof(scheduler));
    ilist_init(&scheduler.sleep_queue);
    scheduler.next_pid = 0;

    cpu_scheduler_count = get_cpu_count();
    if (!cpu_scheduler_count) cpu_scheduler_count = 1;
    cpu_rqs       = calloc(cpu_scheduler_count, sizeof(eevdf_rq_t));
    ap_boot_tasks = calloc(cpu_scheduler_count, sizeof(task_t));
    if (!cpu_rqs || !ap_boot_tasks) panic("sched: Cannot allocate per-CPU scheduler state.");

    for (uint32_t i = 0; i < cpu_scheduler_count; i++) {
        rb_init_root(&cpu_rqs[i].timeline);
        cpu_rqs[i].online = 1;
        plogk("sched: CPU %u scheduler slot initialized.\n", i);
    }

    next_task_cpu         = 0;
    cpu_rqs[0].curr       = &boot_task;
    cpu_rqs[0].nr_running = 0;

    memset(&boot_task, 0, sizeof(boot_task));
    boot_task.pid            = 0;
    boot_task.state          = TASK_RUNNING;
    boot_task.page_directory = get_kernel_pagedir();
    boot_task.kernel_stack   = &boot_stack_marker;
    boot_task.weight         = SCHED_NICE_0_LOAD;
    boot_task.process        = NULL;
    task_name_copy(&boot_task, "kernel");
    ilist_init(&boot_task.sched_node);

    for (uint32_t i = 0; i < cpu_scheduler_count; i++) {
        cpu_rqs[i].idle = task_alloc("swapper");
        snprintf(cpu_rqs[i].idle->name, sizeof(cpu_rqs[i].idle->name), "swapper/%u", i);
        if (!cpu_rqs[i].idle) panic("sched: Cannot create idle task.");
        cpu_rqs[i].idle->pid          = 0;
        cpu_rqs[i].idle->state        = TASK_IDLE;
        cpu_rqs[i].idle->cpu_id       = i;
        cpu_rqs[i].idle->weight       = SCHED_NICE_0_LOAD;
        cpu_rqs[i].idle->kernel_stack = malloc(TASK_KERNEL_STACK);
        if (!cpu_rqs[i].idle->kernel_stack) panic("sched: Cannot allocate idle kernel stack.");
        uint64_t *stack                 = (uint64_t *)ALIGN_DOWN((uint64_t)(cpu_rqs[i].idle->kernel_stack + TASK_KERNEL_STACK), 16ULL);
        *(--stack)                      = (uint64_t)idle_thread;
        cpu_rqs[i].idle->context.rsp    = (uint64_t)stack;
        cpu_rqs[i].idle->context.rdi    = (uint64_t)NULL;
        cpu_rqs[i].idle->context.rflags = 0x202;
        plogk("task: Created task %llu (%s) on CPU %u.\n", cpu_rqs[i].idle->pid, cpu_rqs[i].idle->name, cpu_rqs[i].idle->cpu_id);
    }

    scheduler.next_pid = 1;
    for (uint32_t i = 1; i < cpu_scheduler_count; i++) {
        memset(&ap_boot_tasks[i], 0, sizeof(task_t));
        ap_boot_tasks[i].pid            = 0;
        ap_boot_tasks[i].state          = TASK_BLOCKED;
        ap_boot_tasks[i].page_directory = get_kernel_pagedir();
        ap_boot_tasks[i].cpu_id         = i;
        ap_boot_tasks[i].weight         = SCHED_NICE_0_LOAD;
        ilist_init(&ap_boot_tasks[i].sched_node);
        cpu_rqs[i].curr = &ap_boot_tasks[i];
    }

    plogk("sched: Initialized EEVDF kernel scheduler.\n");
}

/* ------------------------------------------------------------------ */
/*  AP management                                                       */
/* ------------------------------------------------------------------ */

void sched_ap_online(uint32_t cpu_id)
{
    if (!cpu_rqs || cpu_id >= cpu_scheduler_count) return;

    cpu_rqs[cpu_id].online = 1;
    plogk("sched: CPU %u scheduler online.\n", cpu_id);
}

void sched_ap_start(uint32_t cpu_id)
{
    while (!cpu_rqs || !cpu_scheduler_count) __asm__ volatile("pause");
    if (cpu_id == 0 || cpu_id >= cpu_scheduler_count) krn_halt();

    while (!scheduler.started) __asm__ volatile("pause");
    cpu_rqs[cpu_id].curr = &ap_boot_tasks[cpu_id];
    sched_yield();
    panic("sched: AP scheduler exited.");
}

void sched_ipi_reschedule(void)
{
    uint32_t cpu_id = get_current_cpu_id();

    if (!cpu_rqs || cpu_id >= cpu_scheduler_count) return;
    cpu_rqs[cpu_id].reschedule_ipis++;
    if (scheduler.started && has_ready_task()) sched_yield();
}

uint32_t sched_cpu_count(void)
{
    return cpu_scheduler_count;
}

/* ------------------------------------------------------------------ */
/*  task_set_cpu — migrate a task to a different CPU                    */
/* ------------------------------------------------------------------ */

int task_set_cpu(task_t *task, uint32_t cpu_id)
{
    if (!task || cpu_id >= cpu_scheduler_count) return 1;

    spin_lock(&scheduler.lock);
    if (task->state == TASK_RUNNING || task->state == TASK_IDLE || task->state == TASK_ZOMBIE) {
        spin_unlock(&scheduler.lock);
        return 1;
    }

    if (task->state == TASK_READY) {
        dequeue_entity(&cpu_rqs[task->cpu_id], task);
        enqueue_task_on_cpu(task, cpu_id, 0);
    } else {
        task->cpu_id = cpu_id;
    }
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sched_yield — core context switch entry point                       */
/* ------------------------------------------------------------------ */

void sched_yield(void)
{
    spin_lock(&scheduler.lock);

    eevdf_rq_t *rq   = local_rq();
    task_t     *prev = rq->curr;
    task_t     *next;

    /* Advance vruntime and re-enqueue the current task if it was running */
    if (prev && prev->state == TASK_RUNNING && prev != rq->idle) {
        update_curr(rq, 1);
        update_deadline(rq, prev);
        prev->vlag  = (int64_t)(avg_vruntime(rq) - prev->vruntime);
        prev->state = TASK_READY;
        enqueue_entity(rq, prev);
    }

    next = pick_eevdf(rq);

    if (prev == next) {
        spin_unlock(&scheduler.lock);
        return;
    }

    /* Advance min_vruntime when going idle so that tasks waking up
	 * later don't get a huge vruntime windfall. */
    if (next == rq->idle && rq->nr_running == 0) {
        uint64_t avg = avg_vruntime(rq);
        if ((int64_t)(avg - rq->min_vruntime) > 0) rq->min_vruntime = avg;
    }

    /* Dequeue the selected task from the timeline */
    if (next != rq->idle && next->state == TASK_READY) {
        dequeue_entity(rq, next);
        next->state      = TASK_RUNNING;
        next->time_slice = 0;
    }

    rq->curr = next;
    update_tss_stack(next);
    switch_page_directory(next->page_directory);

    spin_unlock(&scheduler.lock);
    context_switch(&prev->context, &next->context);
}

/* ------------------------------------------------------------------ */
/*  sched_start — launch the scheduler on the BSP                       */
/* ------------------------------------------------------------------ */

void sched_start(void)
{
    disable_intr();
    local_current()->state = TASK_BLOCKED;
    scheduler.started      = 1;
    enable_intr();
    sched_yield();
    panic("sched: bootstrap task resumed.");
}

/* ------------------------------------------------------------------ */
/*  task_sleep_ticks — voluntary sleep for N ticks                      */
/* ------------------------------------------------------------------ */

void task_sleep_ticks(uint64_t ticks)
{
    if (!ticks) {
        sched_yield();
        return;
    }

    disable_intr();
    spin_lock(&scheduler.lock);

    eevdf_rq_t *rq   = local_rq();
    task_t     *curr = rq->curr;

    /* Save lag before sleeping */
    curr->vlag = (int64_t)(avg_vruntime(rq) - curr->vruntime);
    sleep_task(curr, scheduler.ticks + ticks);

    spin_unlock(&scheduler.lock);
    sched_yield();
    enable_intr();
}

/* ------------------------------------------------------------------ */
/*  task_block — block the current task                                 */
/* ------------------------------------------------------------------ */

void task_block(void)
{
    disable_intr();
    spin_lock(&scheduler.lock);

    eevdf_rq_t *rq   = local_rq();
    task_t     *curr = rq->curr;

    curr->vlag  = (int64_t)(avg_vruntime(rq) - curr->vruntime);
    curr->state = TASK_BLOCKED;

    spin_unlock(&scheduler.lock);
    sched_yield();
    enable_intr();
}

/* ------------------------------------------------------------------ */
/*  task_wakeup — wake a blocked or sleeping task                       */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Wait queue implementation                                           */
/* ------------------------------------------------------------------ */

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

    eevdf_rq_t *rq   = local_rq();
    task_t     *curr = rq->curr;

    curr->vlag       = (int64_t)(avg_vruntime(rq) - curr->vruntime);
    curr->state      = TASK_BLOCKED;
    curr->wake_tick  = 0;
    curr->wait_queue = queue;
    ilist_insert_before(&queue->tasks, &curr->sched_node);

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
    task_t       *task = sched_node_to_task(node);

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
        task_t       *task = sched_node_to_task(node);

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

/* ------------------------------------------------------------------ */
/*  sched_tick — periodic tick accounting and preemption                */
/* ------------------------------------------------------------------ */

void sched_tick(void)
{
    if (!scheduler.started || !cpu_rqs) return;

    uint32_t    cpu_id = get_current_cpu_id();
    eevdf_rq_t *rq     = &cpu_rqs[cpu_id];

    /* CPU 0 handles global tick, sleep queue, and load balancing */
    if (cpu_id == 0) {
        task_t *balanced = NULL;

        spin_lock(&scheduler.lock);
        scheduler.ticks++;
        wake_sleeping_tasks();
        if ((scheduler.ticks % SCHED_LOAD_BALANCE_INTERVAL) == 0) balanced = balance_ready_queues_locked();
        spin_unlock(&scheduler.lock);
        request_task_cpu(balanced);
    }

    task_t *curr = rq->curr;

    /* Idle task: yield if there is real work */
    if (curr == rq->idle) {
        if (has_ready_task()) sched_yield();
        return;
    }

    if (curr->state != TASK_RUNNING) return;

    /* Advance vruntime by 1 tick */
    curr->time_slice++;
    update_curr(rq, 1);

    /* Check if the current task has exhausted its slice (deadline) */
    update_deadline(rq, curr);

    /* Minimum granularity: don't preempt within the first tick(s) */
    if (curr->time_slice < SCHED_MIN_GRANULARITY) return;

    /* Preempt if there is a better candidate */
    if (has_ready_task()) {
        task_t *best = pick_eevdf(rq);
        if (best != curr) sched_yield();
    }
}

/* ------------------------------------------------------------------ */
/*  sched_ticks — return the global tick count                          */
/* ------------------------------------------------------------------ */

uint64_t sched_ticks(void)
{
    return scheduler.ticks;
}

/* ------------------------------------------------------------------ */
/*  task_exit — terminate the current task                              */
/* ------------------------------------------------------------------ */

void task_exit(void)
{
    disable_intr();
    spin_lock(&scheduler.lock);

    task_t *curr = local_current();
    curr->state  = TASK_ZOMBIE;

    eevdf_rq_t *rq = local_rq();
    if (rq->curr == curr) { rq->curr = rq->idle; }

    spin_unlock(&scheduler.lock);

    sched_yield();
    panic("sched: zombie task resumed.");
}

/* ------------------------------------------------------------------ */
/*  current_task — return the task running on this CPU                  */
/* ------------------------------------------------------------------ */

task_t *current_task(void)
{
    return cpu_rqs[get_current_cpu_id()].curr;
}
