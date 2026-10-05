/*
 *
 *      timer.c
 *      Timer
 *
 *      2025/2/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <arch/tables/idt.h>
#include <drivers/firmware/apic.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/time/tsc.h>
#include <drivers/tty/tty.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/math.h>
#include <net/core/netdev.h>
#include <process/kthread.h>
#include <process/process.h>
#include <process/sched.h>
#include <syscall/syscall.h>
#include <syscall/timerfd.h>

static int64_t      timer_realtime_base_ns;
static uint64_t     net_timer_last_tick;
static uint64_t     timer_monotonic_floor_ns;
static wait_queue_t timer_deferred_wait;
static spinlock_t   timer_deferred_lock;
static bool         timer_deferred_pending;
static bool         timer_deferred_registered;
static uint64_t     timer_deferred_last_tick;

/* Check whether the given clock ID and flags support sleeping */
bool timer_clock_sleep_supported(uint64_t clockid, uint64_t flags)
{
    return (clockid == TIMER_CLOCK_REALTIME || clockid == TIMER_CLOCK_MONOTONIC || clockid == TIMER_CLOCK_BOOTTIME) && (flags == 0 || flags == TIMER_ABSTIME);
}

/* Convert a timespec to nanoseconds, validating the input range */
bool timer_timespec_to_ns(const linux_timespec_t *ts, uint64_t *ns)
{
    if (!ts || !ns || ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= (int64_t)TIMER_NSEC_PER_SEC) return false;
    if ((uint64_t)ts->tv_sec > (UINT64_MAX - (uint64_t)ts->tv_nsec) / TIMER_NSEC_PER_SEC) return false;

    *ns = (uint64_t)ts->tv_sec * TIMER_NSEC_PER_SEC + (uint64_t)ts->tv_nsec;
    return true;
}

/* Convert nanoseconds to timer ticks, rounding up */
uint64_t timer_ns_to_ticks_ceil(uint64_t ns)
{
    return (ns / TIMER_TICK_NS) + (ns % TIMER_TICK_NS != 0);
}

/* Convert timer ticks to nanoseconds */
uint64_t timer_ticks_to_ns(uint64_t ticks)
{
    return ticks > UINT64_MAX / TIMER_TICK_NS ? UINT64_MAX : ticks * TIMER_TICK_NS;
}

/* Convert timer ticks to user-space ticks */
uint64_t timer_ticks_to_user_ticks(uint64_t ticks)
{
    uint64_t seconds = ticks / CONFIG_TIMER_HZ;
    uint64_t rest    = ticks % CONFIG_TIMER_HZ;
    if (seconds > UINT64_MAX / TIMER_USER_HZ) return UINT64_MAX;
    return (seconds * TIMER_USER_HZ) + (rest * TIMER_USER_HZ / CONFIG_TIMER_HZ);
}

/* Convert nanoseconds to a timespec */
linux_timespec_t timer_ns_to_timespec(uint64_t ns)
{
    linux_timespec_t ts = {
        .tv_sec  = (int64_t)(ns / TIMER_NSEC_PER_SEC),
        .tv_nsec = (int64_t)(ns % TIMER_NSEC_PER_SEC),
    };
    return ts;
}

/* Compute the sleep duration and tick count for a sleep request */
bool timer_sleep_duration(const linux_timespec_t *request, uint64_t now_ns, bool absolute, uint64_t *duration_ns, uint64_t *ticks)
{
    uint64_t request_ns;
    if (!duration_ns || !ticks || !timer_timespec_to_ns(request, &request_ns)) return false;

    if (absolute) {
        *duration_ns = (request_ns > now_ns) ? request_ns - now_ns : 0;
    } else {
        *duration_ns = request_ns;
    }
    *ticks = timer_ns_to_ticks_ceil(*duration_ns);
    return true;
}

/* Run the deferred timer bottom-half: TTY/timerfd flush, interval timers, vblank and network ticks. */
static void timer_deferred_service(void)
{
    tty_deferred_flush();
    timerfd_tick();

    uint64_t now = sched_ticks();
    signal_itimer_real_tick(now);
    drm_vblank_tick();

    uint64_t interval = CONFIG_TIMER_HZ / 100U;
    if (!interval) interval = 1;
    if (now - net_timer_last_tick >= interval) {
        net_timer_last_tick = now;
        net_timer(now);
    }
}

