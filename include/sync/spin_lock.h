/*
 *
 *      spin_lock.h
 *      Spin lock header file
 *
 *      2025/7/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SPIN_LOCK_H_
#define INCLUDE_SPIN_LOCK_H_

#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

typedef struct {
        volatile uint64_t lock;   // lock state
        uint64_t          rflags; // compatibility storage for spin_lock()
} spinlock_t;

/* Lock that never touches interrupt state; the caller must prove no interrupt handler on this CPU takes it */
typedef struct {
        volatile uint32_t lock; // lock state
} raw_spinlock_t;

/* Lock while saving interrupt state in caller-owned storage. */
uint64_t spin_lock_irqsave(spinlock_t *lock);

/* Unlock and restore caller-owned interrupt state. */
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t rflags);

/* Lock a spinlock */
void spin_lock(spinlock_t *lock);

/* Try to lock a spinlock without spinning; 1 = acquired, 0 = contended (spin_unlock releases it) */
int spin_trylock(spinlock_t *lock);

/* Unlock a spinlock */
void spin_unlock(spinlock_t *lock);

/* Lock a raw spinlock */
void raw_spin_lock(raw_spinlock_t *lock);

/* Try to lock a raw spinlock without spinning; 1 = acquired, 0 = contended (raw_spin_unlock releases it) */
int raw_spin_trylock(raw_spinlock_t *lock);

/* Unlock a raw spinlock */
void raw_spin_unlock(raw_spinlock_t *lock);

/* Spin, never sleeping, until a flag cleared by another CPU is false */
void spin_until_flag_clear(const volatile bool *flag);

/* Spin, never sleeping, until a counter published under guard reads zero */
void spin_until_zero(const volatile uint32_t *counter, spinlock_t *guard);

#endif // INCLUDE_SPIN_LOCK_H_
