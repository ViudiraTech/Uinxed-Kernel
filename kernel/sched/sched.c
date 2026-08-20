/*
 *
 *      sched.c
 *      Kernel EEVDF scheduler
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/fpu.h>
#include <arch/gdt.h>
#include <arch/smp.h>
#include <cgroup/cgroup.h>
#include <drivers/firmware/apic.h>
#include <kernel/debug/debug.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/list/intrusive_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <libs/util/rbtree.h>
#include <mem/heap.h>
#include <mem/page.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <sync/spin_lock.h>

/*
 * Build system may pre-define these via -D in the Makefile.
 * The #ifndef guards ensure the command-line value takes precedence.
 */
#ifndef SCHED_LOAD_BALANCE_INTERVAL
#    define SCHED_LOAD_BALANCE_INTERVAL 8
#endif
#ifndef SCHED_BASE_SLICE
#    define SCHED_BASE_SLICE 2ULL
#endif
#ifndef SCHED_LATENCY
#    define SCHED_LATENCY 8ULL
#endif
#ifndef SCHED_MIN_GRANULARITY
#    define SCHED_MIN_GRANULARITY 1ULL
#endif
#ifndef SCHED_WAKEUP_GRANULARITY
#    define SCHED_WAKEUP_GRANULARITY 0ULL
#endif
#ifndef SCHED_BALANCE_BATCH
#    define SCHED_BALANCE_BATCH 4U
#endif
#ifndef SCHED_MIGRATION_COOLDOWN
#    define SCHED_MIGRATION_COOLDOWN 4ULL
#endif
#ifndef SCHED_AFFINITY_BONUS
#    define SCHED_AFFINITY_BONUS (SCHED_NICE_0_LOAD / 4ULL)
#endif
#ifndef SCHED_SYNC_WAKE_BONUS
#    define SCHED_SYNC_WAKE_BONUS (SCHED_NICE_0_LOAD / 8ULL)
#endif

/* Global state */

scheduler_t     scheduler;
eevdf_rq_t     *cpu_rqs;
uint32_t        cpu_scheduler_count;
static task_t   boot_task = {.pid = 0, .name = "swapper"};
static uint8_t  boot_stack_marker;
static task_t  *ap_boot_tasks;
static uint32_t next_task_cpu;

/* Forward declarations */

void context_switch(task_context_t *prev, task_context_t *next, volatile uint64_t *prev_on_cpu);

/* Context switch (naked assembly) */
__attribute__((naked)) void context_switch(task_context_t *prev __attribute__((unused)), task_context_t *next __attribute__((unused)), volatile uint64_t *prev_on_cpu __attribute__((unused)))
{
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
                     "movq 56(%rsi), %rax\n\t"
                     "movq 64(%rsi), %rdi\n\t"
                     "movq 0(%rsi), %rsp\n\t"
                     "movq $0, (%rdx)\n\t"
                     "pushq %rax\n\t"
                     "popfq\n\t"
                     "ret\n\t");
}

/* Return the task whose scheduler-list node is given */
static task_t *sched_node_to_task(ilist_node_t *node)
{
    return (task_t *)((uint8_t *)node - offsetof(task_t, sched_node));
}

/* Return the task whose timer-list node is given */
static task_t *timer_node_to_task(ilist_node_t *node)
{
    return (task_t *)((uint8_t *)node - offsetof(task_t, timer_node));
}

/* Whether an intrusive-list node is currently linked */
static int node_is_linked(const ilist_node_t *node)
{
    return node->prev && node->next && node->prev != node;
}

/* EEVDF core: virtual-time arithmetic */

/* Convert wall-clock delta to virtual-time delta for a given weight */
static uint64_t calc_delta_fair(uint64_t delta, task_t *task)
{
    if (task->weight == SCHED_NICE_0_LOAD) return delta;
    return delta * SCHED_NICE_0_LOAD / task->weight;
}

/* Add a signed offset to vruntime, saturating at zero on the low end */
static uint64_t add_signed_vruntime(uint64_t base, int64_t offset)
{
    if (offset >= 0) return base + (uint64_t)offset;
    uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1;
    return magnitude > base ? 0 : base - magnitude;
}

/*
 * Compute V, the weighted-average vruntime of every runnable entity.
 * rq->avg_{vruntime,load} describe the RB tree only; the running entity is
 * outside that tree and must be folded in explicitly.  Omitting curr makes V
 * stick near an old value whenever a CPU has only one runnable task.  A task
 * woken later is then placed far in the future and can remain ineligible for
 * seconds even though the CPU has runnable work.
 */
static uint64_t avg_vruntime(eevdf_rq_t *rq)
{
    int64_t  average = rq->avg_vruntime;
    uint64_t load    = rq->avg_load;

    if (rq->curr && rq->curr->state == TASK_RUNNING && rq->curr != rq->idle) {
        average += (int64_t)(rq->curr->vruntime - rq->min_vruntime) * (int64_t)rq->curr->weight;
        load += rq->curr->weight;
    }
    if (!load) return rq->min_vruntime;
    return add_signed_vruntime(rq->min_vruntime, average / (int64_t)load);
}

/*
 * Scale the base slice by the number of runnable tasks to stay within
 * the scheduling latency period.  Ensures each task gets at least
 * min_granularity.
 */
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

/*
 * EEVDF core: eligibility check
 *
 * A task is eligible iff lag >= 0, i.e. its ideal service time
 * exceeds its actual service time.  In discrete form:
 * avg_vruntime >= (vruntime - min_vruntime) * avg_load
 */

/* Whether a vruntime is eligible given the runqueue state */
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

/* Whether a task is eligible to run */
static int entity_eligible(eevdf_rq_t *rq, task_t *task)
{
    return entity_eligible_vruntime(rq, task->vruntime);
}

/* EEVDF core: RB-tree comparison and augmentation */

/* Compare two rb_nodes by their task's deadline (tiebreak: vruntime) */
static int entity_less(const rb_node_t *a, const rb_node_t *b)
{
    const task_t *ta = rb_entry(a, task_t, run_node);
    const task_t *tb = rb_entry(b, task_t, run_node);

    if (ta->deadline != tb->deadline) return (int64_t)(ta->deadline - tb->deadline) < 0;
    return (int64_t)(ta->vruntime - tb->vruntime) < 0;
}

/*
 * Check whether candidate is "significantly" better than curr.
 * Uses wakeup_granularity to prevent preemption ping-pong.
 */
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

/*
 * rq->avg_vruntime is stored relative to rq->min_vruntime.  Moving the
 * origin therefore requires rebasing the weighted sum.  The old code merely
 * assigned min_vruntime in update_curr(); the accumulated error grew on every
 * timer tick and eventually placed newly forked/woken tasks thousands of
 * ticks ahead of the active desktop tasks.
 */
static void advance_min_vruntime(eevdf_rq_t *rq)
{
    uint64_t candidate = UINT64_MAX;

    if (rq->curr && rq->curr->state == TASK_RUNNING && rq->curr != rq->idle) candidate = rq->curr->vruntime;
    if (rq->timeline.root) {
        uint64_t queued = rq->timeline.root->min_vruntime;
        if (candidate == UINT64_MAX || (int64_t)(queued - candidate) < 0) candidate = queued;
    }
    if (candidate == UINT64_MAX || (int64_t)(candidate - rq->min_vruntime) <= 0) return;

    uint64_t delta = candidate - rq->min_vruntime;
    rq->avg_vruntime -= (int64_t)delta * (int64_t)rq->avg_load;
    rq->min_vruntime = candidate;
}

