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
    while (1) {
        uint64_t desired = 1;
        __asm__ volatile("lock xchg %[desired], %[lock];" : [lock] "+m"(lock->lock), [desired] "+r"(desired)::"memory");
        if (!desired) break;
        __asm__ volatile("pause");
    }
    return rflags;
}

/* Unlock and restore caller-owned interrupt state. */
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t rflags)
{
    __asm__ volatile("movq $0, %0" : "=m"(lock->lock)::"memory");
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
