/*
 *
 *      mutex.h
 *      Sleepable mutual exclusion
 *
 *      2026/9/19 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MUTEX_H_
#define INCLUDE_MUTEX_H_

#include <libs/std/stdbool.h>
#include <process/task.h>
#include <sync/spin_lock.h>

/* Mutual exclusion for process context: the holder may block on I/O, so the lock never masks interrupts.  Call mutex_init() before first use. */
typedef struct {
        spinlock_t   guard; // guards busy
        bool         busy;
        wait_queue_t wait;
} mutex_t;

/* Initialize a mutex */
void mutex_init(mutex_t *mutex);

/* Try to acquire a mutex without sleeping; 1 = acquired, 0 = held by another task */
int mutex_trylock(mutex_t *mutex);

/* Report whether a mutex is held, without acquiring it */
int mutex_is_locked(const mutex_t *mutex);

/* Acquire a mutex, sleeping while another task holds it */
void mutex_lock(mutex_t *mutex);

/* Release a mutex and wake every waiter */
void mutex_unlock(mutex_t *mutex);

#endif // INCLUDE_MUTEX_H_