/* EEVDF core: avg_vruntime / avg_load bookkeeping */

/* Add a task's vruntime contribution to the runqueue */
static void avg_vruntime_add(eevdf_rq_t *rq, task_t *task)
{
    int64_t delta = (int64_t)(task->vruntime - rq->min_vruntime) * (int64_t)task->weight;
    rq->avg_vruntime += delta;

    /* avg_load is read locklessly by the cross-CPU balancer. */
    __atomic_add_fetch(&rq->avg_load, task->weight, __ATOMIC_RELAXED);
}

/* Subtract a task's vruntime contribution from the runqueue */
static void avg_vruntime_sub(eevdf_rq_t *rq, task_t *task)
{
    int64_t delta = (int64_t)(task->vruntime - rq->min_vruntime) * (int64_t)task->weight;

    rq->avg_vruntime -= delta;
    __atomic_sub_fetch(&rq->avg_load, task->weight, __ATOMIC_RELAXED);
}

/* EEVDF core: update_curr - advance vruntime of the running task */
static void update_curr(eevdf_rq_t *rq, uint64_t delta_ticks)
{
    task_t *curr = rq->curr;

    if (!curr || curr == rq->idle) return;

    curr->vruntime += calc_delta_fair(delta_ticks, curr);
    advance_min_vruntime(rq);
}

/* EEVDF core: update_deadline - assign a new deadline slice */
static void update_deadline(eevdf_rq_t *rq, task_t *task)
{
    /* Only update if the task has consumed its current slice */
    if ((int64_t)(task->vruntime - task->deadline) < 0) return;

    uint64_t slice = calc_effective_slice(rq);

    task->deadline = task->vruntime + calc_delta_fair(slice, task);
    task->vlag     = (int64_t)(avg_vruntime(rq) - task->vruntime);
}

/* EEVDF core: place_entity - set vruntime/deadline on enqueue */
static void place_entity(eevdf_rq_t *rq, task_t *task, int initial)
{
    uint64_t vruntime = avg_vruntime(rq);
    int64_t  lag      = 0;

    uint64_t slice  = calc_effective_slice(rq);
    uint64_t vslice = calc_delta_fair(slice, task);

    /*
     * PLACE_LAG: adjust vruntime based on stored vlag.
     * Scale the stored lag to account for the changed load.
     */
    uint64_t load = rq->avg_load;
    if (rq->curr && rq->curr->state == TASK_RUNNING && rq->curr != rq->idle) load += rq->curr->weight;
    if (load) {
        uint64_t new_load = load + task->weight;
        lag               = task->vlag;
        if (new_load > load) lag = lag * (int64_t)new_load / (int64_t)load;

        /*
         * Sleeping credit/debt is bounded just as it is in Linux EEVDF.
         * This also prevents a stale value surviving a CPU migration from
         * pushing an otherwise runnable task arbitrarily far into the future.
         */
        int64_t lag_limit = (int64_t)(vslice * 2);
        if (lag > lag_limit) lag = lag_limit;
        if (lag < -lag_limit) lag = -lag_limit;
    }

    task->vruntime = add_signed_vruntime(vruntime, -lag);

    /* PLACE_DEADLINE_INITIAL: new tasks start with half a slice */
    if (initial) vslice /= 2;

    task->deadline = task->vruntime + vslice;
}

/* EEVDF core: enqueue_entity / dequeue_entity */
static void enqueue_entity(eevdf_rq_t *rq, task_t *task)
{
    /*
     * Only ready tasks belong to the EEVDF timeline.  In particular,
     * never let a late wakeup resurrect a task that has already exited.
     */
    if (!task || task->state != TASK_READY) {
        static uint64_t last_log;
        if (task && scheduler.ticks - last_log >= 1000) {
            plogk("sched: Refusing to enqueue task %llu (%s) in state %u\n", task->pid, task->name, task->state);
            last_log = scheduler.ticks;
        }
        return;
    }

    avg_vruntime_add(rq, task);
    rb_insert_augmented(&rq->timeline, &task->run_node, entity_less, update_min_vruntime, NULL);
    __atomic_add_fetch(&rq->nr_running, 1, __ATOMIC_RELAXED);
}

/* Remove a task from the EEVDF timeline */
static void dequeue_entity(eevdf_rq_t *rq, task_t *task)
{
    rb_erase_augmented(&rq->timeline, &task->run_node, update_min_vruntime, NULL);
    avg_vruntime_sub(rq, task);
    if (__atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED)) __atomic_sub_fetch(&rq->nr_running, 1, __ATOMIC_RELAXED);
}

/*
 * EEVDF core: pick_eevdf - select the next task to run
 *
 * Strategy:
 * 1. If only one task is runnable, return it directly.
 * 2. Check the cached leftmost (earliest deadline).
 * If eligible, it wins.
 * 3. Otherwise, traverse the rbtree, using the min_vruntime
 * augmentation to skip subtrees that contain no eligible
 * entities.
 */
static task_t *pick_eevdf(eevdf_rq_t *rq)
{
    task_t    *curr = rq->curr;
    rb_node_t *node;

    /* Fast path: empty runqueue */
    if (rq->nr_running == 0) return rq->idle;

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

        /* Neither left nor current is eligible - go right */
        node = node->right;
    }

    /* No eligible entity found; keep current if it is still runnable. */
    if (curr && curr->state == TASK_RUNNING) return curr;

    /*
     * nr_running is authoritative: never idle a CPU while its runqueue is
     * non-empty.  With exact EEVDF accounting at least one entity is eligible,
     * but this fallback also makes rounding and transient rebase errors safe.
     */
    if (rq->timeline.leftmost) return rb_entry(rq->timeline.leftmost, task_t, run_node);
    return rq->idle;
}

/* Return the runqueue of the current CPU */
static eevdf_rq_t *local_rq(void)
{
    return &cpu_rqs[get_current_cpu_id()];
}

/* Return the task currently running on this CPU */
static task_t *local_current(void)
{
    return __atomic_load_n(&local_rq()->curr, __ATOMIC_RELAXED);
}

/* Point the TSS stack at the given task's kernel stack */
static void update_tss_stack(task_t *task)
{
    if (!task) return;

    if (task->process && task->process->kernel_stack) {
        set_kernel_stack((uint64_t)(task->process->kernel_stack + PROCESS_KERNEL_STACK));
    } else if (task->kernel_stack) {
        set_kernel_stack((uint64_t)(task->kernel_stack + TASK_KERNEL_STACK));
    }
}

/* Place a task on the given CPU's runqueue. Takes the target rq lock (the caller holds scheduler.lock, or none). */
static void enqueue_task_on_cpu(task_t *task, uint32_t cpu_id, int initial)
{
    if (!task || task->state == TASK_ZOMBIE || task->state == TASK_IDLE) return;
    if (cpu_id >= cpu_scheduler_count) cpu_id = 0;

    eevdf_rq_t *rq = &cpu_rqs[cpu_id];

    spin_lock(&rq->lock);
    place_entity(rq, task, initial);
    task->state      = TASK_READY;
    task->wake_tick  = 0;
    task->wait_queue = NULL;
    task->cpu_id     = cpu_id;

    enqueue_entity(rq, task);
    spin_unlock(&rq->lock);
}

/* Enqueue a task on its assigned CPU */
void enqueue_task(task_t *task)
{
    enqueue_task_on_cpu(task, task->cpu_id, 0);
}

/* Enqueue a newly created task with initial placement */
void enqueue_task_initial(task_t *task)
{
    enqueue_task_on_cpu(task, task->cpu_id, 1);
}

