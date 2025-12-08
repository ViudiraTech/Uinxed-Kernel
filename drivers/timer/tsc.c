/*
 *
 *      tsc.c
 *      Time stamp counter
 *
 *      2025/10/29 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <cpuid.h>
#include <printk.h>
#include <tsc.h>

static uint64_t tsc_frequency  = 0;
static uint64_t tsc_boot_value = 0;
static uint64_t tsc_ns_ratio   = 0;

/* Check if TSC is constant (not affected by CPU frequency changes) */
int tsc_check_invariant(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000007, &eax, &ebx, &ecx, &edx);
    int tsc_invariant = (edx & (1 << 8)) != 0;

    if (!tsc_invariant) tsc_invariant = ((rdmsr(0x1a0) & (1 << 12)) != 0);
    return tsc_invariant;
}

/* Use HPET to calibrate TSC frequency */
uint64_t tsc_calibrate_with_hpet(hpet_info_t *hpet_addr)
{
    if (!hpet_addr) {
        plogk("tsc: HPET not available for calibration.\n");
        return 0;
    }
    if (!(hpet_addr->general_configuration & 1)) {
        plogk("tsc: HPET not enabled.\n");
        return 0;
    }

    const uint64_t calibration_time = 10000000;
    uint64_t       hpet_start, hpet_end, tsc_start, tsc_end;

    uint64_t  total_frequency  = 0;
    const int calibration_runs = 5;

    for (int i = 0; i < calibration_runs; i++) {
        hpet_start = nano_time();
        tsc_start  = rdtsc_serialized();

        uint64_t target_time = hpet_start + calibration_time;
        while (nano_time() < target_time) __asm__ volatile("pause");

        hpet_end = nano_time();
        tsc_end  = rdtsc_serialized();

        uint64_t hpet_elapsed = hpet_end - hpet_start;
        uint64_t tsc_elapsed  = tsc_end - tsc_start;

        if (hpet_elapsed > 0) {
            uint64_t frequency = (tsc_elapsed * 1000000000ULL) / hpet_elapsed;
            total_frequency += frequency;
        }
    }
    tsc_frequency = total_frequency / calibration_runs;
    tsc_ns_ratio  = (1000000000ULL << 32) / tsc_frequency;

    plogk("tsc: calibrated frequency = %lu MHz.\n", tsc_frequency / 1000000);
    plogk("tsc: ns ratio = %lu\n", tsc_ns_ratio);
    return tsc_frequency;
}

/* Get CPU frequency (Hz) */
uint64_t tsc_get_cpu_frequency(void)
{
    return tsc_frequency;
}

/* Returns the TSC nanosecond value of the current time */
uint64_t tsc_nano_time(void)
{
    uint64_t current_tsc = rdtsc_serialized();
    uint64_t elapsed_tsc = current_tsc - tsc_boot_value;
    return (elapsed_tsc * 1000000000ULL) / tsc_frequency;
}

/* Initialize TSC */
void tsc_init(void)
{
    if (!cpu_support_rdtsc()) {
        plogk("tsc: TSC not supported by CPU.\n");
        return;
    }
    plogk("tsc: rdtscp is %s.\n", cpu_support_rdtscp() ? "supported" : "not supported");
    plogk("tsc: invariant TSC is %s.\n", tsc_check_invariant() ? "supported" : "not supported");

    tsc_boot_value = rdtsc_serialized();
    tsc_calibrate_with_hpet(get_acpi_hpet());
}