/* Kernel worker that services one queued deferred-timer pass per wakeup */
static int timer_deferred_worker(void *arg)
{
    (void)arg;

    while (!kthread_should_stop()) {
        spin_lock(&timer_deferred_lock);
        if (!timer_deferred_pending) {
            wait_queue_prepare(&timer_deferred_wait);
            spin_unlock(&timer_deferred_lock);
            wait_queue_sleep();
            continue;
        }
        timer_deferred_pending = false;
        spin_unlock(&timer_deferred_lock);
        timer_deferred_service();
    }
    return 0;
}

/* Ask the deferred-timer worker to run once */
static void timer_queue_deferred_work(void)
{
    bool wake = false;

    spin_lock(&timer_deferred_lock);
    if (!timer_deferred_pending) {
        timer_deferred_pending = true;
        wake                   = true;
    }
    spin_unlock(&timer_deferred_lock);

    if (wake) (void)wait_queue_wake_one_sync(&timer_deferred_wait);
}

/* Register the deferred-timer kernel worker before kernel workers start. */
void timer_deferred_init(void)
{
    if (timer_deferred_registered) return;

    wait_queue_init(&timer_deferred_wait);
    timer_deferred_lock      = (spinlock_t) {0};
    timer_deferred_pending   = false;
    timer_deferred_last_tick = 0;
    if (kernel_worker_register("timer-deferred", timer_deferred_worker, NULL, NULL) != EOK) {
        plogk("timer: Unable to register deferred worker.\n");
        return;
    }
    timer_deferred_registered = true;
}

/*
 * Return one unified boot-relative monotonic timeline.  Prefer a calibrated
 * invariant TSC because it is cheap to read; fall back to HPET, and only use
 * scheduler ticks when no high-resolution clocksource is available.
 *
 * The atomic floor also prevents a tiny cross-CPU TSC skew from making time
 * move backwards when a task migrates between CPUs.
 */