/* Wake a sleeping or blocked task, enqueueing it if runnable */
static void wake_task_locked(task_t *task, int remove_linked_node)
{
    if (!task) return;

    /*
     * A readiness notification can race another wakeup.  Waking an already
     * runnable task is a normal idempotent operation, not scheduler damage.
     */
    if (task->state == TASK_READY || task->state == TASK_RUNNING) return;

    if (task->state == TASK_STOPPED || task->state == TASK_IDLE || task->state == TASK_ZOMBIE) {
        {
            static uint64_t last_log;
            if (scheduler.ticks - last_log >= 1000) {
                plogk("sched: Rejected wake of task %llu (%s) in state %u\n", task->pid, task->name, task->state);
                last_log = scheduler.ticks;
            }
        }
        return;
    }

    if (remove_linked_node) ilist_remove(&task->sched_node);
    if (task->process && __atomic_load_n(&task->process->signal.group_stopped, __ATOMIC_ACQUIRE)) {
        task->state     = TASK_STOPPED;
        task->wake_tick = 0;
        return;
    }
    enqueue_task(task);
}

/* Wait-queue and timer membership are serialized by scheduler.lock. */
static void finish_wait_locked(task_t *task, task_wake_reason_t reason)
{
    if (!task->wait_queue) return;

    ilist_remove(&task->sched_node);
    task->wait_queue  = NULL;
    task->wake_reason = reason;
    task->wake_tick   = 0;
    if (node_is_linked(&task->timer_node)) ilist_remove(&task->timer_node);

    if (task->state == TASK_BLOCKED) wake_task_locked(task, 0);
}

/* Send a reschedule IPI to a task's CPU when it is remote. */
void request_task_cpu(task_t *task)
{
    if (!task || !scheduler.started || !cpu_rqs) return;

    uint32_t target = task->cpu_id;
    uint32_t local  = get_current_cpu_id();
    if (target >= cpu_scheduler_count || target == local || !cpu_rqs[target].online) return;

    /* Coalesce wake storms: one outstanding reschedule IPI per target CPU. */
    if (!__atomic_exchange_n(&cpu_rqs[target].resched_pending, 1, __ATOMIC_ACQ_REL)) send_ipi_cpu(target, IPI_RESCHEDULE);
}

/* Put a task to sleep until a wake tick */
static void sleep_task(task_t *task, uint64_t wake_tick)
{
    task->state      = TASK_SLEEPING;
    task->wake_tick  = wake_tick;
    task->wait_queue = NULL;
    ilist_insert_before(&scheduler.sleep_queue, &task->sched_node);
}

/* Load balancing */

/*
 * Number of runnable entities including the currently executing non-idle task.
 * Read locklessly (relaxed atomics) for the cross-CPU balancer.
 */
static uint64_t rq_task_count(const eevdf_rq_t *rq)
{
    uint64_t count = __atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED);
    task_t  *curr  = __atomic_load_n(&rq->curr, __ATOMIC_RELAXED);
    if (curr && curr != rq->idle && __atomic_load_n(&curr->state, __ATOMIC_RELAXED) == TASK_RUNNING) count++;
    return count;
}

/* Weighted runnable load.  avg_load covers queued tasks; curr lives outside the tree. */
static uint64_t rq_weighted_load(const eevdf_rq_t *rq)
{
    uint64_t load = __atomic_load_n(&rq->avg_load, __ATOMIC_RELAXED);
    task_t  *curr = __atomic_load_n(&rq->curr, __ATOMIC_RELAXED);
    if (curr && curr != rq->idle && __atomic_load_n(&curr->state, __ATOMIC_RELAXED) == TASK_RUNNING) load += __atomic_load_n(&curr->weight, __ATOMIC_RELAXED);
    return load;
}

/* Treat an online CPU with neither queued nor executing work as immediately idle. */
static bool rq_is_idle_cpu(uint32_t cpu)
{
    if (cpu >= cpu_scheduler_count || !cpu_rqs[cpu].online) return false;
    eevdf_rq_t *rq   = &cpu_rqs[cpu];
    task_t     *curr = __atomic_load_n(&rq->curr, __ATOMIC_RELAXED);
    return __atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED) == 0 && (!curr || curr == rq->idle || __atomic_load_n(&curr->state, __ATOMIC_RELAXED) != TASK_RUNNING);
}

/*
 * Score a CPU for placement.  The basic unit is NICE_0_LOAD so weighted tasks
 * and queue depth naturally influence the result.  Previous-CPU affinity is a
 * discount, not a hard pin, which avoids cache-thrashing migration while still
 * allowing a meaningfully less loaded CPU to win.
 */
static uint64_t placement_score_locked(task_t *task, uint32_t cpu, uint32_t prev_cpu, bool sync)
{
    uint64_t score = rq_weighted_load(&cpu_rqs[cpu]);

    /* Queue depth matters even when weights happen to be small. */
    score += __atomic_load_n(&cpu_rqs[cpu].nr_running, __ATOMIC_RELAXED) * (SCHED_NICE_0_LOAD / 8ULL);
    if (cpu == prev_cpu && score >= SCHED_AFFINITY_BONUS) score -= SCHED_AFFINITY_BONUS;

    /* WF_SYNC-like hint: the waker may block/yield soon, so stacking is a bit cheaper. */
    uint32_t this_cpu = get_current_cpu_id();
    if (sync && cpu == this_cpu && score >= SCHED_SYNC_WAKE_BONUS) score -= SCHED_SYNC_WAKE_BONUS;
    if (task && cpu != prev_cpu && scheduler.ticks - task->last_migrate_tick < SCHED_MIGRATION_COOLDOWN) score += SCHED_NICE_0_LOAD / 2ULL;

    return score;
}

/*
 * Linux select_task_rq_fair() first tries wake-affine/idle placement and only
 * falls back to wider balancing when needed.  Uinxed has no sched-domain/LLC
 * topology yet, so use the same policy shape over the online CPU set:
 *   1. keep an idle previous CPU (best cache locality, full parallelism),
 *   2. otherwise use any idle CPU,
 *   3. otherwise compare weighted pressure with an affinity hysteresis.
 */
static uint32_t select_wakeup_cpu_locked(task_t *task, bool sync)
{
    if (!cpu_rqs || !cpu_scheduler_count) return 0;

    uint32_t this_cpu = get_current_cpu_id();
    if (this_cpu >= cpu_scheduler_count) this_cpu = 0;

    uint32_t prev_cpu = task && task->cpu_id < cpu_scheduler_count ? task->cpu_id : this_cpu;
    if (!cpu_rqs[prev_cpu].online) prev_cpu = this_cpu;
    if (rq_is_idle_cpu(prev_cpu)) return prev_cpu;

    /* Rotate the idle scan so CPU 0 is not a permanent magnet for wakeups. */
    uint32_t start = (next_task_cpu++) % cpu_scheduler_count;
    for (uint32_t n = 0; n < cpu_scheduler_count; n++) {
        uint32_t cpu = (start + n) % cpu_scheduler_count;
        if (rq_is_idle_cpu(cpu)) return cpu;
    }

    uint32_t best       = prev_cpu;
    uint64_t best_score = placement_score_locked(task, best, prev_cpu, sync);

    for (uint32_t cpu = 0; cpu < cpu_scheduler_count; cpu++) {
        if (!cpu_rqs[cpu].online || cpu == best) continue;
        uint64_t score = placement_score_locked(task, cpu, prev_cpu, sync);
        if (score < best_score) {
            best       = cpu;
            best_score = score;
        }
    }

    return best;
}

