/*
 *
 *      spin_lock.c
 *      Spin lock
 *
 *      2025/7/12 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <spin_lock.h>

/* Lock a spinlock */
uint64_t spin_lock(spinlock_t *lock)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=rm"(flags)::"memory");

    while (1) {
        uint64_t desired = 1;
        __asm__ volatile("lock xchg %[desired], %[lock]" : [lock] "+m"(lock->lock), [desired] "+r"(desired)::"memory");

        if (!desired) break;

        while (lock->lock) { __asm__ volatile("pause"); }
    }
    return flags;
}

/* Unlock a spinlock */
void spin_unlock(spinlock_t *lock, uint64_t flags)
{
    __asm__ volatile("movq $0, %0" : "+m"(lock->lock)::"memory");
    __asm__ volatile("push %0; popfq" ::"rm"(flags) : "memory");
}
