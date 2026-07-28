/*
 *
 *      spin_lock.h
 *      Spin lock header file
 *
 *      2025/7/12 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SPIN_LOCK_H_
#define INCLUDE_SPIN_LOCK_H_

#include <libs/std/stdint.h>

typedef struct {
        volatile uint64_t lock;   // lock state
        uint64_t          rflags; // compatibility storage for spin_lock()
} spinlock_t;

/* Lock while saving interrupt state in caller-owned storage. */
uint64_t spin_lock_irqsave(spinlock_t *lock);

/* Unlock and restore caller-owned interrupt state. */
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t rflags);

/* Lock a spinlock */
void spin_lock(spinlock_t *lock);

/* Unlock a spinlock */
void spin_unlock(spinlock_t *lock);

#endif // INCLUDE_SPIN_LOCK_H_