/* Choose a low-pressure CPU for a newly runnable task. */
uint32_t choose_task_cpu_locked(void)
{
    if (!cpu_rqs || !cpu_scheduler_count) return 0;

    uint32_t start      = (next_task_cpu++) % cpu_scheduler_count;
    uint32_t best       = UINT32_MAX;
    uint64_t best_score = UINT64_MAX;

    for (uint32_t n = 0; n < cpu_scheduler_count; n++) {
        uint32_t cpu = (start + n) % cpu_scheduler_count;
        if (!cpu_rqs[cpu].online) continue;

        uint64_t score = rq_weighted_load(&cpu_rqs[cpu]);
        score += __atomic_load_n(&cpu_rqs[cpu].nr_running, __ATOMIC_RELAXED) * (SCHED_NICE_0_LOAD / 8ULL);
        if (score < best_score) {
            best       = cpu;
            best_score = score;
            if (!score) break;
        }
    }

    return best == UINT32_MAX ? 0 : best;
}

/* Whether the current runqueue has a runnable task. */
static int has_ready_task(void)
{
    return __atomic_load_n(&local_rq()->nr_running, __ATOMIC_RELAXED) > 0;
}

/* In-order predecessor helper used to scan low-urgency EEVDF candidates. */
static rb_node_t *rb_prev_local(rb_node_t *node)
{
    if (!node) return NULL;
    if (node->left) {
        node = node->left;
        while (node->right) node = node->right;
        return node;
    }
    rb_node_t *parent = node->parent;
    while (parent && node == parent->left) {
        node   = parent;
        parent = parent->parent;
    }
    return parent;
}

/* Pick a queued task that is cheap to migrate, preferring the least urgent. */
static task_t *pick_steal_candidate_locked(eevdf_rq_t *src, bool newly_idle)
{
    rb_node_t *node = src->timeline.root;
    if (!node) return NULL;
    while (node->right) node = node->right;

    unsigned int scanned = 0;
    while (node && scanned++ < 8) {
        task_t *task = rb_entry(node, task_t, run_node);
        bool    hot  = scheduler.ticks - task->last_migrate_tick < SCHED_MIGRATION_COOLDOWN;
        if (!hot || newly_idle || __atomic_load_n(&src->nr_running, __ATOMIC_RELAXED) > 2) return task;
        node = rb_prev_local(node);
    }
    return NULL;
}

/* Find the most overloaded source runqueue relative to dst. */
static uint32_t find_busiest_cpu_locked(uint32_t dst)
{
    uint32_t busiest  = UINT32_MAX;
    uint64_t max_load = 0;

    for (uint32_t cpu = 0; cpu < cpu_scheduler_count; cpu++) {
        if (cpu == dst || !cpu_rqs[cpu].online || __atomic_load_n(&cpu_rqs[cpu].nr_running, __ATOMIC_RELAXED) == 0) continue;
        uint64_t load = rq_weighted_load(&cpu_rqs[cpu]);
        if (load > max_load) {
            max_load = load;
            busiest  = cpu;
        }
    }
    return busiest;
}

/*
 * Move one queued entity, preserving EEVDF lag across the runqueue boundary.
 * Caller holds scheduler.lock.  Dequeues from src under src->lock, then
 * enqueues on dst (enqueue_task_on_cpu takes dst->lock) - never two rq locks at once.
 */
static task_t *migrate_one_locked(uint32_t src_cpu, uint32_t dst_cpu, bool newly_idle)
{
    if (src_cpu >= cpu_scheduler_count || dst_cpu >= cpu_scheduler_count || src_cpu == dst_cpu) return NULL;
    eevdf_rq_t *src = &cpu_rqs[src_cpu];
    eevdf_rq_t *dst = &cpu_rqs[dst_cpu];
    if (!__atomic_load_n(&src->nr_running, __ATOMIC_RELAXED) || !dst->online) return NULL;

    spin_lock(&src->lock);
    task_t *task = pick_steal_candidate_locked(src, newly_idle);
    if (!task || task->state != TASK_READY || __atomic_load_n(&task->on_cpu, __ATOMIC_ACQUIRE)) {
        spin_unlock(&src->lock);
        return NULL;
    }

    task->vlag = (int64_t)(avg_vruntime(src) - task->vruntime);
    dequeue_entity(src, task);

    task->last_cpu          = src_cpu;
    task->last_migrate_tick = scheduler.ticks;
    task->migration_count++;
    __atomic_add_fetch(&src->nr_migrations, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&dst->nr_migrations, 1, __ATOMIC_RELAXED);
    if (newly_idle) __atomic_add_fetch(&dst->nr_steals, 1, __ATOMIC_RELAXED);
    spin_unlock(&src->lock);

    enqueue_task_on_cpu(task, dst_cpu, 0);
    return task;
}

/*
 * New-idle work stealing: pull exactly one task.  Like Linux newidle_balance,
 * the critical path is deliberately bounded so going idle never turns into a
 * long O(N*tasks) scan.
 */
static task_t *newidle_balance_locked(uint32_t dst_cpu)
{
    if (cpu_scheduler_count < 2 || dst_cpu >= cpu_scheduler_count) return NULL;
    if (__atomic_load_n(&cpu_rqs[dst_cpu].nr_running, __ATOMIC_RELAXED)) return NULL;

    uint32_t src_cpu = find_busiest_cpu_locked(dst_cpu);
    if (src_cpu == UINT32_MAX) return NULL;

    uint64_t src_load = rq_weighted_load(&cpu_rqs[src_cpu]);
    uint64_t dst_load = rq_weighted_load(&cpu_rqs[dst_cpu]);
    if (src_load <= dst_load + SCHED_NICE_0_LOAD / 2ULL) return NULL;

    return migrate_one_locked(src_cpu, dst_cpu, true);
}

/*
 * Self-locking wrapper used when the caller does not hold scheduler.lock
 * (sched_switch's going-idle path).  Returns the task stolen onto dst, if any.
 */
static task_t *newidle_balance(uint32_t dst_cpu)
{
    spin_lock(&scheduler.lock);
    task_t *stolen = newidle_balance_locked(dst_cpu);
    spin_unlock(&scheduler.lock);
    return stolen;
}

/* Periodic bounded balancing for sustained asymmetric load. */
static task_t *balance_ready_queues_locked(void)
{
    if (cpu_scheduler_count < 2) return NULL;

    uint32_t busiest  = UINT32_MAX;
    uint32_t idlest   = UINT32_MAX;
    uint64_t max_load = 0;
    uint64_t min_load = UINT64_MAX;

    for (uint32_t cpu = 0; cpu < cpu_scheduler_count; cpu++) {
        if (!cpu_rqs[cpu].online) continue;
        uint64_t load = rq_weighted_load(&cpu_rqs[cpu]);
        if (load > max_load) {
            max_load = load;
            busiest  = cpu;
        }
        if (load < min_load) {
            min_load = load;
            idlest   = cpu;
        }
    }

    if (busiest == UINT32_MAX || idlest == UINT32_MAX || busiest == idlest) return NULL;
    if (max_load <= min_load + SCHED_NICE_0_LOAD) return NULL;

    task_t      *first  = NULL;
    unsigned int budget = SCHED_BALANCE_BATCH;
    while (budget-- && __atomic_load_n(&cpu_rqs[busiest].nr_running, __ATOMIC_RELAXED)) {
        uint64_t src_load = rq_weighted_load(&cpu_rqs[busiest]);
        uint64_t dst_load = rq_weighted_load(&cpu_rqs[idlest]);
        if (src_load <= dst_load + SCHED_NICE_0_LOAD) break;

        task_t *moved = migrate_one_locked(busiest, idlest, false);
        if (!moved) break;
        if (!first) first = moved;

        /* Stop near the midpoint instead of overshooting and bouncing back. */
        if (rq_task_count(&cpu_rqs[busiest]) <= rq_task_count(&cpu_rqs[idlest]) + 1) break;
    }
    __atomic_store_n(&cpu_rqs[idlest].last_balance, scheduler.ticks, __ATOMIC_RELAXED);
    return first;
}

