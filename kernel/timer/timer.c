/*
 *
 *      timer.c
 *      Timer
 *
 *      2025/2/17 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <acpi.h>
#include <apic.h>
#include <common.h>
#include <interrupt.h>
#include <math.h>
#include <printk.h>
#include <stdint.h>
#include <tsc.h>

/* Timer interrupt */
INTERRUPT_BEGIN void timer_handle(interrupt_frame_t *frame)
{
    (void)frame;
    disable_intr();
    send_eoi();
    enable_intr();
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
