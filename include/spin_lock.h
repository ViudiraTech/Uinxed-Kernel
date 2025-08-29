/*
 *
 *      spin_lock.h
 *      Spin lock header file
 *
 *      2025/7/12 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SPIN_LOCK_H_
#define INCLUDE_SPIN_LOCK_H_

#include "stdint.h"

typedef struct {
  volatile uint64_t lock; // lock state
  uint64_t rflags;        // stored rflags
} spinlock_t;

/* Lock a spinlock */
void spin_lock(spinlock_t *lock);

/* Unlock a spinlock */
void spin_unlock(spinlock_t *lock);

#endif // INCLUDE_SPIN_LOCK_H_
