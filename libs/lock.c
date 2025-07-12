/*
 *
 *      lock.c
 *      Synchronous lock
 *
 *      2025/7/12 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "lock.h"

/* Lock a spinlock */
void spin_lock(spinlock_t *lock)
{
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(lock->rflags));
    while (1) {
        uint64_t desired = 1;
        asm volatile("lock xchg %[desired], %[lock];" : [lock] "+m"(lock->lock), [desired] "+r"(desired) : : "memory");
        if (desired == 0) { break; }
        __asm__ volatile("pause");
    }
}

/* Unlock a spinlock */
void spin_unlock(spinlock_t *lock)
{
    lock->lock = 0;
    asm volatile("push %0; popfq" : : "r"(lock->rflags));
}