/* Wake sleeping tasks and timed wait-queue tasks whose deadlines have expired */
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

    /*
     * Check timed wait-queue tasks: tasks that are in a wait queue
     * but also have a wake_tick deadline (e.g. futex timeouts).
     * On expiry, remove from the wait queue and wake.
     */
    node = scheduler.timer_queue.next;
    while (node != &scheduler.timer_queue) {
        ilist_node_t *next = node->next;
        task_t       *task = timer_node_to_task(node);

        if (task->wake_tick <= scheduler.ticks) {
            ilist_remove(node);
            finish_wait_locked(task, TASK_WAKE_TIMEOUT);
        }
        node = next;
    }
}

/* Per-CPU idle loop: halt until an interrupt, then yield to real work */
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

/*
 * Allocate an idle (swapper/N) task with PID 0.  Idle tasks are not real
 * schedulable tasks: they are absent from the PID hash and cgroup, and are
 * never reaped.  They are never created through task_alloc().
 */
static task_t *idle_task_alloc(uint32_t cpu_id)
{
    task_t *idle = calloc(1, sizeof(task_t));
    if (!idle) return NULL;

    idle->pid            = 0;
    idle->tgid           = 0;
    idle->state          = TASK_IDLE;
    idle->cpu_id         = cpu_id;
    idle->page_directory = get_kernel_pagedir();
    idle->weight         = SCHED_NICE_0_LOAD;
    idle->base_weight    = SCHED_NICE_0_LOAD;
    idle->pi_weight      = SCHED_NICE_0_LOAD;
    ilist_init(&idle->sched_node);
    ilist_init(&idle->timer_node);
    ilist_init(&idle->thread_node);
    ilist_init(&idle->cgroup_node);
    (void)snprintf(idle->name, sizeof(idle->name), "swapper/%u", cpu_id);

    idle->kernel_stack = malloc(TASK_KERNEL_STACK);
    if (!idle->kernel_stack) {
        free(idle);
        return NULL;
    }

    uint64_t *stack      = (uint64_t *)ALIGN_DOWN((uint64_t)(idle->kernel_stack + TASK_KERNEL_STACK), 16ULL);
    *(--stack)           = (uint64_t)idle_thread;
    idle->context.rsp    = (uint64_t)stack;
    idle->context.rdi    = (uint64_t)(uintptr_t)NULL; // NOLINT(bugprone-casting-through-void)
    idle->context.rflags = 0x202;
    return idle;
}

/* sched_init - bootstrap the scheduler */
void sched_init(void)
{
    memset(&scheduler, 0, sizeof(scheduler));
    ilist_init(&scheduler.sleep_queue);
    ilist_init(&scheduler.timer_queue);
    scheduler.next_pid = 0;

    cpu_scheduler_count = get_cpu_count();
    if (!cpu_scheduler_count) cpu_scheduler_count = 1;
    cpu_rqs       = calloc(cpu_scheduler_count, sizeof(eevdf_rq_t));
    ap_boot_tasks = calloc(cpu_scheduler_count, sizeof(task_t));
    if (!cpu_rqs || !ap_boot_tasks) panic("sched: Cannot allocate per-CPU scheduler state.");

    for (uint32_t i = 0; i < cpu_scheduler_count; i++) {
        rb_init_root(&cpu_rqs[i].timeline);
        cpu_rqs[i].online = 1;
    }

    next_task_cpu = 0;
    __atomic_store_n(&cpu_rqs[0].curr, &boot_task, __ATOMIC_RELAXED);
    cpu_rqs[0].nr_running = 0;

    memset(&boot_task, 0, sizeof(boot_task));
    boot_task.pid            = 0;
    boot_task.state          = TASK_RUNNING;
    boot_task.page_directory = get_kernel_pagedir();
    boot_task.kernel_stack   = &boot_stack_marker;
    boot_task.weight         = SCHED_NICE_0_LOAD;
    boot_task.process        = NULL;
    task_name_copy(&boot_task, "swapper/0");
    ilist_init(&boot_task.sched_node);
    ilist_init(&boot_task.timer_node);

    /* boot_task is the swapper/0 idle task for CPU 0 */
    cpu_rqs[0].idle = &boot_task;
    percpu_gs_set_current(&boot_task);

    for (uint32_t i = 1; i < cpu_scheduler_count; i++) {
        cpu_rqs[i].idle = idle_task_alloc(i);
        if (!cpu_rqs[i].idle) panic("sched: Cannot create idle task.");
    }
    plogk("sched: Created task 0 (swapper/0) on CPU 0\n");
    for (unsigned int i = 1; i < cpu_scheduler_count; i++) plogk("sched: Created task %llu (%s) on CPU %u\n", cpu_rqs[i].idle->pid, cpu_rqs[i].idle->name, cpu_rqs[i].idle->cpu_id);
    plogk("sched: %u CPU(s) registered, using EEVDF scheduler.\n", cpu_scheduler_count);

    scheduler.next_pid = 1;
    for (uint32_t i = 1; i < cpu_scheduler_count; i++) {
        memset(&ap_boot_tasks[i], 0, sizeof(task_t));
        ap_boot_tasks[i].pid            = 0;
        ap_boot_tasks[i].state          = TASK_BLOCKED;
        ap_boot_tasks[i].page_directory = get_kernel_pagedir();
        ap_boot_tasks[i].cpu_id         = i;
        ap_boot_tasks[i].weight         = SCHED_NICE_0_LOAD;
        ilist_init(&ap_boot_tasks[i].sched_node);
        ilist_init(&ap_boot_tasks[i].timer_node);
        __atomic_store_n(&cpu_rqs[i].curr, &ap_boot_tasks[i], __ATOMIC_RELAXED);
    }
}

/* Mark an application processor's runqueue online */
void sched_ap_online(uint32_t cpu_id)
{
    if (!cpu_rqs || cpu_id >= cpu_scheduler_count) return;

    cpu_rqs[cpu_id].online = 1;
}

/* Enter the scheduler loop on an application processor */
void sched_ap_start(uint32_t cpu_id)
{
    while (!cpu_rqs || !cpu_scheduler_count) __asm__ volatile("pause");
    if (cpu_id == 0 || cpu_id >= cpu_scheduler_count) krn_halt();

    while (!scheduler.started) __asm__ volatile("pause");
    __atomic_store_n(&cpu_rqs[cpu_id].curr, &ap_boot_tasks[cpu_id], __ATOMIC_RELAXED);
    percpu_gs_set_current(&ap_boot_tasks[cpu_id]);
    sched_yield();
    panic("sched: AP scheduler exited.");
}

