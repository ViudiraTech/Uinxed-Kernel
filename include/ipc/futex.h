/*
 *
 *      futex.h
 *      Fast userspace mutex header file
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FUTEX_H_
#define INCLUDE_FUTEX_H_

#include <libs/std/stdint.h>

/* Futex operations */

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_FD              2
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_LOCK_PI         6
#define FUTEX_UNLOCK_PI       7
#define FUTEX_TRYLOCK_PI      8
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI  12
#define FUTEX_LOCK_PI2        13

#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

/* 64-bit mask/bitset support (used by classic bitsets and futex2 masks) */
#define FUTEX_BITSET_MATCH_ANY 0xffffffffffffffffULL

/* futex2 flags (Linux 6.7+ futex_wake / futex_wait / futex_requeue) */

#define FUTEX2_SIZE_U8   0x00
#define FUTEX2_SIZE_U16  0x01
#define FUTEX2_SIZE_U32  0x02
#define FUTEX2_SIZE_U64  0x03
#define FUTEX2_SIZE_MASK 0x03

#define FUTEX2_PRIVATE        FUTEX_PRIVATE_FLAG
#define FUTEX2_CLOCK_REALTIME 0x10

#define FUTEX2_VALID_MASK (FUTEX2_SIZE_MASK | FUTEX2_PRIVATE | FUTEX2_CLOCK_REALTIME)

/* The futex_waitv descriptor used by futex_requeue() (and futex_waitv). */
struct futex_waitv {
        uint64_t val;
        uint64_t uaddr;
        uint32_t flags;
        uint32_t __reserved;
};

/* Classic futex syscall: wait/wake/requeue on a userspace word. */
int64_t sys_futex(uint32_t *uaddr, int futex_op, uint32_t val, uint64_t timeout, uint32_t *uaddr2, uint32_t val3);

/* futex_waitv: block until one of a vector of 32-bit futexes is woken. */
int64_t sys_futex_waitv(uint64_t waiters, uint64_t nr_waiters, uint64_t flags, uint64_t timeout, uint64_t clockid, uint64_t reserved);

/* futex2 syscalls (Linux 6.7+): syscall numbers 454 / 455 / 456 */
int64_t sys_futex_wake(uint64_t uaddr, uint64_t mask, uint64_t nr, uint64_t flags, uint64_t a4, uint64_t a5);
int64_t sys_futex_wait(uint64_t uaddr, uint64_t val, uint64_t mask, uint64_t flags, uint64_t timeout, uint64_t clockid);
int64_t sys_futex_requeue(uint64_t waiters, uint64_t flags, uint64_t nr_wake, uint64_t nr_requeue, uint64_t a4, uint64_t a5);

/* Kernel-side wake operation, including clear_child_tid users. */
int futex_wake(uint32_t *uaddr, int nr_wake, uint64_t bitset);

/* Initialize the futex subsystem. */
void futex_init(void);

/* Weak default realtime-clock hook (overridable by the syscall clock layer). */
uint64_t futex_realtime_ticks(void);

#endif // INCLUDE_FUTEX_H_
