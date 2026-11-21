/*
 *
 *      syscall_time.c
 *      Clocks, sleeps, timers and time conversion
 *
 *      2026/11/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/abi.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/stdint.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <sync/signal.h>
#include <syscall/syscall_impl.h>

/* getitimer / setitimer / alarm */
typedef struct linux_itimerval {
        int64_t it_interval_sec;
        int64_t it_interval_usec;
        int64_t it_value_sec;
        int64_t it_value_usec;
} linux_itimerval_t;

/* times */
typedef struct linux_tms {
        int64_t tms_utime;
        int64_t tms_stime;
        int64_t tms_cutime;
        int64_t tms_cstime;
} linux_tms_t;

/* Current time in nanoseconds for the requested clock */
static uint64_t clock_sleep_now_ns(int clockid)
{
    uint64_t uptime_ns = timer_monotonic_ns();
    if (clockid != TIMER_CLOCK_REALTIME) return uptime_ns;

    int64_t realtime_ns = timer_realtime_ns();
    return realtime_ns > 0 ? (uint64_t)realtime_ns : 0;
}

/* Sleep until a deadline or signal delivery */
static int64_t clock_sleep(uint64_t clockid, uint64_t flags, uint64_t req, uint64_t rem)
{
    if (!timer_clock_sleep_supported(clockid, flags)) return -EINVAL;
    if (!req) return -EFAULT;

    linux_timespec_t request;
    if (copy_from_user(&request, (const void *)req, sizeof(request))) return -EFAULT;

    uint64_t now_ns = clock_sleep_now_ns((int)clockid);
    uint64_t duration_ns;
    uint64_t sleep_ticks;
    if (!timer_sleep_duration(&request, now_ns, flags == TIMER_ABSTIME, &duration_ns, &sleep_ticks)) return -EINVAL;
    if (!sleep_ticks) return EOK;

    uint64_t     start_tick    = sched_ticks();
    uint64_t     deadline_tick = UINT64_MAX - start_tick < sleep_ticks ? UINT64_MAX : start_tick + sleep_ticks;
    wait_queue_t sleep_queue;
    wait_queue_init(&sleep_queue);

    for (;;) {
        uint64_t now_tick = sched_ticks();
        if (now_tick >= deadline_tick) return EOK;

        wait_queue_prepare(&sleep_queue);
        now_tick = sched_ticks();
        if (signal_has_pending_current()) {
            wait_queue_cancel(&sleep_queue);
            if (!(flags & TIMER_ABSTIME) && rem) {
                uint64_t         elapsed_ticks = now_tick - start_tick;
                uint64_t         elapsed_ns    = elapsed_ticks > UINT64_MAX / TIMER_TICK_NS ? UINT64_MAX : elapsed_ticks * TIMER_TICK_NS;
                uint64_t         remaining_ns  = elapsed_ns < duration_ns ? duration_ns - elapsed_ns : 0;
                linux_timespec_t remaining     = timer_ns_to_timespec(remaining_ns);
                if (copy_to_user((void *)rem, &remaining, sizeof(remaining))) return -EFAULT;
            }
            return -EINTR;
        }
        if (now_tick >= deadline_tick) {
            wait_queue_cancel(&sleep_queue);
            return EOK;
        }
        wait_queue_wait_timed(&sleep_queue, deadline_tick);
    }
}

/* System call handler for `nanosleep`. */
int64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return clock_sleep(TIMER_CLOCK_MONOTONIC, 0, req, rem);
}

/* System call handler for `gettimeofday`. */
int64_t sys_gettimeofday(uint64_t tv, uint64_t tz, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tz;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!tv) return 0;

    int64_t         uptime_ns = timer_realtime_ns();
    linux_timeval_t now       = {
              .tv_sec  = uptime_ns / 1000000000LL,
              .tv_usec = (uptime_ns % 1000000000LL) / 1000,
    };

    return copy_to_user((void *)tv, &now, sizeof(now)) ? -EFAULT : 0;
}

/* System call handler for `time`. */
int64_t sys_time(uint64_t tloc, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    int64_t now = timer_realtime_ns() / 1000000000LL;
    if (tloc && copy_to_user((void *)tloc, &now, sizeof(now))) return -EFAULT;
    return now;
}

/* System call handler for `clock_settime`. */
int64_t sys_clock_settime_impl(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!tp) return -EFAULT;
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_REALTIME_COARSE && clockid != CLOCK_REALTIME_ALARM) return -EINVAL;

    linux_timespec_t ts;
    if (copy_from_user(&ts, (const void *)tp, sizeof(ts))) return -EFAULT;
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) return -EINVAL;

    timer_realtime_set_ns((ts.tv_sec * 1000000000LL) + ts.tv_nsec);
    return EOK;
}

/* System call handler for `clock_nanosleep`. */
int64_t sys_clock_nanosleep_impl(uint64_t clockid, uint64_t flags, uint64_t req, uint64_t rem, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return clock_sleep(clockid, flags, req, rem);
}

/* Convert seconds+microseconds to timer ticks */
static int itimer_time_to_ticks(int64_t sec, int64_t usec, uint64_t *ticks)
{
    if (!ticks || sec < 0 || usec < 0 || usec >= 1000000) return -EINVAL;
    uint64_t sub_ticks = ((uint64_t)usec * CONFIG_TIMER_HZ + 999999ULL) / 1000000ULL;

    if ((uint64_t)sec > (UINT64_MAX - sub_ticks) / CONFIG_TIMER_HZ) return -EINVAL;
    *ticks = ((uint64_t)sec * CONFIG_TIMER_HZ) + sub_ticks;

    return 0;
}