/* Handle a reschedule IPI, yielding when work is ready */
void sched_ipi_reschedule(void)
{
    uint32_t cpu_id = get_current_cpu_id();

    if (!cpu_rqs || cpu_id >= cpu_scheduler_count) return;
    __atomic_add_fetch(&cpu_rqs[cpu_id].reschedule_ipis, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu_rqs[cpu_id].resched_pending, 0, __ATOMIC_RELEASE);
    if (!scheduler.started) return;

    /* A group-exit IPI must also retire a CPU-bound thread in userspace. */
    task_t *current = current_task();
    if (current && current->process && __atomic_load_n(&current->process->signal.group_exit, __ATOMIC_ACQUIRE))
        process_exit(__atomic_load_n(&current->process->signal.group_exit_code, __ATOMIC_RELAXED));

    spin_lock(&cpu_rqs[cpu_id].lock);
    current    = cpu_rqs[cpu_id].curr;
    bool ready = has_ready_task() || (current && current->state != TASK_RUNNING && current != cpu_rqs[cpu_id].idle);
    spin_unlock(&cpu_rqs[cpu_id].lock);
    if (ready) sched_yield();
}

/* Return the number of registered scheduler CPUs */
uint32_t sched_cpu_count(void)
{
    return cpu_scheduler_count;
}

/* task_set_cpu - migrate a task to a different CPU */
int task_set_cpu(task_t *task, uint32_t cpu_id)
{
    if (!task || cpu_id >= cpu_scheduler_count) return 1;

    spin_lock(&scheduler.lock);
    if (task->state == TASK_RUNNING || task->state == TASK_IDLE || task->state == TASK_ZOMBIE) {
        spin_unlock(&scheduler.lock);
        return 1;
    }

    uint32_t old_cpu = task->cpu_id;
    if (task->state == TASK_READY) {
        eevdf_rq_t *src = &cpu_rqs[old_cpu];
        spin_lock(&src->lock);
        task->vlag = (int64_t)(avg_vruntime(src) - task->vruntime);
        dequeue_entity(src, task);
        spin_unlock(&src->lock);
        enqueue_task_on_cpu(task, cpu_id, 0); // locks dst internally
    } else {
        task->cpu_id = cpu_id;
    }
    if (old_cpu != cpu_id) {
        task->last_cpu          = old_cpu;
        task->last_migrate_tick = scheduler.ticks;
        task->migration_count++;
        __atomic_add_fetch(&cpu_rqs[old_cpu].nr_migrations, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&cpu_rqs[cpu_id].nr_migrations, 1, __ATOMIC_RELAXED);
    }
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);
    return 0;
}

/* Switch to the next runnable task on the current CPU */
static void sched_switch(bool voluntary)
{
    eevdf_rq_t *rq           = local_rq();
    uint64_t    entry_rflags = spin_lock_irqsave(&rq->lock);

    task_t *prev = rq->curr;
    task_t *next;

    /* Advance vruntime and re-enqueue the current task if it was running */
    if (prev && prev->state == TASK_RUNNING && prev != rq->idle) {
        update_deadline(rq, prev);
        prev->vlag  = (int64_t)(avg_vruntime(rq) - prev->vruntime);
        prev->state = TASK_READY;
        enqueue_entity(rq, prev);
    }

    next = pick_eevdf(rq);

    /*
     * Going idle: pull one task immediately instead of waiting for CPU 0's
     * periodic pass.  Drop the rq lock first so scheduler.lock stays outer,
     * steal, then re-lock.  Keep interrupts off across the gap so a tick
     * cannot charge the just-blocked task's itimers while prev is no longer
     * the running task.
     */
    if (next == rq->idle && rq->nr_running == 0) {
        /* Keep IRQs disabled while rq->curr is between scheduling states. */
        spin_unlock_irqrestore(&rq->lock, entry_rflags & ~(1ULL << 9));
        task_t *stolen = newidle_balance(get_current_cpu_id());
        (void)spin_lock_irqsave(&rq->lock);
        next = stolen ? stolen : pick_eevdf(rq);
    }

    if (prev == next) {
        /*
         * A remote wake may have queued this CPU's current task between
         * committing a block and entering the scheduler.
         */
        if (next && next != rq->idle && next->state == TASK_READY) {
            dequeue_entity(rq, next);
            next->state      = TASK_RUNNING;
            next->time_slice = 0;
            advance_min_vruntime(rq);
        }
        spin_unlock_irqrestore(&rq->lock, entry_rflags);
        return;
    }

    /*
     * Advance min_vruntime when going idle so that tasks waking up
     * later don't get a huge vruntime windfall.
     */
    if (next == rq->idle && rq->nr_running == 0) {
        uint64_t avg = avg_vruntime(rq);
        if ((int64_t)(avg - rq->min_vruntime) > 0) rq->min_vruntime = avg;
    }

    /*
     * Dequeue the selected task.  During bootstrap swapper/0 (also rq->idle)
     * blocks on a kthread_create() wait queue and is re-enqueued, so it can be
     * selected here as a TASK_READY entity and must be dequeued like any other
     * task.  A normally-idle task is never enqueued (TASK_IDLE), so
     * next->state == TASK_READY still excludes it.
     */
    if (next->state == TASK_READY) {
        dequeue_entity(rq, next);
        next->state      = TASK_RUNNING;
        next->time_slice = 0;
    }

    __atomic_store_n(&next->on_cpu, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&rq->curr, next, __ATOMIC_RELAXED);

    /* Keep the GS-relative current pointer in lockstep with rq->curr. */
    percpu_gs_set_current(next);
    advance_min_vruntime(rq);
    update_tss_stack(next);

    /*
     * Retaining CR3 preserves the TLB when switching between threads in
     * the same address space (and for kernel threads).
     */
    if (!prev || prev->page_directory != next->page_directory) switch_page_directory(next->page_directory);
    rq->context_switches++;
    if (prev && prev != rq->idle) {
        if (voluntary)
            prev->voluntary_switches++;
        else
            prev->involuntary_switches++;
    }

    /*
     * Release the runqueue without reopening the timer-interrupt window.
     * The old code restored IF here and disabled it again afterwards; a tick
     * in that gap could enter sched_switch() after rq->curr had changed but
     * before the CPU had changed stacks.
     */
    spin_unlock_irqrestore(&rq->lock, entry_rflags & ~(1ULL << 9));

    /*
     * arch_prctl keeps the software values authoritative, so avoid two
     * serializing RDMSRs and skip WRMSRs whose values do not change.
     *
     * In kernel mode IA32_GS_BASE is the per-CPU base, so the user GS base is
     * parked in KERNEL_GS_BASE; the return-to-user swapgs restores it.
     */
    if (!prev || prev->thread.fs_base != next->thread.fs_base) wrmsr(0xC0000100, next->thread.fs_base);
    if (!prev || prev->thread.gs_base != next->thread.gs_base) set_user_gs_base(next->thread.gs_base);
    ptrace_arch_switch(prev, next);

    fpu_switch(prev, next);
    context_switch(&prev->context, &next->context, &prev->on_cpu);

    /* Restore the IRQ state of the call site when this task is scheduled in. */
    if (entry_rflags & (1ULL << 9))
        enable_intr();
    else
        disable_intr();
}

/* Yield the current task to the scheduler */
void sched_yield(void)
{
    sched_switch(true);
}

/* sched_start - launch the scheduler on the BSP */
void sched_start(void)
{
    disable_intr();
    local_current()->state = TASK_IDLE;
    scheduler.started      = 1;
    enable_intr();
    sched_yield();

    /* swapper/0 resumed - enter idle loop */
    for (;;) {
        enable_intr();
        __asm__ volatile("hlt");
        disable_intr();
        sched_yield();
    }
}

