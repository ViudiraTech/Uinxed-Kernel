/*
 *
 *      rt_mutex.c
 *      Real-time mutex with priority inheritance
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <ipc/futex.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/list/intrusive_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <libs/util/rbtree.h>
#include <mem/alloc.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/rt_mutex.h>
#include <sync/spin_lock.h>

/*
 * Locking protocol
 *
 * mutex->lock serialises owner, owner_died and the pi_waiters rbtree of one
 * rt_mutex.  Lock order (never nested the other way around):
 *
 *     futex bucket->lock  ->  mutex->lock  ->  scheduler.lock  ->  rq->lock
 *
 * pi_propagate_chain() walks the blocked_on chain across several mutexes; it
 * therefore runs OUTSIDE every mutex->lock and only performs relaxed atomic
 * reads/writes on chain pointers and weights, like Linux's RT-mutex PI walk.
 * The scheduler wait queue is guarded by scheduler.lock alone; it is taken
 * and released on its own, never while holding a mutex->lock.
 */

/* Helpers: convert weight -> "priority" for rbtree ordering */

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

/* rbtree augment callback (no per-node augmentation is maintained). */
void pi_waiter_augment(rb_node_t *node, void *data)
{
    (void)data;
    (void)node;
}

/* Priority inheritance / donation */

/*
 * Determine the effective weight that should be donated to @owner
 * based on the highest-weight waiter in its pi_waiters tree.
 * Returns the original weight if no waiter has a higher weight.
 */
static uint32_t pi_effective_weight(task_t *owner)
{
    rt_mutex_t *mutex = __atomic_load_n(&owner->blocked_on, __ATOMIC_RELAXED);
    if (!mutex) return __atomic_load_n(&owner->base_weight, __ATOMIC_RELAXED);

    rb_node_t *leftmost = rb_first(&mutex->pi_waiters);
    if (!leftmost) return __atomic_load_n(&owner->base_weight, __ATOMIC_RELAXED);

    task_t  *top_waiter = rb_entry(leftmost, task_t, pi_node);
    uint32_t donated    = __atomic_load_n(&top_waiter->pi_weight, __ATOMIC_RELAXED);
    uint32_t base       = __atomic_load_n(&owner->base_weight, __ATOMIC_RELAXED);

    return donated > base ? donated : base;
}

/*
 * Re-evaluate priority inheritance for @owner.
 * If the highest-weight waiter has a higher weight than @owner's
 * current weight, boost @owner. Then propagate the chain upward.
 */
void pi_propagate_chain(task_t *owner)
{
    while (owner) {
        rt_mutex_t *mutex = __atomic_load_n(&owner->blocked_on, __ATOMIC_RELAXED);
        if (!mutex) {
            uint32_t base = __atomic_load_n(&owner->base_weight, __ATOMIC_RELAXED);
            if (__atomic_load_n(&owner->weight, __ATOMIC_RELAXED) != base) __atomic_store_n(&owner->weight, base, __ATOMIC_RELAXED);
            return;
        }

        uint32_t new_weight = pi_effective_weight(owner);

        if (__atomic_load_n(&owner->weight, __ATOMIC_RELAXED) != new_weight) __atomic_store_n(&owner->weight, new_weight, __ATOMIC_RELAXED);

        owner = __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED);
    }
}

/*
 * Remove a waiter from the pi_waiters tree of its blocked_on mutex,
 * then propagate the chain to re-evaluate priorities.
 *
 * Safe to call when an unlock already popped the waiter: blocked_on is
 * NULL then and nothing is erased.  Callers mutating a specific mutex's
 * tree hold that mutex->lock.
 */
void pi_waiter_remove(task_t *waiter)
{
    rt_mutex_t *mutex = waiter->blocked_on;

    if (!mutex) return;
    if (!rb_is_empty(&mutex->pi_waiters)) rb_erase_augmented(&mutex->pi_waiters, &waiter->pi_node, pi_waiter_augment, NULL);

    waiter->blocked_on = NULL;
    pi_propagate_chain(mutex->owner);
}

/*
 * Add a waiter to the pi_waiters tree of its blocked_on mutex,
 * then propagate the chain to donate priority if necessary.
 * Caller must hold mutex->lock.
 */
