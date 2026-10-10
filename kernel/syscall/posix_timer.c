/*
 *
 *      posix_timer.c
 *      Per-process POSIX interval timers (timer_create and friends)
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <process/process.h>
#include <process/uaccess.h>
#include <sync/signal.h>
#include <syscall/posix_timer.h>
#include <syscall/syscall.h>

/* Linux x86-64 itimerspec: two timespecs, no padding. */
typedef struct {
        linux_timespec_t it_interval;
        linux_timespec_t it_value;
} posix_itimerspec_t;

_Static_assert(sizeof(posix_itimerspec_t) == 32, "x86-64 itimerspec ABI size");

/*
 * The leading fields of struct sigevent.  glibc stores sigev_notify_thread_id
 * in the first member of the trailing union, which starts at offset 16.
 */
typedef struct {
        sigval_t sigev_value;
        int32_t  sigev_signo;
        int32_t  sigev_notify;
        int32_t  sigev_notify_thread_id;
        int32_t  __pad;
} posix_sigevent_t;

_Static_assert(offsetof(posix_sigevent_t, sigev_signo) == 8, "x86-64 sigevent sigev_signo offset");
_Static_assert(offsetof(posix_sigevent_t, sigev_notify) == 12, "x86-64 sigevent sigev_notify offset");
_Static_assert(offsetof(posix_sigevent_t, sigev_notify_thread_id) == 16, "x86-64 sigevent sigev_notify_thread_id offset");

#define POSIX_TIMER_SLOTS   64
#define POSIX_TIMER_SIG_MAX 64

typedef struct {
        bool     in_use;
        uint64_t owner; // owning thread group pid; timers are process-scoped
        uint32_t clockid;
        int32_t  notify;
        int32_t  signo;
        int32_t  notify_tid; // SIGEV_THREAD_ID target, 0 for process-directed
        sigval_t value;
        uint64_t interval_ns;
        uint64_t deadline_ns; // absolute in the timer's clock base, 0 when disarmed
        uint64_t overrun;
} posix_timer_t;

static posix_timer_t posix_timers[POSIX_TIMER_SLOTS];
static spinlock_t    posix_timer_lock;
static uint64_t      posix_next_monotonic_ns = UINT64_MAX;
static uint64_t      posix_next_realtime_ns  = UINT64_MAX;

/* Current time in the timer's clock base, or -1 for an unsupported clock. */
static int64_t posix_clock_now(uint32_t clockid)
{
    if (clockid == CLOCK_MONOTONIC) return (int64_t)timer_monotonic_ns();
    if (clockid == CLOCK_REALTIME) return timer_realtime_ns();
    return -1;
}

/* Convert a user timespec to nanoseconds, rejecting a malformed value. */
static int posix_timespec_to_ns(const linux_timespec_t *ts, uint64_t *ns)
{
    if (!ts || ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= (int64_t)TIMER_NSEC_PER_SEC) return -EINVAL;
    if ((uint64_t)ts->tv_sec > (UINT64_MAX - (uint64_t)ts->tv_nsec) / TIMER_NSEC_PER_SEC) return -EINVAL;
    *ns = ((uint64_t)ts->tv_sec * TIMER_NSEC_PER_SEC) + (uint64_t)ts->tv_nsec;
    return 0;
}

static void posix_ns_to_timespec(uint64_t ns, linux_timespec_t *ts)
{
    ts->tv_sec  = (int64_t)(ns / TIMER_NSEC_PER_SEC);
    ts->tv_nsec = (int64_t)(ns % TIMER_NSEC_PER_SEC);
}