/* task_sleep_ticks - voluntary sleep for N ticks */
void task_sleep_ticks(uint64_t ticks)
{
    if (!ticks) {
        sched_yield();
        return;
    }

    disable_intr();
    spin_lock(&scheduler.lock);

    eevdf_rq_t *rq   = local_rq();
    task_t     *curr = __atomic_load_n(&rq->curr, __ATOMIC_RELAXED);

    /* Save lag before sleeping (rq state under rq->lock). */
    spin_lock(&rq->lock);
    curr->vlag = (int64_t)(avg_vruntime(rq) - curr->vruntime);
    spin_unlock(&rq->lock);
    sleep_task(curr, scheduler.ticks + ticks);

    spin_unlock(&scheduler.lock);
    sched_yield();
    enable_intr();
}

/* task_block - block the current task */
void task_block(void)
{
    disable_intr();
    spin_lock(&scheduler.lock);

    eevdf_rq_t *rq   = local_rq();
    task_t     *curr = __atomic_load_n(&rq->curr, __ATOMIC_RELAXED);

    spin_lock(&rq->lock);
    curr->vlag  = (int64_t)(avg_vruntime(rq) - curr->vruntime);
    curr->state = TASK_BLOCKED;
    spin_unlock(&rq->lock);

    spin_unlock(&scheduler.lock);
    sched_yield();
    enable_intr();
}

/* task_wakeup - wake a blocked or sleeping task */
int task_wakeup(task_t *task)
{
    if (!task) return 1;

    spin_lock(&scheduler.lock);
    if ((task->state == TASK_BLOCKED || task->state == TASK_SLEEPING) && !__atomic_load_n(&task->on_cpu, __ATOMIC_ACQUIRE)) {
        uint32_t old_cpu = task->cpu_id;
        uint32_t new_cpu = select_wakeup_cpu_locked(task, false);
        if (new_cpu != old_cpu) {
            task->last_cpu          = old_cpu;
            task->last_migrate_tick = scheduler.ticks;
            task->migration_count++;
        }
        task->cpu_id         = new_cpu;
        task->last_wake_tick = scheduler.ticks;
        __atomic_add_fetch(&cpu_rqs[new_cpu].nr_wakeups, 1, __ATOMIC_RELAXED);
    }
    if (task->wait_queue) {
        finish_wait_locked(task, TASK_WAKE_NORMAL);
    } else if (task->state == TASK_SLEEPING) {
        wake_task_locked(task, 1);
    } else {
        wake_task_locked(task, 0);
    }
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);
    return 0;
}

/* Resume a stopped task, reporting whether it was actually continued */
int task_continue(task_t *task)
{
    if (!task) return 1;

    spin_lock(&scheduler.lock);
    bool continued = task->state == TASK_STOPPED;
    if (continued) {
        if (__atomic_load_n(&task->on_cpu, __ATOMIC_ACQUIRE))
            task->state = TASK_RUNNING;
        else
            enqueue_task(task);
    }
    spin_unlock(&scheduler.lock);
    if (continued) request_task_cpu(task);
    return continued ? 0 : 1;
}

/* Stop a task without leaving a READY entity behind in its runqueue tree. */
int task_stop(task_t *task)
{
    if (!task) return 1;

    spin_lock(&scheduler.lock);
    bool stopped = task->state == TASK_READY || task->state == TASK_RUNNING;
    if (stopped) {
        eevdf_rq_t *rq = task->cpu_id < cpu_scheduler_count ? &cpu_rqs[task->cpu_id] : &cpu_rqs[0];
        spin_lock(&rq->lock);
        if (task->state == TASK_READY) dequeue_entity(rq, task);
        task->state = TASK_STOPPED;
        spin_unlock(&rq->lock);
    }
    spin_unlock(&scheduler.lock);
    if (stopped) request_task_cpu(task);
    return stopped ? 0 : 1;
}

/* Wait queue implementation */

/* Initialize a wait queue */
void wait_queue_init(wait_queue_t *queue)
{
    if (!queue) return;

    ilist_init(&queue->tasks);
    queue->lock.lock   = 0;
    queue->lock.rflags = 0;
}

/* Block the current task on a wait queue, preparing and sleeping in one call */
void wait_queue_wait(wait_queue_t *queue)
{
    if (!queue) {
        task_block();
        return;
    }

    wait_queue_prepare(queue);
    wait_queue_sleep();
}

/*
 * Prepare the current task to wait on a queue.  The caller must hold any
 * external lock that guards the condition.  After calling prepare, release
 * the external lock, then call wait_queue_sleep() to actually block.
 */
void wait_queue_prepare(wait_queue_t *queue)
{
    if (!queue) {
        task_block();
        return;
    }

    spin_lock(&scheduler.lock);

    eevdf_rq_t *rq   = local_rq();
    task_t     *curr = __atomic_load_n(&rq->curr, __ATOMIC_RELAXED);

    spin_lock(&rq->lock);
    curr->vlag = (int64_t)(avg_vruntime(rq) - curr->vruntime);
    spin_unlock(&rq->lock);
    curr->wake_tick   = 0;
    curr->wait_queue  = queue;
    curr->wake_reason = TASK_WAKE_NONE;
    ilist_insert_before(&queue->tasks, &curr->sched_node);

    spin_unlock(&scheduler.lock);
}

/*
 * Commit a prepared wait: context-switch away from the current task.
 * Must be paired with a preceding wait_queue_prepare().
 */
void wait_queue_sleep(void)
{
    task_t *curr  = local_current();
    int     sleep = 0;

    spin_lock(&scheduler.lock);
    if (curr->wait_queue && curr->wake_reason == TASK_WAKE_NONE) {
        curr->state = TASK_BLOCKED;
        sleep       = 1;
    }
    spin_unlock(&scheduler.lock);

    if (sleep) sched_yield();
}

/* Cancel the current task's two-phase wait before it becomes blocked. */
void wait_queue_cancel(wait_queue_t *queue)
{
    if (!queue) return;

    task_t *curr = local_current();
    spin_lock(&scheduler.lock);
    if (curr->wait_queue == queue && curr->wake_reason == TASK_WAKE_NONE) finish_wait_locked(curr, TASK_WAKE_NORMAL);
    spin_unlock(&scheduler.lock);
}

/*
 * Two-phase wait with timeout.
 * Must be paired with a preceding wait_queue_prepare().
 * The caller must hold the external lock during prepare() and release
 * it before calling this function.
 *
 * Returns 0 if woken normally, -ETIMEDOUT if the deadline expired.
 *
 * NOTE: the caller must re-check the condition under the external
 * lock after this function returns, because the wakeup might be
 * spurious (e.g., the deadline expired but the condition was already
 * satisfied).
 */
int wait_queue_wait_timed(wait_queue_t *queue, uint64_t deadline_ticks)
{
    if (!queue) {
        task_block();
        return 0;
    }

    /*
     * The task is already in the wait queue (via wait_queue_prepare).
     * Now also add it to the timer queue so the scheduler tick can
     * wake it when the deadline expires.
     */
    task_t *curr  = local_current();
    int     sleep = 0;

    spin_lock(&scheduler.lock);
    if (curr->wait_queue == queue && curr->wake_reason == TASK_WAKE_NONE) {
        if (deadline_ticks <= scheduler.ticks) {
            finish_wait_locked(curr, TASK_WAKE_TIMEOUT);
        } else {
            curr->wake_tick = deadline_ticks;
            ilist_insert_before(&scheduler.timer_queue, &curr->timer_node);
            curr->state = TASK_BLOCKED;
            sleep       = 1;
        }
    }
    spin_unlock(&scheduler.lock);

    if (sleep) sched_yield();

    return curr->wake_reason == TASK_WAKE_TIMEOUT ? -ETIMEDOUT : 0;
}

