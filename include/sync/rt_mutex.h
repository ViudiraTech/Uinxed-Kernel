/*
 *
 *      rt_mutex.h
 *      Real-time mutex with priority inheritance
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RT_MUTEX_H_
#define INCLUDE_RT_MUTEX_H_

#include <libs/list/intrusive_list.h>
#include <libs/std/stdint.h>
#include <libs/util/rbtree.h>
#include <process/task.h>
#include <sync/spin_lock.h>

/* Futex word flags for PI mutex */
#define FUTEX_WAITERS    0x80000000
#define FUTEX_OWNER_DIED 0x40000000
#define FUTEX_TID_MASK   0x3fffffff

typedef struct rt_mutex rt_mutex_t;

struct rt_mutex {
        /*
         * Protects owner, owner_died and pi_waiters.  Lock order:
         * futex bucket lock -> mutex->lock -> scheduler.lock.
         * Never acquire scheduler.lock while holding mutex->lock except
         * through task_wakeup(), which takes it after this lock is dropped
         * by all callers in this kernel.
         */
        spinlock_t   lock;
        task_t      *owner;
        rb_root_t    pi_waiters;
        wait_queue_t wq; // membership serialised by scheduler.lock only
        uint32_t    *uaddr;
        int          owner_died;

        /*
         * Reference count: the owning futex entry holds one reference and
         * every waiter holds one for the duration of its wait.  An rt_mutex
         * is freed by rt_mutex_unref() only when the count reaches zero, so
         * a waiter that has been removed from the wait queue can still
         * safely touch the mutex until it drops its own reference.  A waiter
         * must take its reference under the futex bucket lock (the same lock
         * that serialises entry cleanup); it may release it afterwards in
         * any context.
         */
        uint32_t refs;

        /* Intrusive link into the current owner's task->pi_owned list. */
        ilist_node_t pi_owned_node;
};

/* Initialize a rt_mutex */
void rt_mutex_init(rt_mutex_t *mutex, uint32_t *uaddr);

/* Take a reference on an rt_mutex.  Take it under the futex bucket lock. */
void rt_mutex_ref(rt_mutex_t *mutex);

/* Drop a reference; frees the mutex when the last reference is released. */
void rt_mutex_unref(rt_mutex_t *mutex);

/* Try to lock the rt_mutex (non-blocking) */
int rt_mutex_trylock(rt_mutex_t *mutex, task_t *self);

/* Lock the rt_mutex (may block with priority inheritance) */
int rt_mutex_lock(rt_mutex_t *mutex, task_t *self);

/* Unlock the rt_mutex */
int rt_mutex_unlock(rt_mutex_t *mutex, task_t *self);

/* Wake the highest-priority waiter (returns the woken task; mutex->lock held) */
task_t *rt_mutex_wake_top_waiter(rt_mutex_t *mutex);

/*
 * Change the owner of a PI mutex, keeping the owner's task->pi_owned list in
 * sync.  Caller holds mutex->lock.  If old_owner differs from new_owner the
 * mutex is removed from old_owner's list and linked into new_owner's (both
 * guarded by the owner's pi_owned_lock, lock order mutex->lock ->
 * pi_owned_lock).
 */
void pi_mutex_set_owner(rt_mutex_t *mutex, task_t *old_owner, task_t *new_owner);

/* Priority inheritance helpers (used by futex.c) */
void pi_waiter_add(task_t *waiter, rt_mutex_t *mutex);
void pi_waiter_remove(task_t *waiter);
void pi_propagate_chain(task_t *owner);
void pi_waiter_augment(rb_node_t *node, void *data);

#endif // INCLUDE_RT_MUTEX_H_
