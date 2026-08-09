/*
 *
 *      timer.c
 *      Timer
 *
 *      2025/2/17 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/smp.h>
#include <chipset/common.h>
#include <drivers/clocksource/tsc.h>
#include <drivers/firmware/acpi.h>
#include <drivers/firmware/apic.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/tty/tty.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/math.h>
#include <libs/std/stdint.h>
#include <net/core/netdev.h>
#include <proc/sched.h>
#include <syscall/timerfd.h>

static int64_t  timer_realtime_base_ns;
static uint64_t net_timer_last_tick;

uint64_t timer_monotonic_ns(void)
{
    return timer_ticks_to_ns(sched_ticks());
}

int64_t timer_realtime_ns(void)
{
    uint64_t monotonic = timer_monotonic_ns();
    int64_t  base      = __atomic_load_n(&timer_realtime_base_ns, __ATOMIC_ACQUIRE);
    if (base >= 0 && monotonic > (uint64_t)INT64_MAX - (uint64_t)base) return INT64_MAX;
    return (int64_t)monotonic + base;
}

void timer_realtime_set_ns(int64_t nanoseconds)
{
    uint64_t monotonic = timer_monotonic_ns();
    int64_t  base      = monotonic > (uint64_t)INT64_MAX ? INT64_MIN : nanoseconds - (int64_t)monotonic;
    __atomic_store_n(&timer_realtime_base_ns, base, __ATOMIC_RELEASE);
}

uint32_t timer_realtime_seconds32(void)
{
    int64_t  nanoseconds = timer_realtime_ns();
    uint64_t seconds     = nanoseconds > 0 ? (uint64_t)nanoseconds / TIMER_NSEC_PER_SEC : 1;
    return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

/* Timer interrupt */
INTERRUPT_BEGIN void timer_handle(interrupt_frame_t *frame)
{
    (void)frame;
    disable_intr();
    uint32_t cpu_id = get_current_cpu_id();
    if (cpu_id == 0) tty_deferred_flush();
    send_eoi();
    sched_tick();
    timerfd_tick();
    if (cpu_id == 0) {
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
}
INTERRUPT_END

/* Nanosecond-based delay function */
void nsleep(uint64_t ns)
{
    uint64_t (*nano)(void) = tsc_check_invariant() ? tsc_nano_time : nano_time;

    uint64_t start_time = nano();
    uint64_t elapsed    = 0;

    while (elapsed < ns) {
        uint64_t current_time = nano();

        if (current_time < start_time) {
            elapsed = UINT64_MAX - start_time + current_time;
        } else {
            elapsed = current_time - start_time;
        }
    }
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
