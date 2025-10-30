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

#include "stdint.h"

typedef struct {
        volatile uint64_t lock;   // lock state
        uint64_t          rflags; // stored rflags
} spinlock_t;

/* Lock a spinlock */
void spin_lock(spinlock_t *lock);

/* Unlock a spinlock */
void spin_unlock(spinlock_t *lock);

#endif // INCLUDE_SPIN_LOCK_H_
