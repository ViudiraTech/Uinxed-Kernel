/*
 *
 *      spin_lock.c
 *      Spin lock
 *
 *      2025/7/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <sync/spin_lock.h>

/* Lock while returning interrupt state to the caller. */
uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    uint64_t rflags;
    rflags = get_rflags();
    disable_intr();

    /*
     * Test-and-test-and-set: spin on shared reads and issue the expensive
     * locked exchange only when the cacheline appears free.  This avoids
     * turning scheduler/wait-queue contention into a stream of cacheline
     * invalidations across every CPU.
     */
    for (;;) {
        while (__atomic_load_n(&lock->lock, __ATOMIC_RELAXED)) cpu_relax();
        if (!__atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE)) break;
    }
    return rflags;
}

/* Try to lock a spinlock without spinning; returns 1 on success */
int spin_trylock(spinlock_t *lock)
{
    uint64_t rflags;
    rflags = get_rflags();
    disable_intr();

    if (__atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE)) {
        /* Contended: restore the interrupt state just cleared. */
        __asm__ volatile("push %0; popfq" : : "r"(rflags) : "memory", "cc");
        return 0;
    }

    /* Waiters cannot overwrite compatibility state before owning the lock. */
    lock->rflags = rflags;
    return 1;
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

/* Lock a raw spinlock */
void raw_spin_lock(raw_spinlock_t *lock)
{
    /* Test-and-test-and-set: share the cacheline while the lock looks busy. */
    for (;;) {
        while (__atomic_load_n(&lock->lock, __ATOMIC_RELAXED)) cpu_relax();
        if (!__atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE)) return;
    }
}

/* Try to lock a raw spinlock without spinning; returns 1 on success */
int raw_spin_trylock(raw_spinlock_t *lock)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&lock->lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* Unlock a raw spinlock */
void raw_spin_unlock(raw_spinlock_t *lock)
{
    __atomic_store_n(&lock->lock, 0, __ATOMIC_RELEASE);
}

/* Spin until a flag cleared by another CPU is false */
void spin_until_flag_clear(const volatile bool *flag)
{
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE)) cpu_relax();
}

/* Spin until a counter that is published under guard reads zero */
void spin_until_zero(const volatile uint32_t *counter, spinlock_t *guard)
{
    for (;;) {
        uint64_t rflags = spin_lock_irqsave(guard);
        uint32_t value  = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
        spin_unlock_irqrestore(guard, rflags);
        if (!value) return;
        cpu_relax();
    }
}