/* Republish the earliest armed deadline so the tick only scans when needed. */
static void posix_timer_publish_locked(void)
{
    uint64_t mono = UINT64_MAX, real = UINT64_MAX;
    for (size_t i = 0; i < POSIX_TIMER_SLOTS; i++) {
        if (!posix_timers[i].in_use || !posix_timers[i].deadline_ns) continue;
        if (posix_timers[i].clockid == CLOCK_REALTIME) {
            if (posix_timers[i].deadline_ns < real) real = posix_timers[i].deadline_ns;
        } else if (posix_timers[i].deadline_ns < mono)
            mono = posix_timers[i].deadline_ns;
    }
    __atomic_store_n(&posix_next_monotonic_ns, mono, __ATOMIC_RELEASE);
    __atomic_store_n(&posix_next_realtime_ns, real, __ATOMIC_RELEASE);
}

bool posix_timer_deferred_due(uint64_t monotonic_ns)
{
    if (monotonic_ns >= __atomic_load_n(&posix_next_monotonic_ns, __ATOMIC_ACQUIRE)) return true;
    uint64_t real = __atomic_load_n(&posix_next_realtime_ns, __ATOMIC_ACQUIRE);
    if (real == UINT64_MAX) return false;
    int64_t now = timer_realtime_ns();
    return now >= 0 && (uint64_t)now >= real;
}

/* Locate a slot owned by the calling process; the returned index is only valid under the lock. */
static int posix_timer_lookup_locked(uint64_t id, uint64_t owner)
{
    if (!id || id > POSIX_TIMER_SLOTS) return -EINVAL;
    size_t index = (size_t)id - 1;
    if (!posix_timers[index].in_use || posix_timers[index].owner != owner) return -EINVAL;
    return (int)index;
}

int64_t sys_timer_create(uint64_t clockid, uint64_t sigevent, uint64_t timerid_ptr, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc || !proc->task) return -ESRCH;
    if (!timerid_ptr) return -EFAULT;
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC) return -EINVAL;

    posix_sigevent_t sev = {.sigev_value = {.sival_ptr = NULL}, .sigev_signo = 15 /* SIGALRM */, .sigev_notify = SIGEV_SIGNAL};
    if (sigevent && copy_from_user(&sev, (const void *)sigevent, sizeof(sev))) return -EFAULT;

    /*
     * SIGEV_THREAD is implemented in userspace by glibc, which reaches the
     * kernel as SIGEV_THREAD_ID; anything else is not a kernel-visible mode.
     */
    if (sev.sigev_notify != SIGEV_SIGNAL && sev.sigev_notify != SIGEV_NONE && sev.sigev_notify != SIGEV_THREAD_ID) return -EINVAL;
    if (sev.sigev_notify == SIGEV_SIGNAL && (sev.sigev_signo < 1 || sev.sigev_signo > POSIX_TIMER_SIG_MAX)) return -EINVAL;
    if (sev.sigev_notify == SIGEV_THREAD_ID && sev.sigev_notify_thread_id <= 0) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&posix_timer_lock);
    int      slot  = -1;
    for (size_t i = 0; i < POSIX_TIMER_SLOTS; i++)
        if (!posix_timers[i].in_use) {
            slot = (int)i;
            break;
        }
    if (slot < 0) {
        spin_unlock_irqrestore(&posix_timer_lock, flags);
        return -EAGAIN;
    }
    posix_timer_t *timer = &posix_timers[slot];
    memset(timer, 0, sizeof(*timer));
    timer->in_use     = true;
    timer->owner      = proc->task->pid;
    timer->clockid    = (uint32_t)clockid;
    timer->notify     = sev.sigev_notify;
    timer->signo      = sev.sigev_signo;
    timer->notify_tid = sev.sigev_notify_thread_id;
    timer->value      = sev.sigev_value;
    spin_unlock_irqrestore(&posix_timer_lock, flags);

    uint64_t id = (uint64_t)slot + 1;
    if (copy_to_user((void *)timerid_ptr, &id, sizeof(id))) {
        flags = spin_lock_irqsave(&posix_timer_lock);
        memset(timer, 0, sizeof(*timer));
        spin_unlock_irqrestore(&posix_timer_lock, flags);
        return -EFAULT;
    }
    return 0;
}

