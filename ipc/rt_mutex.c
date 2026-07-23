/*
 *
 *      rt_mutex.c
 *      Real-time mutex with priority inheritance
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <ipc/futex.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/data/rbtree.h>
#include <libs/glist/intrusive_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/rt_mutex.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Helpers: convert weight → "priority" for rbtree ordering            */
/* ------------------------------------------------------------------ */

/*
 * PI waiters are ordered by weight (higher weight = higher priority).
 * The rbtree uses a "less" predicate such that the leftmost node is
 * the highest priority (highest weight) waiter.
 */
static int pi_waiter_less(const rb_node_t *a, const rb_node_t *b)
{
    task_t *ta = rb_entry(a, task_t, pi_node);
    task_t *tb = rb_entry(b, task_t, pi_node);

    if (ta->pi_weight != tb->pi_weight) return ta->pi_weight > tb->pi_weight;

    return ta->pid > tb->pid;
}

void pi_waiter_augment(rb_node_t *node, void *data)
{
    (void)data;
    (void)node;
}

/* ------------------------------------------------------------------ */
/*  Priority inheritance / donation                                    */
/* ------------------------------------------------------------------ */

/*
 * Determine the effective weight that should be donated to @owner
 * based on the highest-weight waiter in its pi_waiters tree.
 * Returns the original weight if no waiter has a higher weight.
 */
static uint32_t pi_effective_weight(task_t *owner)
{
    rt_mutex_t *mutex = owner->blocked_on;
    if (!mutex) return owner->base_weight;

    rb_node_t *leftmost = rb_first(&mutex->pi_waiters);
    if (!leftmost) return owner->base_weight;

    task_t  *top_waiter = rb_entry(leftmost, task_t, pi_node);
    uint32_t donated    = top_waiter->pi_weight;

    return donated > owner->base_weight ? donated : owner->base_weight;
}

/*
 * Re-evaluate priority inheritance for @owner.
 * If the highest-weight waiter has a higher weight than @owner's
 * current weight, boost @owner. Then propagate the chain upward.
 */
void pi_propagate_chain(task_t *owner)
{
    while (owner) {
        rt_mutex_t *mutex = owner->blocked_on;
        if (!mutex) {
            if (owner->weight != owner->base_weight) { owner->weight = owner->base_weight; }
            return;
        }

        uint32_t new_weight = pi_effective_weight(owner);

        if (new_weight != owner->weight) { owner->weight = new_weight; }

        owner = mutex->owner;
    }
}

/*
 * Remove a waiter from the pi_waiters tree of its blocked_on mutex,
 * then propagate the chain to re-evaluate priorities.
 */
void pi_waiter_remove(task_t *waiter)
{
    rt_mutex_t *mutex = waiter->blocked_on;
    if (!mutex) return;

    if (!rb_is_empty(&mutex->pi_waiters)) { rb_erase_augmented(&mutex->pi_waiters, &waiter->pi_node, pi_waiter_augment, NULL); }

    waiter->blocked_on = NULL;

    pi_propagate_chain(mutex->owner);
}

/*
 * Add a waiter to the pi_waiters tree of its blocked_on mutex,
 * then propagate the chain to donate priority if necessary.
 */
void pi_waiter_add(task_t *waiter, rt_mutex_t *mutex)
{
    waiter->blocked_on = mutex;

    rb_insert_augmented(&mutex->pi_waiters, &waiter->pi_node, pi_waiter_less, pi_waiter_augment, NULL);

    pi_propagate_chain(mutex->owner);
}

/* ------------------------------------------------------------------ */
/*  rt_mutex operations                                                 */
/* ------------------------------------------------------------------ */

void rt_mutex_init(rt_mutex_t *mutex, uint32_t *uaddr)
{
    memset(mutex, 0, sizeof(rt_mutex_t));
    mutex->owner      = NULL;
    mutex->uaddr      = uaddr;
    mutex->owner_died = 0;
    wait_queue_init(&mutex->wq);
    rb_init_root(&mutex->pi_waiters);
}

int rt_mutex_trylock(rt_mutex_t *mutex, task_t *self)
{
    if (!mutex || !self) return -EINVAL;
    if (mutex->owner) return -EAGAIN;

    mutex->owner      = self;
    mutex->owner_died = 0;
    self->pi_weight   = self->weight;
    self->base_weight = self->weight;

    return EOK;
}

int rt_mutex_lock(rt_mutex_t *mutex, task_t *self)
{
    if (!mutex || !self) return -EINVAL;

    for (;;) {
        if (!mutex->owner) {
            mutex->owner      = self;
            mutex->owner_died = 0;
            self->pi_weight   = self->weight;
            self->base_weight = self->weight;
            return EOK;
        }

        if (mutex->owner == self) return -EDEADLK;

        self->pi_weight   = self->weight;
        self->base_weight = self->weight;

        pi_waiter_add(self, mutex);

        wait_queue_prepare(&mutex->wq);

        if (!mutex->owner || mutex->owner == self) {
            wait_queue_wake_one(&mutex->wq);
            pi_waiter_remove(self);
            continue;
        }

        task_block();

        if (mutex->owner_died) {
            pi_waiter_remove(self);
            return -EOWNERDEAD;
        }
    }
}

int rt_mutex_unlock(rt_mutex_t *mutex, task_t *self)
{
    if (!mutex || !self) return -EINVAL;
    if (mutex->owner != self) return -EPERM;

    mutex->owner = NULL;

    pi_propagate_chain(self);

    task_t *next = rt_mutex_wake_top_waiter(mutex);
    if (next) {
        mutex->owner = next;
        pi_waiter_remove(next);

        uint32_t old_futex_val = 0;
        if (mutex->uaddr) { copy_from_user(&old_futex_val, mutex->uaddr, sizeof(old_futex_val)); }

        task_wakeup(next);

        uint32_t new_futex_val = (next->pid & FUTEX_TID_MASK);
        if (!ilist_is_empty(&mutex->wq.tasks)) { new_futex_val |= FUTEX_WAITERS; }
        if (mutex->uaddr) { copy_to_user(mutex->uaddr, &new_futex_val, sizeof(new_futex_val)); }
    }

    return EOK;
}

task_t *rt_mutex_wake_top_waiter(rt_mutex_t *mutex)
{
    if (!mutex) return NULL;

    rb_node_t *leftmost = rb_first(&mutex->pi_waiters);
    if (!leftmost) return NULL;

    task_t *top = rb_entry(leftmost, task_t, pi_node);

    rb_erase_augmented(&mutex->pi_waiters, leftmost, pi_waiter_augment, NULL);
    top->blocked_on = NULL;

    return top;
}