void pi_waiter_add(task_t *waiter, rt_mutex_t *mutex)
{
    waiter->blocked_on = mutex;
    rb_insert_augmented(&mutex->pi_waiters, &waiter->pi_node, pi_waiter_less, pi_waiter_augment, NULL);
    pi_propagate_chain(mutex->owner);
}

/* Initialize an rt_mutex, binding it to a futex word. */
void rt_mutex_init(rt_mutex_t *mutex, uint32_t *uaddr)
{
    memset(mutex, 0, sizeof(rt_mutex_t));
    mutex->owner       = NULL;
    mutex->uaddr       = uaddr;
    mutex->owner_died  = 0;
    mutex->lock.lock   = 0;
    mutex->lock.rflags = 0;
    mutex->refs        = 1; // the owning futex entry holds the initial reference
    wait_queue_init(&mutex->wq);
    rb_init_root(&mutex->pi_waiters);
    ilist_init(&mutex->pi_owned_node);
}

/*
 * Change the owner of a PI mutex, keeping the owner's task->pi_owned list in
 * sync (futex_pi_owner_exit() walks it instead of the global futex hash).
 * Caller holds mutex->lock.  Lock order: mutex->lock -> owner->pi_owned_lock.
 */
void pi_mutex_set_owner(rt_mutex_t *mutex, task_t *old_owner, task_t *new_owner)
{
    /*
     * No ownership change (covers old == new != NULL, e.g. the contended-lock
     * path re-confirming the word's tid is the current owner, and old == new
     * == NULL): leave the owned list untouched.  Re-inserting the node into
     * the same owner's list would corrupt it.
     */
    if (old_owner == new_owner) return;
    if (old_owner) {
        spin_lock(&old_owner->pi_owned_lock);
        ilist_remove(&mutex->pi_owned_node);
        spin_unlock(&old_owner->pi_owned_lock);
    }

    mutex->owner = new_owner;

    if (new_owner) {
        spin_lock(&new_owner->pi_owned_lock);
        ilist_insert_before(&new_owner->pi_owned, &mutex->pi_owned_node);
        spin_unlock(&new_owner->pi_owned_lock);
    }
}

/*
 * Take a reference on an rt_mutex.  Must be taken under the futex bucket
 * lock so it cannot race entry cleanup, which drops the entry's own ref.
 */
void rt_mutex_ref(rt_mutex_t *mutex)
{
    __atomic_add_fetch(&mutex->refs, 1, __ATOMIC_RELAXED);
}

/*
 * Drop a reference; the last reference frees the mutex.  Safe in any
 * context: the atomic transition to zero is observed by exactly one caller,
 * which must have finished using the mutex.
 */
void rt_mutex_unref(rt_mutex_t *mutex)
{
    if (__atomic_sub_fetch(&mutex->refs, 1, __ATOMIC_RELEASE) == 0) {
        /*
         * Destroyed: release the owner reference the mutex held (defensive;
         * an owned mutex is never reported empty by futex_entry_empty, so
         * the owner should already have released it).
         */
        task_t *owner = mutex->owner;
        if (owner) task_put(owner);
        free(mutex);
    }
}

/* Try to acquire the mutex without blocking. */
int rt_mutex_trylock(rt_mutex_t *mutex, task_t *self)
{
    if (!mutex || !self) return -EINVAL;
    spin_lock(&mutex->lock);

    if (mutex->owner) {
        spin_unlock(&mutex->lock);
        return -EAGAIN;
    }
    task_ref(self); // mutex holds an owner reference
    pi_mutex_set_owner(mutex, mutex->owner, self);
    mutex->owner_died = 0;
    self->base_weight = self->weight;
    self->pi_weight   = self->weight;
    spin_unlock(&mutex->lock);

    return EOK;
}