int64_t sys_timer_settime(uint64_t timerid, uint64_t flags, uint64_t new_value, uint64_t old_value, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc || !proc->task) return -ESRCH;
    if (flags & ~(uint64_t)TIMER_ABSTIME) return -EINVAL;
    if (!new_value) return -EFAULT;

    posix_itimerspec_t requested;
    if (copy_from_user(&requested, (const void *)new_value, sizeof(requested))) return -EFAULT;
    uint64_t interval_ns = 0, value_ns = 0;
    int      result = posix_timespec_to_ns(&requested.it_interval, &interval_ns);
    if (result) return result;
    result = posix_timespec_to_ns(&requested.it_value, &value_ns);
    if (result) return result;

    uint64_t irq  = spin_lock_irqsave(&posix_timer_lock);
    int      slot = posix_timer_lookup_locked(timerid, proc->task->pid);
    if (slot < 0) {
        spin_unlock_irqrestore(&posix_timer_lock, irq);
        return slot;
    }
    posix_timer_t *timer = &posix_timers[slot];

    if (old_value) {
        posix_itimerspec_t previous;
        posix_ns_to_timespec(timer->interval_ns, &previous.it_interval);
        int64_t  now       = posix_clock_now(timer->clockid);
        uint64_t remaining = 0;
        if (timer->deadline_ns && now >= 0 && (uint64_t)now < timer->deadline_ns) remaining = timer->deadline_ns - (uint64_t)now;
        posix_ns_to_timespec(remaining, &previous.it_value);
        if (copy_to_user((void *)old_value, &previous, sizeof(previous))) {
            spin_unlock_irqrestore(&posix_timer_lock, irq);
            return -EFAULT;
        }
    }

    timer->interval_ns = interval_ns;
    timer->overrun     = 0;
    if (!value_ns) {
        timer->deadline_ns = 0;
    } else if (flags & TIMER_ABSTIME) {
        timer->deadline_ns = value_ns;
    } else {
        int64_t now = posix_clock_now(timer->clockid);
        if (now < 0) {
            spin_unlock_irqrestore(&posix_timer_lock, irq);
            return -EINVAL;
        }
        /* A relative value that overflows the clock base delays to the far future. */
        timer->deadline_ns = (uint64_t)now > UINT64_MAX - value_ns ? UINT64_MAX : (uint64_t)now + value_ns;
    }
    posix_timer_publish_locked();
    spin_unlock_irqrestore(&posix_timer_lock, irq);
    return 0;
}

int64_t sys_timer_gettime(uint64_t timerid, uint64_t cur_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc || !proc->task) return -ESRCH;
    if (!cur_value) return -EFAULT;

    uint64_t irq  = spin_lock_irqsave(&posix_timer_lock);
    int      slot = posix_timer_lookup_locked(timerid, proc->task->pid);
    if (slot < 0) {
        spin_unlock_irqrestore(&posix_timer_lock, irq);
        return slot;
    }
    posix_timer_t     *timer = &posix_timers[slot];
    posix_itimerspec_t current;
    posix_ns_to_timespec(timer->interval_ns, &current.it_interval);
    int64_t  now       = posix_clock_now(timer->clockid);
    uint64_t remaining = 0;
    if (timer->deadline_ns && now >= 0 && (uint64_t)now < timer->deadline_ns) remaining = timer->deadline_ns - (uint64_t)now;
    posix_ns_to_timespec(remaining, &current.it_value);
    spin_unlock_irqrestore(&posix_timer_lock, irq);

    return copy_to_user((void *)cur_value, &current, sizeof(current)) ? -EFAULT : 0;
}

int64_t sys_timer_getoverrun(uint64_t timerid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc || !proc->task) return -ESRCH;

    uint64_t irq    = spin_lock_irqsave(&posix_timer_lock);
    int      slot   = posix_timer_lookup_locked(timerid, proc->task->pid);
    int64_t  result = slot < 0 ? slot : (int64_t)posix_timers[slot].overrun;
    spin_unlock_irqrestore(&posix_timer_lock, irq);
    return result;
}

