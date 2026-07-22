/*
 *
 *      futex.h
 *      Fast userspace mutex header file
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FUTEX_H_
#define INCLUDE_FUTEX_H_

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Futex operations                                                   */
/* ------------------------------------------------------------------ */

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_FD             2
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE    4
#define FUTEX_WAKE_OP        5
#define FUTEX_LOCK_PI        6
#define FUTEX_UNLOCK_PI      7
#define FUTEX_TRYLOCK_PI     8
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI  12
#define FUTEX_LOCK_PI2         13

#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_BITSET_MATCH_ANY 0xffffffff

/* ------------------------------------------------------------------ */
/*  Futex syscall interface                                            */
/* ------------------------------------------------------------------ */

int64_t sys_futex(uint32_t *uaddr, int futex_op, uint32_t val, uint64_t timeout,
                  uint32_t *uaddr2, uint32_t val3);

void futex_init(void);

#endif /* INCLUDE_FUTEX_H_ */