/* Acquire the mutex, blocking with priority inheritance on contention. */
int rt_mutex_lock(rt_mutex_t *mutex, task_t *self)
{
    if (!mutex || !self) return -EINVAL;
    bool waited = false;

    for (;;) {
        spin_lock(&mutex->lock);
        if (!mutex->owner) {
            task_ref(self); // mutex holds an owner reference
            pi_mutex_set_owner(mutex, mutex->owner, self);
            mutex->owner_died = 0;
            self->base_weight = self->weight;
            self->pi_weight   = self->weight;
            spin_unlock(&mutex->lock);
            return EOK;
        }

        if (mutex->owner == self) {
            /*
             * Seeing ourselves as owner is either a genuine recursive lock
             * (-EDEADLK, before we ever waited) or a hand-off completed by
             * rt_mutex_unlock() picking us as top waiter - the latter is
             * success.
             */
            int ret = waited ? EOK : -EDEADLK;
            spin_unlock(&mutex->lock);
            return ret;
        }

        /*
         * Contended: queue as PI waiter unless an earlier attempt left us
         * queued (foreign wake-up retry keeps the existing node).
         */
        self->base_weight = self->weight;
        self->pi_weight   = self->weight;

        if (self->blocked_on != mutex) pi_waiter_add(self, mutex);

        spin_unlock(&mutex->lock);
        wait_queue_prepare(&mutex->wq);

        /*
         * Re-check ownership after linking into the wait queue but before
         * committing to sleep.  A concurrent release must not leave us
         * sleeping behind a condition that already became true.
         */
        spin_lock(&mutex->lock);
        bool ready = !mutex->owner || mutex->owner == self;
        spin_unlock(&mutex->lock);

        if (ready) {
            /*
             * Withdraw OUR OWN prepared entry instead of sleeping on it.
             * Never wake_one(): waking some other waiter would leave this
             * sched_node behind in the queue and corrupt the next prepare.
             * The cancel removes us from whichever queue currently holds
             * the node, so even a concurrent requeue cannot strand it.
             */
            wait_queue_cancel(&mutex->wq);
            spin_lock(&mutex->lock);

            /* No-op when an unlock already popped us (blocked_on == NULL). */
            pi_waiter_remove(self);
            spin_unlock(&mutex->lock);
            continue;
        }

        task_block();
        waited = true;

        spin_lock(&mutex->lock);
        bool handed       = mutex->owner == self;
        bool died         = mutex->owner_died && !handed && mutex->owner != NULL;
        bool still_queued = self->blocked_on == mutex;

        if ((handed || died) && still_queued) pi_waiter_remove(self);
        spin_unlock(&mutex->lock);
        if (died) return -EOWNERDEAD;

        /*
         * Handed to us: loop and take the owner==self branch with
         * waited == true.  Any other wake-up: either still queued in the PI
         * tree (retry reuses the same node) or already popped (retry adds a
         * fresh node); both are consistent states.
         */
    }
}

/* Release the mutex, waking the highest-priority waiter as the new owner. */
int rt_mutex_unlock(rt_mutex_t *mutex, task_t *self)
{
    if (!mutex || !self) return -EINVAL;
    spin_lock(&mutex->lock);
    if (mutex->owner != self) {
        spin_unlock(&mutex->lock);
        return -EPERM;
    }

    /* The mutex releases its owner reference on self. */
    task_put(self);
    pi_mutex_set_owner(mutex, self, NULL);
    task_t *next = rt_mutex_wake_top_waiter(mutex);
    if (next) {
        /* Optimistic hand-off: @next owns the mutex from this point on. */
        task_ref(next); // mutex holds an owner reference on @next
        pi_mutex_set_owner(mutex, NULL, next);
    }

    spin_unlock(&mutex->lock);
    pi_propagate_chain(self);

    if (next) {
        uint32_t new_futex_val = (next->pid & FUTEX_TID_MASK);
        spin_lock(&scheduler.lock);
        bool has_waiters = !ilist_is_empty(&mutex->wq.tasks);
        spin_unlock(&scheduler.lock);
        if (has_waiters) new_futex_val |= FUTEX_WAITERS;
        if (mutex->uaddr && copy_to_user(mutex->uaddr, &new_futex_val, sizeof(new_futex_val))) {
            plogk("rt_mutex: copy_to_user failed for uaddr %p, waiter %llu may spin.\n", (void *)mutex->uaddr, (unsigned long long)next->pid);

            /* Still wake next owner but report fault to caller */
            task_wakeup(next);
            return -EFAULT;
        }
        task_wakeup(next);
    }
    return EOK;
}

/*
 * Remove and return the highest-priority waiter from the PI tree.
 * Caller must hold mutex->lock.
 */
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
