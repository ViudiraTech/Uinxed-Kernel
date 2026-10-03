/*
 *
 *      timer.h
 *      Timer header file
 *
 *      2025/2/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TIMER_H_
#define INCLUDE_TIMER_H_

#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <syscall/abi.h>

#define TIMER_NSEC_PER_SEC    1000000000ULL
#define TIMER_USER_HZ         100ULL
#define TIMER_TICK_NS         (TIMER_NSEC_PER_SEC / CONFIG_TIMER_HZ)
#define TIMER_ABSTIME         1
#define TIMER_CLOCK_REALTIME  0
#define TIMER_CLOCK_MONOTONIC 1
#define TIMER_CLOCK_BOOTTIME  7

/* clock IDs (clock_gettime/clock_settime/timerfd) */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define CLOCK_REALTIME_ALARM     8
#define CLOCK_BOOTTIME_ALARM     9
#define CLOCK_TAI                11

/* Check whether the given clock ID and flags support sleeping */
bool timer_clock_sleep_supported(uint64_t clockid, uint64_t flags);

/* Convert a timespec to nanoseconds, validating the input range */
bool timer_timespec_to_ns(const linux_timespec_t *ts, uint64_t *ns);

/* Convert nanoseconds to timer ticks, rounding up */
uint64_t timer_ns_to_ticks_ceil(uint64_t ns);

/* Convert timer ticks to nanoseconds */
uint64_t timer_ticks_to_ns(uint64_t ticks);

/* Convert timer ticks to user-space ticks */
uint64_t timer_ticks_to_user_ticks(uint64_t ticks);

/* Convert nanoseconds to a timespec */
linux_timespec_t timer_ns_to_timespec(uint64_t ns);

/* Compute the sleep duration and tick count for a sleep request */
bool timer_sleep_duration(const linux_timespec_t *request, uint64_t now_ns, bool absolute, uint64_t *duration_ns, uint64_t *ticks);

/* Nanosecond-based delay function */
void nsleep(uint64_t ns);

/* Microsecond-based delay function */
void usleep(uint64_t us);

/* Millisecond-based delay function */
void msleep(uint64_t ms);

/* Wall-clock time shared by syscalls and persistent filesystem timestamps. */
int64_t  timer_realtime_ns(void);
uint64_t timer_monotonic_ns(void);
uint64_t timer_monotonic_resolution_ns(void);
void     timer_realtime_set_ns(int64_t nanoseconds);
uint32_t timer_realtime_seconds32(void);
int64_t  timer_realtime_seconds(void);

/* Whether CLOCK_MONOTONIC is backed by a source independent of scheduler ticks (TSC/HPET). */
int timer_monotonic_highres(void);

/* Periodic timer interrupt entry (fixed full-register assembly frame). */
void timer_handle(void);

/* Register timer bottom-half processing before kernel workers start. */
void timer_deferred_init(void);

#endif // INCLUDE_TIMER_H_
