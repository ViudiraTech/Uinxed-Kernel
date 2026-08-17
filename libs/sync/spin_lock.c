/*
 *
 *      spin_lock.c
 *      Spin lock
 *
 *      2025/7/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <sync/spin_lock.h>

/* Lock while returning interrupt state to the caller. */
uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags)::"memory");
    /*
     * Test-and-test-and-set: spin on shared reads and issue the expensive
     * locked exchange only when the cacheline appears free.  This avoids
     * turning scheduler/wait-queue contention into a stream of cacheline
     * invalidations across every CPU.
     */
    for (;;) {
        while (__atomic_load_n(&lock->lock, __ATOMIC_RELAXED)) __asm__ volatile("pause");
        if (!__atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE)) break;
    }
    return rflags;
}

/* Unlock and restore caller-owned interrupt state. */
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t rflags)
{
    __atomic_store_n(&lock->lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile("push %0; popfq" : : "r"(rflags) : "memory", "cc");
}

/* Lock a spinlock */
void spin_lock(spinlock_t *lock)
{
    uint64_t rflags = spin_lock_irqsave(lock);

    /* Waiters cannot overwrite compatibility state before owning the lock. */
    lock->rflags = rflags;
}

/* Unlock a spinlock */
void spin_unlock(spinlock_t *lock)
{
    uint64_t rflags = lock->rflags;

    spin_unlock_irqrestore(lock, rflags);
}
