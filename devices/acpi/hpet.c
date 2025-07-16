/*
 *
 *      hpet.c
 *      High-precision event timer
 *
 *      2025/2/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "acpi.h"
#include "hhdm.h"
#include "idt.h"
#include "printk.h"
#include "stdint.h"

HpetInfo *hpet_addr;
static uint32_t hpetPeriod = 0;

void timer_handle(interrupt_frame_t *frame);

/* Returns the nanosecond value of the current time */
uint64_t nano_time(void)
{
    if (!hpet_addr) return 0;
    return (hpet_addr->mainCounterValue) * hpetPeriod;
}

/* Initialize high-precision event timer */
void hpet_init(Hpet *hpet)
{
    hpet_addr = phys_to_virt(hpet->base_address.address);
    plogk("hpet: HPET Base address mapped to virtual address %p\n", hpet_addr);

    uint32_t counterClockPeriod = hpet_addr->generalCapabilities >> 32;
    hpetPeriod                  = counterClockPeriod / 1000000;
    hpet_addr->mainCounterValue = 0;

    plogk("hpet: HPET Main counter is initialized to 0\n");
    plogk("hpet: HPET Counter Clock Period = %u (ns)\n", counterClockPeriod);
    plogk("hpet: HPET Timer Period = %u (us)\n", hpetPeriod);

    hpet_addr->generalConfiguration |= 1;
    register_interrupt_handler(IRQ_0, (void *)timer_handle, 0, 0x8e);
    plogk("hpet: HPET General Configuration Register set to 0x%08llx\n", hpet_addr->generalConfiguration);
}