/* Wake one task from the queue, returning it */
task_t *wait_queue_wake_one(wait_queue_t *queue)
{
    if (!queue) return NULL;

    spin_lock(&scheduler.lock);
    if (ilist_is_empty(&queue->tasks)) {
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    ilist_node_t *node = queue->tasks.next;
    task_t       *task = sched_node_to_task(node);

    if (task->state == TASK_BLOCKED && !__atomic_load_n(&task->on_cpu, __ATOMIC_ACQUIRE)) {
        uint32_t old_cpu = task->cpu_id;
        uint32_t new_cpu = select_wakeup_cpu_locked(task, false);
        if (new_cpu != old_cpu) {
            task->last_cpu          = old_cpu;
            task->last_migrate_tick = scheduler.ticks;
            task->migration_count++;
        }
        task->cpu_id         = new_cpu;
        task->last_wake_tick = scheduler.ticks;
        __atomic_add_fetch(&cpu_rqs[new_cpu].nr_wakeups, 1, __ATOMIC_RELAXED);
    }
    finish_wait_locked(task, TASK_WAKE_NORMAL);
    spin_unlock(&scheduler.lock);
    request_task_cpu(task);
    return task;
}

/* Wake one task, using a Linux WF_SYNC-like placement hint. */
task_t *wait_queue_wake_one_sync(wait_queue_t *queue)
{
    if (!queue) return NULL;

    uint32_t this_cpu = get_current_cpu_id();
    spin_lock(&scheduler.lock);
    if (ilist_is_empty(&queue->tasks)) {
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    ilist_node_t *node = queue->tasks.next;
    task_t       *task = sched_node_to_task(node);

    /*
     * WF_SYNC is only a placement hint.  Keep an idle previous CPU whenever
     * possible so producer and consumer can run in parallel; migrate only if
     * measured runqueue pressure says another CPU is materially better.
     */
    if (task->state == TASK_BLOCKED && !__atomic_load_n(&task->on_cpu, __ATOMIC_ACQUIRE)) {
        uint32_t old_cpu = task->cpu_id;
        uint32_t new_cpu = select_wakeup_cpu_locked(task, true);
        if (new_cpu != old_cpu) {
            task->last_cpu          = old_cpu;
            task->last_migrate_tick = scheduler.ticks;
            task->migration_count++;
        }
        task->cpu_id         = new_cpu;
        task->last_wake_tick = scheduler.ticks;
        __atomic_add_fetch(&cpu_rqs[new_cpu].nr_wakeups, 1, __ATOMIC_RELAXED);
    }
    finish_wait_locked(task, TASK_WAKE_NORMAL);
    uint32_t target_cpu = task->cpu_id;
    spin_unlock(&scheduler.lock);

    if (target_cpu != this_cpu) request_task_cpu(task);
    return task;
}

/* Wake every task waiting on the queue */
uint64_t wait_queue_wake_all(wait_queue_t *queue)
{
    if (!queue) return 0;

    uint64_t count       = 0;
    uint64_t remote_cpus = 0;
    bool     broadcast   = false;
    uint32_t this_cpu    = get_current_cpu_id();

    spin_lock(&scheduler.lock);
    while (!ilist_is_empty(&queue->tasks)) {
        ilist_node_t *node = queue->tasks.next;
        task_t       *task = sched_node_to_task(node);

        finish_wait_locked(task, TASK_WAKE_NORMAL);
        if (task->cpu_id != this_cpu) {
            if (task->cpu_id < 64)
                remote_cpus |= 1ULL << task->cpu_id;
            else
                broadcast = true;
        }
        count++;
    }
    spin_unlock(&scheduler.lock);

    /*
     * A reschedule IPI can enter sched_yield() immediately.  Send it only
     * after dropping scheduler.lock so a remote CPU never spins in its IPI
     * handler on a lock held by the waking IRQ CPU.
     */
    if (scheduler.started) {
        if (broadcast) {
            send_ipi_all(IPI_RESCHEDULE);
        } else {
            for (uint32_t cpu = 0; cpu < 64; cpu++)
                if (remote_cpus & (1ULL << cpu)) send_ipi_cpu(cpu, IPI_RESCHEDULE);
        }
    }
    return count;
}

/* sched_tick - periodic tick accounting and preemption */
void sched_tick(bool user_mode)
{
    if (!scheduler.started || !cpu_rqs) return;

    uint32_t    cpu_id   = get_current_cpu_id();
    eevdf_rq_t *rq       = &cpu_rqs[cpu_id];
    task_t     *balanced = NULL;
    bool        preempt  = false;

    /*
     * Local runqueue accounting under this CPU's own rq lock.  Remote wakeups
     * and migrations also take the target rq lock, so the RB tree stays
     * consistent without a global lock on every tick.
     */
    spin_lock(&rq->lock);
    task_t *curr = rq->curr;

    /* Idle task: yield if there is real work */
    if (curr == rq->idle) {
        rq->idle_ticks++;
        preempt = __atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED) > 0;
    } else if (curr->state == TASK_RUNNING) {
        if (user_mode) {
            curr->user_ticks++;
            rq->user_ticks++;
        } else {
            curr->system_ticks++;
            rq->system_ticks++;
        }

        /* Advance vruntime by one tick and test the next eligible deadline. */
        curr->time_slice++;
        update_curr(rq, 1);
        update_deadline(rq, curr);
        if (curr->time_slice >= SCHED_MIN_GRANULARITY && __atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED) > 0) preempt = pick_eevdf(rq) != curr;
    }
    spin_unlock(&rq->lock);

    /* CPU 0 handles global tick, sleep queue, and load balancing under the global lock. */
    if (cpu_id == 0) {
        spin_lock(&scheduler.lock);
        scheduler.ticks++;
        wake_sleeping_tasks();
        if ((scheduler.ticks % SCHED_LOAD_BALANCE_INTERVAL) == 0) balanced = balance_ready_queues_locked();
        spin_unlock(&scheduler.lock);
    }
    request_task_cpu(balanced);

    /*
     * sched_tick() already charged this millisecond.  Reusing the ordinary
     * yield accounting here used to charge every timer preemption twice.
     */
    if (preempt) sched_switch(false);
}

/* sched_ticks - return the global tick count */
uint64_t sched_ticks(void)
{
    return scheduler.ticks;
}

/* task_exit - terminate the current task */
void task_exit(void)
{
    disable_intr();
    spin_lock(&scheduler.lock);

    task_t     *curr = local_current();
    eevdf_rq_t *rq   = local_rq();

    /*
     * A running task is not present in rq->timeline: it was removed when
     * it was selected.  Dequeuing it again corrupts the RB tree through
     * the stale links left in run_node and can make the zombie runnable.
     *
     * Keep rq->curr pointing at the real execution context until
     * sched_yield() saves that context and switches to the next task.
     */
    if (curr && curr != rq->idle) curr->state = TASK_ZOMBIE;

    spin_unlock(&scheduler.lock);

    cgroup_task_exit(curr);
    sched_yield();

    /* A zombie is never enqueued, so the switch above cannot return. */
    panic("sched: zombie task resumed after exit.");
}

/* current_task - return the task running on this CPU */
task_t *current_task(void)
{
    /* Pre-scheduler boot runs on swapper/0 with no per-CPU current yet. */
    if (!cpu_rqs) return &boot_task;
    return percpu_gs_current();
}
