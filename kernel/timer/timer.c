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
#include <drivers/gpu/drm_device.h>
#include <drivers/misc/apic.h>
#include <drivers/tty/tty.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/math.h>
#include <libs/std/stdint.h>
#include <net/netdev.h>
#include <proc/sched.h>
#include <syscall/timerfd.h>

static int64_t timer_realtime_base_ns;

int64_t timer_realtime_ns(void)
{
    return (int64_t)(sched_ticks() * TIMER_TICK_NS) + timer_realtime_base_ns;
}

void timer_realtime_set_ns(int64_t nanoseconds)
{
    timer_realtime_base_ns = nanoseconds - (int64_t)(sched_ticks() * TIMER_TICK_NS);
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
    tty_deferred_flush();
    send_eoi();
    sched_tick();
    timerfd_tick();
    if (get_current_cpu_id() == 0) drm_vblank_tick();
    if (get_current_cpu_id() == 0) net_timer(sched_ticks());
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
