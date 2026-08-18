/*
 *
 *      timer.c
 *      Timer
 *
 *      2025/2/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/smp.h>
#include <drivers/firmware/acpi.h>
#include <drivers/firmware/apic.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/time/tsc.h>
#include <drivers/tty/tty.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/math.h>
#include <libs/std/stdint.h>
#include <net/core/netdev.h>
#include <process/process.h>
#include <process/sched.h>
#include <sync/signal.h>
#include <syscall/timerfd.h>

static int64_t  timer_realtime_base_ns;
static uint64_t net_timer_last_tick;
static uint64_t timer_monotonic_floor_ns;

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

    if (tsc_clocksource_available())
        now = tsc_nano_time();
    else if (hpet_available())
        now = nano_time();
    else
        now = timer_ticks_to_ns(sched_ticks());

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

/* Timer interrupt */
INTERRUPT_BEGIN void timer_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    disable_intr();
    uint32_t cpu_id      = get_current_cpu_id();
    task_t  *interrupted = current_task();
    if (interrupted && interrupted->process) signal_itimer_cpu_tick(interrupted->process, (frame->cs & 3U) == 3U);
    if (cpu_id == 0) tty_deferred_flush();
    send_eoi();
    sched_tick();
    timerfd_tick();
    if (cpu_id == 0) {
        signal_itimer_real_tick(sched_ticks());
        drm_vblank_tick();

        /*
         * Protocol timers only need 10 ms service resolution.  Keep them on
         * the TIMER_HZ time base, but do not scan every socket at 1000 Hz.
         */
        uint64_t now      = sched_ticks();
        uint64_t interval = TIMER_HZ / 100U;
        if (!interval) interval = 1;
        if (now - net_timer_last_tick >= interval) {
            net_timer_last_tick = now;
            net_timer(now);
        }
    }
    /* iretq restores IF; enabling it here would make the saved frame re-entrant. */
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Nanosecond-based delay function */
void nsleep(uint64_t ns)
{
    uint64_t start_time = timer_monotonic_ns();
    while (timer_monotonic_ns() - start_time < ns) __asm__ volatile("pause");
}

/* Millisecond-based delay functions */
void usleep(uint64_t us)
{
    nsleep(us * 1000);
}

/* Millisecond-based delay functions */
void msleep(uint64_t ms)
{
    nsleep(ms * 1000000);
}
