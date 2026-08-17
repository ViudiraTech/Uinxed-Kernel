/*
 *
 *      tsc.h
 *      Time stamp counter header file
 *
 *      2025/10/29 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TSC_H_
#define INCLUDE_TSC_H_

#include <drivers/firmware/acpi.h>
#include <libs/std/stdint.h>

/* Check if TSC is constant (not affected by CPU frequency changes) */
int tsc_check_invariant(void);

/* Use HPET to calibrate TSC frequency */
uint64_t tsc_calibrate_with_hpet(hpet_info_t *hpet_addr);

/* Get TSC frequency (Hz) */
uint64_t tsc_get_cpu_frequency(void);

/* Whether TSC is safe and calibrated for monotonic clocksource use */
int tsc_clocksource_available(void);

/* TSC clocksource resolution in nanoseconds */
uint64_t tsc_resolution_ns(void);

/* Returns TSC time aligned to the boot-relative monotonic epoch */
uint64_t tsc_nano_time(void);

/* Initialize TSC */
void tsc_init(void);

#endif // INCLUDE_TSC_H_