uint64_t timer_monotonic_ns(void)
{
    uint64_t now;

    if (tsc_clocksource_available()) {
        now = tsc_nano_time();
    } else if (hpet_available()) {
        now = nano_time();
    } else {
        now = timer_ticks_to_ns(sched_ticks());
    }

    uint64_t floor = __atomic_load_n(&timer_monotonic_floor_ns, __ATOMIC_ACQUIRE);
    for (;;) {
        if (now <= floor) return floor;
        if (__atomic_compare_exchange_n(&timer_monotonic_floor_ns, &floor, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return now;
    }
}

/* Resolution of the clocksource currently backing CLOCK_MONOTONIC. */
uint64_t timer_monotonic_resolution_ns(void)
{
    if (tsc_clocksource_available()) {
        uint64_t resolution = tsc_resolution_ns();
        if (resolution) return resolution;
    }
    if (hpet_available()) {
        uint64_t resolution = hpet_resolution_ns();
        if (resolution) return resolution;
    }
    return TIMER_TICK_NS;
}

/*
 * Whether the monotonic timeline advances on its own.  When neither TSC nor
 * HPET exists, timer_monotonic_ns() degenerates to scheduler ticks, and the
 * tick base must keep being driven by interrupt counting.
 */
int timer_monotonic_highres(void)
{
    return tsc_clocksource_available() || hpet_available();
}

/* Return the realtime clock in nanoseconds, saturating at INT64_MAX */
int64_t timer_realtime_ns(void)
{
    uint64_t monotonic = timer_monotonic_ns();
    int64_t  base      = __atomic_load_n(&timer_realtime_base_ns, __ATOMIC_ACQUIRE);
    if (base >= 0 && monotonic > (uint64_t)INT64_MAX - (uint64_t)base) return INT64_MAX;
    return (int64_t)monotonic + base;
}

/* Set the realtime clock to an absolute nanosecond value */
void timer_realtime_set_ns(int64_t nanoseconds)
{
    uint64_t monotonic = timer_monotonic_ns();
    int64_t  base      = monotonic > (uint64_t)INT64_MAX ? INT64_MIN : nanoseconds - (int64_t)monotonic;
    __atomic_store_n(&timer_realtime_base_ns, base, __ATOMIC_RELEASE);
}

/* Return the realtime clock as a clamped 32-bit seconds value */
uint32_t timer_realtime_seconds32(void)
{
    int64_t  nanoseconds = timer_realtime_ns();
    uint64_t seconds     = nanoseconds > 0 ? (uint64_t)nanoseconds / TIMER_NSEC_PER_SEC : 1;
    return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

/* Return the realtime clock as a 64-bit seconds value */
int64_t timer_realtime_seconds(void)
{
    return timer_realtime_ns() / (int64_t)TIMER_NSEC_PER_SEC;
}

/* Timer interrupt body operating on a stable, complete user register frame. */
__attribute__((used)) void timer_handle_frame(syscall_frame_t *frame)
{
    disable_intr();

    /*
     * Re-arm the local timer before any processing.  In TSC-deadline mode the
     * timer is one-shot: arming first keeps the tick period exact even when
     * the handling below runs long (a late arm would fire immediately and
     * self-correct, but arming early avoids the extra IRQ entirely).  Legacy
     * periodic mode re-arms itself in hardware and this call is a no-op.
     */
    lapic_timer_rearm_tick();

    /* Drain any NMI-parked message: a maskable IRQ can never interrupt a spinlock holder, so plogk() is safe here. */
    nmi_log_flush();
    task_t *interrupted = current_task();
    if (interrupted && interrupted->process) signal_itimer_cpu_tick(interrupted->process, user_mode(frame));
    send_eoi();
    if (timer_deferred_registered) {
        /*
         * Any CPU may claim this bottom-half pass; the compare-exchange keeps
         * it single-flight without pinning the work to one CPU, so a stalled
         * CPU can no longer stall TTY/timerfd/vblank/itimer servicing.
         */
        uint64_t now_ticks     = sched_ticks();
        uint64_t base_interval = CONFIG_TIMER_HZ / 100U;
        if (!base_interval) base_interval = 1;

        uint64_t monotonic_ns = timer_monotonic_ns();
        uint64_t last         = __atomic_load_n(&timer_deferred_last_tick, __ATOMIC_RELAXED);
        bool     due
            = now_ticks - last >= base_interval || tty_deferred_pending() || signal_itimer_real_next_tick() <= now_ticks || drm_vblank_deferred_due(monotonic_ns) || timerfd_deferred_due(monotonic_ns);
        if (due && __atomic_compare_exchange_n(&timer_deferred_last_tick, &last, now_ticks, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) timer_queue_deferred_work();
    }

    /* Keep CPU-local/global maintenance ahead of the possible context switch. */
    sched_tick(user_mode(frame));
    if (user_mode(frame)) (void)signal_deliver_if_pending(frame);
}

/* Assembly trampoline for timer_handle: saves all GPRs so signal delivery sees a complete frame. */
__asm__(".text\n"
        ".global timer_handle\n"
        ".type timer_handle, @function\n"
        "timer_handle:\n"
        "cld\n"
        "testb $3, 8(%rsp)\n"
        "jz 1f\n"
        "swapgs\n"
        "1:\n"
        "pushq %rax\n"
        "pushq %rbx\n"
        "pushq %rcx\n"
        "pushq %rdx\n"
        "pushq %rbp\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        "pushq %r8\n"
        "pushq %r9\n"
        "pushq %r10\n"
        "pushq %r11\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        "movq %rsp, %r12\n"
        "movq %r12, %rdi\n"
        "andq $-16, %rsp\n"
        "call timer_handle_frame\n"
        "movq %r12, %rsp\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %r11\n"
        "popq %r10\n"
        "popq %r9\n"
        "popq %r8\n"
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rbp\n"
        "popq %rdx\n"
        "popq %rcx\n"
        "popq %rbx\n"
        "popq %rax\n"
        "testb $3, 8(%rsp)\n"
        "jz 2f\n"
        "cli\n"
        "swapgs\n"
        "2:\n"
        "iretq\n"
        ".size timer_handle, .-timer_handle\n");

/* Nanosecond-based delay function */
void nsleep(uint64_t ns)
{
    uint64_t start_time = timer_monotonic_ns();
    while (timer_monotonic_ns() - start_time < ns) cpu_relax();
}

/* Microsecond-based delay function */
void usleep(uint64_t us)
{
    nsleep(us * 1000);
}

/* Millisecond-based delay function */
void msleep(uint64_t ms)
{
    nsleep(ms * 1000000);
}
