/*
 *
 *      mutex.c
 *      Sleepable mutual exclusion
 *
 *      2026/9/19 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <process/sched.h>
#include <sync/mutex.h>

/* Initialize a mutex */
void mutex_init(mutex_t *mutex)
{
    mutex->guard.lock   = 0;
    mutex->guard.rflags = 0;
    mutex->busy         = false;
    wait_queue_init(&mutex->wait);
}

/* Try to acquire a mutex without sleeping; returns 1 on success */
int mutex_trylock(mutex_t *mutex)
{
    int      acquired = 0;
    uint64_t rflags   = spin_lock_irqsave(&mutex->guard);

    if (!mutex->busy) {
        mutex->busy = true;
        acquired    = 1;
    }
    spin_unlock_irqrestore(&mutex->guard, rflags);
    return acquired;
}

/* Report whether a mutex is held without acquiring it */
int mutex_is_locked(const mutex_t *mutex)
{
    return __atomic_load_n(&mutex->busy, __ATOMIC_ACQUIRE);
}

/* Acquire a mutex, sleeping while another task holds it */
void mutex_lock(mutex_t *mutex)
{
    for (;;) {
        spin_lock(&mutex->guard);
        if (!mutex->busy) {
            mutex->busy = true;
            spin_unlock(&mutex->guard);
            return;
        }

        /* Nothing can be woken before sched_start(): spin instead of sleeping forever. */
        if (__atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE)) {
            wait_queue_prepare(&mutex->wait);
            spin_unlock(&mutex->guard);
            wait_queue_sleep();
            continue;
        }
        spin_unlock(&mutex->guard);
        cpu_relax();
    }
}

/* Release a mutex and wake every waiter */
void mutex_unlock(mutex_t *mutex)
{
    spin_lock(&mutex->guard);
    mutex->busy = false;

    /* Waiters link into the queue under guard, so an empty queue here cannot miss a sleeper. */
    bool wake = !ilist_is_empty(&mutex->wait.tasks);
    spin_unlock(&mutex->guard);
    if (wake) wait_queue_wake_all(&mutex->wait);
}
