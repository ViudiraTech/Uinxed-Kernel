/*
 *
 *      spin_lock.h
 *      Spin lock header file
 *
 *      2025/7/12 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SPIN_LOCK_H_
#define INCLUDE_SPIN_LOCK_H_

#include <stdint.h>

typedef struct {
        volatile uint32_t lock;
} spinlock_t;

/* Lock a spinlock */
uint64_t spin_lock(spinlock_t *lock);

/* Unlock a spinlock */
void spin_unlock(spinlock_t *lock, uint64_t flags);

#endif // INCLUDE_SPIN_LOCK_H_
