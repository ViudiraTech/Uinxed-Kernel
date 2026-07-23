/*
 *
 *      rt_mutex.h
 *      Real-time mutex with priority inheritance
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RT_MUTEX_H_
#define INCLUDE_RT_MUTEX_H_

#include <libs/data/rbtree.h>
#include <libs/glist/intrusive_list.h>
#include <libs/std/stdint.h>
#include <proc/task.h>
#include <sync/spin_lock.h>

/* Futex word flags for PI mutex (Linux-compatible) */
#define FUTEX_WAITERS     0x80000000
#define FUTEX_OWNER_DIED  0x40000000
#define FUTEX_TID_MASK    0x3fffffff

typedef struct rt_mutex rt_mutex_t;

struct rt_mutex {
    spinlock_t     lock;
    task_t        *owner;
    rb_root_t      pi_waiters;
    wait_queue_t   wq;
    uint32_t      *uaddr;
    int            owner_died;
};

/* Initialize a rt_mutex */
void rt_mutex_init(rt_mutex_t *mutex, uint32_t *uaddr);

/* Try to lock the rt_mutex (non-blocking) */
int rt_mutex_trylock(rt_mutex_t *mutex, task_t *self);

/* Lock the rt_mutex (may block with priority inheritance) */
int rt_mutex_lock(rt_mutex_t *mutex, task_t *self);

/* Unlock the rt_mutex */
int rt_mutex_unlock(rt_mutex_t *mutex, task_t *self);

/* Wake the highest-priority waiter (returns the woken task) */
task_t *rt_mutex_wake_top_waiter(rt_mutex_t *mutex);

/* Priority inheritance helpers (used by futex.c) */
void pi_waiter_add(task_t *waiter, rt_mutex_t *mutex);
void pi_waiter_remove(task_t *waiter);
void pi_propagate_chain(task_t *owner);
void pi_waiter_augment(rb_node_t *node, void *data);

#endif /* INCLUDE_RT_MUTEX_H_ */
