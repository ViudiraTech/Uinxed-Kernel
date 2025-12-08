/*
 *
 *      tsc.h
 *      Time stamp counter header file
 *
 *      2025/10/29 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TSC_H_
#define INCLUDE_TSC_H_

#include <acpi.h>
#include <stdint.h>

/* Check if TSC is constant (not affected by CPU frequency changes) */
int tsc_check_invariant(void);

/* Use HPET to calibrate TSC frequency */
uint64_t tsc_calibrate_with_hpet(hpet_info_t *hpet_addr);

/* Get CPU frequency (Hz) */
uint64_t tsc_get_cpu_frequency(void);

/* Returns the TSC nanosecond value of the current time */
uint64_t tsc_nano_time(void);

/* Initialize TSC */
void tsc_init(void);

#endif // INCLUDE_TSC_H_
