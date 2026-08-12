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

#include <kernel/interrupt/interrupt.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

#define TIMER_NSEC_PER_SEC 1000000000ULL
#ifndef TIMER_HZ
#    define TIMER_HZ 1000ULL
#endif
#define TIMER_USER_HZ         100ULL
#define TIMER_TICK_NS         (TIMER_NSEC_PER_SEC / TIMER_HZ)
#define TIMER_ABSTIME         1
#define TIMER_CLOCK_REALTIME  0
#define TIMER_CLOCK_MONOTONIC 1
#define TIMER_CLOCK_BOOTTIME  7

/* Linux clock IDs (clock_gettime/clock_settime/timerfd) */
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

typedef struct {
        int64_t tv_sec;
        int64_t tv_nsec;
} timer_timespec_t;

static inline bool timer_clock_sleep_supported(uint64_t clockid, uint64_t flags)
{
    return (clockid == TIMER_CLOCK_REALTIME || clockid == TIMER_CLOCK_MONOTONIC || clockid == TIMER_CLOCK_BOOTTIME) && (flags == 0 || flags == TIMER_ABSTIME);
}

static inline bool timer_timespec_to_ns(const timer_timespec_t *ts, uint64_t *ns)
{
    if (!ts || !ns || ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= (int64_t)TIMER_NSEC_PER_SEC) return false;
    if ((uint64_t)ts->tv_sec > (UINT64_MAX - (uint64_t)ts->tv_nsec) / TIMER_NSEC_PER_SEC) return false;

    *ns = (uint64_t)ts->tv_sec * TIMER_NSEC_PER_SEC + (uint64_t)ts->tv_nsec;
    return true;
}

static inline uint64_t timer_ns_to_ticks_ceil(uint64_t ns)
{
    return ns / TIMER_TICK_NS + (ns % TIMER_TICK_NS != 0);
}

static inline uint64_t timer_ticks_to_ns(uint64_t ticks)
{
    return ticks > UINT64_MAX / TIMER_TICK_NS ? UINT64_MAX : ticks * TIMER_TICK_NS;
}

static inline uint64_t timer_ticks_to_user_ticks(uint64_t ticks)
{
    uint64_t seconds = ticks / TIMER_HZ;
    uint64_t rest    = ticks % TIMER_HZ;
    if (seconds > UINT64_MAX / TIMER_USER_HZ) return UINT64_MAX;
    return seconds * TIMER_USER_HZ + rest * TIMER_USER_HZ / TIMER_HZ;
}

static inline timer_timespec_t timer_ns_to_timespec(uint64_t ns)
{
    timer_timespec_t ts = {
        .tv_sec  = (int64_t)(ns / TIMER_NSEC_PER_SEC),
        .tv_nsec = (int64_t)(ns % TIMER_NSEC_PER_SEC),
    };
    return ts;
}

static inline bool timer_sleep_duration(const timer_timespec_t *request, uint64_t now_ns, bool absolute, uint64_t *duration_ns, uint64_t *ticks)
{
    uint64_t request_ns;
    if (!duration_ns || !ticks || !timer_timespec_to_ns(request, &request_ns)) return false;

    *duration_ns = absolute ? (request_ns > now_ns ? request_ns - now_ns : 0) : request_ns;
    *ticks       = timer_ns_to_ticks_ceil(*duration_ns);
    return true;
}

/* Nanosecond-based delay function */
void nsleep(uint64_t ns);

/* Microsecond-based delay function */
void usleep(uint64_t us);

/* Millisecond-based delay function */
void msleep(uint64_t ms);

/* Wall-clock time shared by syscalls and persistent filesystem timestamps. */
int64_t  timer_realtime_ns(void);
uint64_t timer_monotonic_ns(void);
void     timer_realtime_set_ns(int64_t nanoseconds);
uint32_t timer_realtime_seconds32(void);

/* Periodic timer interrupt handler. */
INTERRUPT_BEGIN void timer_handle(interrupt_frame_t *frame);

#endif // INCLUDE_TIMER_H_
