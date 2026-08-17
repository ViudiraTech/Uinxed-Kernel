/*
 *
 *      hpet.c
 *      High-precision event timer
 *
 *      2025/2/16 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/idt.h>
#include <drivers/firmware/acpi.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdint.h>
#include <mem/hhdm.h>

hpet_info_t    *hpet_addr;
static uint64_t hpet_period_fs = 0;

/* Returns HPET time in nanoseconds from the HPET boot-relative epoch. */
uint64_t nano_time(void)
{
    if (!hpet_addr || !hpet_period_fs) return 0;

    uint64_t counter = hpet_addr->main_counter_value;

    /*
     * counter * period_fs / 1e6, without compiler-rt/libgcc 128-bit
     * division helpers. Splitting at 1e6 also keeps the remainder product
     * bounded for the HPET architectural period range.
     */
    uint64_t whole = counter / 1000000ULL;
    uint64_t rem   = counter % 1000000ULL;
    if (whole && hpet_period_fs > UINT64_MAX / whole) return UINT64_MAX;

    uint64_t ns   = whole * hpet_period_fs;
    uint64_t tail = (rem * hpet_period_fs) / 1000000ULL;
    if (tail > UINT64_MAX - ns) return UINT64_MAX;
    return ns + tail;
}

/* Whether HPET is mapped, enabled and has a valid period. */
int hpet_available(void)
{
    return hpet_addr && hpet_period_fs && (hpet_addr->general_configuration & 1ULL);
}

/* HPET resolution rounded up to a whole nanosecond. */
uint64_t hpet_resolution_ns(void)
{
    if (!hpet_period_fs) return 0;
    uint64_t ns = hpet_period_fs / 1000000ULL;
    if (hpet_period_fs % 1000000ULL) ns++;
    return ns ? ns : 1;
}

/* Get the HPET structure. */
hpet_info_t *get_acpi_hpet(void)
{
    return hpet_addr;
}

/* Initialize high-precision event timer. */
void hpet_init(hpet_t *hpet)
{
    hpet_addr = phys_to_virt(hpet->base_address.address);
    plogk("hpet: HPET base mapped to virtual address %p\n", hpet_addr);

    hpet_period_fs = hpet_addr->general_capabilities >> 32;
    hpet_addr->general_configuration &= ~1ULL;
    hpet_addr->main_counter_value = 0;

    plogk("hpet: HPET main counter is initialized to 0\n");
    plogk("hpet: HPET counter period = %lu fs (~%lu ns resolution)\n", hpet_period_fs, hpet_resolution_ns());

    hpet_addr->general_configuration |= 1ULL;
    register_interrupt_handler(IRQ_0, (void *)timer_handle, 0, 0x8e);
    plogk("hpet: HPET general configuration register set to 0x%08llx\n", hpet_addr->general_configuration);
}