int64_t sys_timer_delete(uint64_t timerid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc || !proc->task) return -ESRCH;

    uint64_t irq  = spin_lock_irqsave(&posix_timer_lock);
    int      slot = posix_timer_lookup_locked(timerid, proc->task->pid);
    if (slot < 0) {
        spin_unlock_irqrestore(&posix_timer_lock, irq);
        return slot;
    }
    memset(&posix_timers[slot], 0, sizeof(posix_timers[slot]));
    posix_timer_publish_locked();
    spin_unlock_irqrestore(&posix_timer_lock, irq);
    return 0;
}

/* Drop every timer owned by an exiting process, armed or not. */
void posix_timer_release_process(process_t *proc)
{
    if (!proc || !proc->task) return;
    uint64_t irq = spin_lock_irqsave(&posix_timer_lock);
    for (size_t i = 0; i < POSIX_TIMER_SLOTS; i++)
        if (posix_timers[i].in_use && posix_timers[i].owner == proc->task->pid) memset(&posix_timers[i], 0, sizeof(posix_timers[i]));
    posix_timer_publish_locked();
    spin_unlock_irqrestore(&posix_timer_lock, irq);
}

/*
 * Deliver expiries from the deferred bottom half.  Deliveries are collected
 * under the lock and signalled afterwards so signal delivery never runs with
 * the timer lock held.
 */
void posix_timer_tick(void)
{
    typedef struct {
            uint64_t owner;
            int32_t  notify;
            int32_t  signo;
            int32_t  tid;
            sigval_t value;
            uint64_t overrun;
    } delivery_t;

    delivery_t pending[POSIX_TIMER_SLOTS];
    size_t     count = 0;

    uint64_t irq = spin_lock_irqsave(&posix_timer_lock);
    for (size_t i = 0; i < POSIX_TIMER_SLOTS; i++) {
        posix_timer_t *timer = &posix_timers[i];
        if (!timer->in_use || !timer->deadline_ns) continue;
        int64_t now = posix_clock_now(timer->clockid);
        if (now < 0 || (uint64_t)now < timer->deadline_ns) continue;

        if (count < POSIX_TIMER_SLOTS) {
            pending[count].owner   = timer->owner;
            pending[count].notify  = timer->notify;
            pending[count].signo   = timer->signo;
            pending[count].tid     = timer->notify_tid;
            pending[count].value   = timer->value;
            pending[count].overrun = timer->overrun;
            count++;
        }

        if (!timer->interval_ns) {
            timer->deadline_ns = 0;
            continue;
        }
        /* Count the expiries that were skipped because a previous one was still pending. */
        uint64_t missed = ((uint64_t)now - timer->deadline_ns) / timer->interval_ns;
        timer->overrun += missed;
        if (timer->deadline_ns > UINT64_MAX - timer->interval_ns) {
            timer->deadline_ns = 0;
        } else {
            timer->deadline_ns += (missed + 1) * timer->interval_ns;
        }
    }
    posix_timer_publish_locked();
    spin_unlock_irqrestore(&posix_timer_lock, irq);

    for (size_t i = 0; i < count; i++) {
        if (pending[i].notify == SIGEV_NONE) continue;
        siginfo_t info  = {0};
        info.si_signo   = pending[i].signo;
        info.si_code    = SI_TIMER;
        info.si_value   = pending[i].value;
        info.si_overrun = (int)pending[i].overrun;
        if (pending[i].notify == SIGEV_THREAD_ID) {
            task_t *target = pid_find_task_get((pid_t)pending[i].tid);
            if (target) {
                info.si_tid = pending[i].tid;
                signal_send_thread(target, pending[i].signo, &info);
                task_put(target);
            }
            continue;
        }
        process_t *owner = process_find_get((pid_t)pending[i].owner);
        if (owner) {
            signal_send(owner, pending[i].signo, &info);
            process_put(owner);
        }
    }
}
