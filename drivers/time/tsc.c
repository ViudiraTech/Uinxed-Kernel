/*
 *
 *      tsc.c
 *      Time stamp counter
 *
 *      2025/10/29 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/cpuid.h>
#include <drivers/time/tsc.h>
#include <kernel/printk.h>

static uint64_t tsc_frequency       = 0;
static uint64_t tsc_epoch_value     = 0;
static uint64_t tsc_epoch_ns        = 0;
static uint64_t tsc_ns_ratio        = 0;
static int      tsc_invariant       = 0;
static int      tsc_clocksource_ok  = 0;

/* Check whether the architectural invariant-TSC capability is advertised. */
int tsc_check_invariant(void)
{
    uint32_t eax, ebx, ecx, edx;

    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000007U) return 0;

    cpuid(0x80000007, &eax, &ebx, &ecx, &edx);
    return (edx & (1U << 8)) != 0;
}

/* Use HPET to calibrate TSC frequency. */
uint64_t tsc_calibrate_with_hpet(hpet_info_t *hpet_addr)
{
    if (!hpet_addr || !hpet_available()) {
        plogk("tsc: HPET not available for calibration.\n");
        return 0;
    }

    const uint64_t calibration_time = 10000000ULL; /* 10 ms */
    uint64_t       total_frequency  = 0;
    unsigned int   valid_runs       = 0;
    const unsigned int calibration_runs = 5;

    for (unsigned int i = 0; i < calibration_runs; i++) {
        uint64_t hpet_start = nano_time();
        uint64_t tsc_start  = rdtsc_serialized();
        uint64_t target     = hpet_start + calibration_time;

        while (nano_time() < target) __asm__ volatile("pause");

        uint64_t tsc_end  = rdtsc_serialized();
        uint64_t hpet_end = nano_time();
        if (hpet_end <= hpet_start || tsc_end <= tsc_start) continue;

        uint64_t hpet_elapsed = hpet_end - hpet_start;
        uint64_t tsc_elapsed  = tsc_end - tsc_start;
        uint64_t frequency    = (tsc_elapsed * 1000000000ULL) / hpet_elapsed;
        if (!frequency) continue;

        total_frequency += frequency;
        valid_runs++;
    }

    if (!valid_runs) {
        plogk("tsc: HPET calibration failed.\n");
        return 0;
    }

    tsc_frequency = total_frequency / valid_runs;
    if (!tsc_frequency) return 0;

    /* Q32 fixed-point nanoseconds per TSC cycle for the hot read path. */
    tsc_ns_ratio = (1000000000ULL << 32) / tsc_frequency;

    plogk("tsc: calibrated frequency = %lu MHz (%u/%u valid samples)\n", tsc_frequency / 1000000ULL, valid_runs, calibration_runs);
    return tsc_frequency;
}

/* Get TSC frequency (Hz). */
uint64_t tsc_get_cpu_frequency(void)
{
    return tsc_frequency;
}

/* Whether TSC is safe and fully calibrated for CLOCK_MONOTONIC use. */
int tsc_clocksource_available(void)
{
    return __atomic_load_n(&tsc_clocksource_ok, __ATOMIC_ACQUIRE) != 0;
}

/* Nominal TSC clocksource resolution, rounded up to a whole nanosecond. */
uint64_t tsc_resolution_ns(void)
{
    if (!tsc_frequency) return 0;
    uint64_t resolution = 1000000000ULL / tsc_frequency;
    if (1000000000ULL % tsc_frequency) resolution++;
    return resolution ? resolution : 1;
}

/*
 * Return TSC time on the same boot-relative epoch as HPET.  The epoch is
 * captured after calibration instead of treating tsc_init() as time zero;
 * this keeps TSC, HPET and DRM presentation timestamps interchangeable.
 */
uint64_t tsc_nano_time(void)
{
    if (!tsc_frequency || !tsc_epoch_value) return 0;

    uint64_t current_tsc = rdtsc_serialized();
    if (current_tsc < tsc_epoch_value) return tsc_epoch_ns;

    uint64_t elapsed_tsc = current_tsc - tsc_epoch_value;
    uint64_t elapsed_ns  = (uint64_t)(((__uint128_t)elapsed_tsc * tsc_ns_ratio) >> 32);
    if (elapsed_ns > UINT64_MAX - tsc_epoch_ns) return UINT64_MAX;
    return tsc_epoch_ns + elapsed_ns;
}

/* Initialize and, when safe, select TSC as the high-resolution clocksource. */
void tsc_init(void)
{
    if (!cpu_support_rdtsc()) {
        plogk("tsc: TSC not supported by CPU; HPET will remain the clocksource.\n");
        return;
    }

    tsc_invariant = tsc_check_invariant();
    plogk("tsc: rdtscp is %s.\n", cpu_support_rdtscp() ? "supported" : "not supported");
    plogk("tsc: invariant TSC is %s.\n", tsc_invariant ? "supported" : "not supported");

    if (!tsc_calibrate_with_hpet(get_acpi_hpet())) {
        plogk("tsc: calibration unavailable; HPET will remain the clocksource.\n");
        return;
    }

    /*
     * Bracket the serialized TSC read with HPET reads and use their midpoint.
     * This aligns the TSC epoch to HPET's boot-relative epoch while bounding
     * MMIO-read latency error instead of silently introducing a new epoch.
     */
    uint64_t hpet_before = nano_time();
    uint64_t tsc_epoch   = rdtsc_serialized();
    uint64_t hpet_after  = nano_time();

    tsc_epoch_value = tsc_epoch;
    tsc_epoch_ns    = hpet_before + (hpet_after - hpet_before) / 2ULL;

    if (!tsc_invariant) {
        plogk("tsc: calibrated but not invariant; keeping HPET as CLOCK_MONOTONIC source.\n");
        return;
    }

    __atomic_store_n(&tsc_clocksource_ok, 1, __ATOMIC_RELEASE);
    plogk("tsc: selected as CLOCK_MONOTONIC clocksource (epoch=%lu ns, resolution=%lu ns).\n", tsc_epoch_ns, tsc_resolution_ns());
}