/* Convert ticks back to an itimerval */
static linux_itimerval_t itimer_ticks_to_value(uint64_t remaining, uint64_t interval)
{
    linux_itimerval_t value = {
        .it_interval_sec  = (int64_t)(interval / CONFIG_TIMER_HZ),
        .it_interval_usec = (int64_t)((interval % CONFIG_TIMER_HZ) * 1000000ULL / CONFIG_TIMER_HZ),
        .it_value_sec     = (int64_t)(remaining / CONFIG_TIMER_HZ),
        .it_value_usec    = (int64_t)((remaining % CONFIG_TIMER_HZ) * 1000000ULL / CONFIG_TIMER_HZ),
    };
    return value;
}

/* getitimer syscall: read an interval timer */
int64_t sys_getitimer_impl(uint64_t which, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (which > 2) return -EINVAL;
    if (!curr_value) return -EFAULT;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    uint64_t remaining = 0, interval = 0;

    signal_itimer_get(proc, (unsigned int)which, &remaining, &interval);
    linux_itimerval_t tv = itimer_ticks_to_value(remaining, interval);
    return copy_to_user((void *)curr_value, &tv, sizeof(tv)) ? -EFAULT : 0;
}

/* setitimer syscall: arm or disarm an interval timer */
int64_t sys_setitimer_impl(uint64_t which, uint64_t new_value, uint64_t old_value, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (which > 2) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    /* A NULL new_value disarms the timer. */
    linux_itimerval_t requested = {0};
    if (new_value && copy_from_user(&requested, (const void *)new_value, sizeof(requested))) return -EFAULT;

    uint64_t value_ticks = 0, interval_ticks = 0;
    int      ret = itimer_time_to_ticks(requested.it_value_sec, requested.it_value_usec, &value_ticks);
    if (ret) return ret;

    ret = itimer_time_to_ticks(requested.it_interval_sec, requested.it_interval_usec, &interval_ticks);
    if (ret) return ret;

    uint64_t old_remaining = 0, old_interval = 0;
    signal_itimer_set(proc, (unsigned int)which, value_ticks, interval_ticks, &old_remaining, &old_interval);
    if (old_value) {
        linux_itimerval_t previous = itimer_ticks_to_value(old_remaining, old_interval);
        if (copy_to_user((void *)old_value, &previous, sizeof(previous))) return -EFAULT;
    }

    return 0;
}

/* alarm syscall: schedule a SIGALRM after N seconds */
int64_t sys_alarm_impl(uint64_t seconds, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint64_t value         = seconds > UINT64_MAX / CONFIG_TIMER_HZ ? UINT64_MAX : seconds * CONFIG_TIMER_HZ;
    uint64_t old_remaining = 0;
    signal_itimer_set(proc, 0, value, 0, &old_remaining, NULL);

    if (!old_remaining) return 0;
    uint64_t rounded = (old_remaining / CONFIG_TIMER_HZ) + (old_remaining % CONFIG_TIMER_HZ != 0);

    return rounded > UINT32_MAX ? UINT32_MAX : (int64_t)rounded;
}

/* times syscall: return process CPU time */
int64_t sys_times_impl(uint64_t tms, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!tms) return -EFAULT;
    int64_t     now = (int64_t)timer_ticks_to_user_ticks(sched_ticks());
    linux_tms_t buf = {.tms_utime = now, .tms_stime = 0, .tms_cutime = 0, .tms_cstime = 0};

    if (copy_to_user((void *)tms, &buf, sizeof(buf))) return -EFAULT;
    return now;
}

/* clock_gettime syscall */
int64_t sys_clock_gettime_impl(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!tp) return -EFAULT;
    linux_timespec_t ts;
    int64_t          ns;

    switch (clockid) {
        case 0 :
        case 5 : // CLOCK_REALTIME / CLOCK_REALTIME_COARSE
            ns = timer_realtime_ns();
            break;
        case 1 :
        case 4 :
        case 6 :
        case 7 : // CLOCK_MONOTONIC / _RAW / _COARSE / BOOTTIME
            ns = (int64_t)timer_monotonic_ns();
            break;
        case 2 :
        case 3 : // PROCESS/THREAD_CPUTIME_ID
            ns = 0;
            break;
        default :
            return -EINVAL;
    }

    ts.tv_sec  = ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    return copy_to_user((void *)tp, &ts, sizeof(ts)) ? -EFAULT : 0;
}

/* clock_getres syscall */
int64_t sys_clock_getres_impl(uint64_t clockid, uint64_t res, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!res) return 0;
    linux_timespec_t ts;

    switch (clockid) {
        case CLOCK_REALTIME :
        case CLOCK_MONOTONIC :
        case CLOCK_MONOTONIC_RAW :
        case CLOCK_REALTIME_COARSE :
        case CLOCK_MONOTONIC_COARSE :
        case CLOCK_BOOTTIME :
            ts.tv_sec  = 0;
            ts.tv_nsec = (int64_t)timer_monotonic_resolution_ns();
            break;
        case CLOCK_PROCESS_CPUTIME_ID :
        case CLOCK_THREAD_CPUTIME_ID :
            ts.tv_sec  = 0;
            ts.tv_nsec = TIMER_TICK_NS;
            break;
        default :
            return -EINVAL;
    }

    return copy_to_user((void *)res, &ts, sizeof(ts)) ? -EFAULT : 0;
}

/* settimeofday syscall */
int64_t sys_settimeofday_impl(uint64_t tv, uint64_t tz, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tz;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!tv) return -EFAULT;

    struct {
            int64_t tv_sec;
            int64_t tv_usec;
    } timeval;

    if (copy_from_user(&timeval, (const void *)tv, sizeof(timeval))) return -EFAULT;
    timer_realtime_set_ns((timeval.tv_sec * 1000000000LL) + (timeval.tv_usec * 1000LL));
    return 0;
}
