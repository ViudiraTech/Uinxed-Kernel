/*
 *
 *      apic.c
 *      Advanced Programmable Interrupt Controller
 *
 *      2025/2/17 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "apic.h"
#include "acpi.h"
#include "common.h"
#include "hhdm.h"
#include "idt.h"
#include "limine.h"
#include "printk.h"
#include "stddef.h"
#include "stdint.h"

int x2apic_mode;

PointerCast lapic_ptr;
PointerCast ioapic_ptr;

__attribute__((used, section(".limine_requests"))) volatile struct limine_smp_request smp_request = {
    .id       = LIMINE_SMP_REQUEST,
    .revision = 0,
    .response = 0,
    .flags    = 1,
};

/* Turn off PIC */
void disable_pic(void)
{
    outb(0x21, 0xff);
    outb(0xa1, 0xff);
}

/* Write I/O APIC register */
void ioapic_write(uint32_t reg, uint32_t value)
{
    mmio_write32(ioapic_ptr.ptr, reg);
    PointerCast reg_ptr;
    reg_ptr.val = ioapic_ptr.val + 0x10;
    mmio_write32(reg_ptr.ptr, value);
}

/* Read I/O APIC registers */
uint32_t ioapic_read(uint32_t reg)
{
    mmio_write32(ioapic_ptr.ptr, reg);
    PointerCast reg_ptr;
    reg_ptr.val = ioapic_ptr.val + 0x10;
    return mmio_read32(reg_ptr.ptr);
}

/* Configuring I/O APIC interrupt routing */
void ioapic_add(ioapic_routing *routing)
{
    uint32_t ioredtbl = (uint32_t)(0x10 + (uint32_t)(routing->irq * 2));
    uint64_t redirect = routing->vector;
    redirect |= lapic_id() << 56;
    ioapic_write(ioredtbl, (uint32_t)redirect);
    ioapic_write(ioredtbl + 1, (uint32_t)(redirect >> 32));
}

/* Write local APIC register */
void lapic_write(uint32_t reg, uint32_t value)
{
    if (x2apic_mode) {
        wrmsr(0x800 + (reg >> 4), value);
        return;
    }
    PointerCast reg_ptr;
    reg_ptr.val = lapic_ptr.val + reg;
    mmio_write32(reg_ptr.ptr, value);
}

/* Read local APIC register */
uint32_t lapic_read(uint32_t reg)
{
    if (x2apic_mode) return rdmsr(0x800 + (reg >> 4));
    PointerCast reg_ptr;
    reg_ptr.val = lapic_ptr.val + reg;
    return mmio_read32(reg_ptr.ptr);
}

/* Get the local APIC ID of the current processor */
uint64_t lapic_id(void)
{
    return lapic_read(LAPIC_REG_ID);
}

/* Initialize local APIC */
void local_apic_init(void)
{
    x2apic_mode = (smp_request.response->flags & 1) != 0;

    if (x2apic_mode)
        plogk("ACPI: LAPIC = x2APIC\n");
    else
        plogk("ACPI: LAPIC = xAPIC\n");

    lapic_write(LAPIC_REG_SPURIOUS, 0xff | 1 << 8);
    lapic_write(LAPIC_REG_TIMER, IRQ_32);
    lapic_write(LAPIC_REG_TIMER_DIV, 11);
    lapic_write(LAPIC_REG_TIMER_INITCNT, ~((uint32_t)0));

    uint64_t start = nano_time();
    while (nano_time() - start < 1000000);

    uint64_t lapic_timer              = (~(uint32_t)0) - lapic_read(LAPIC_REG_TIMER_CURCNT);
    uint64_t calibrated_timer_initial = (uint64_t)((uint64_t)(lapic_timer * 1000) / 250);

    lapic_write(LAPIC_REG_TIMER, lapic_read(LAPIC_REG_TIMER) | 1 << 17);
    lapic_write(LAPIC_REG_TIMER_INITCNT, calibrated_timer_initial);
}

/* Initialize I/O APIC */
void io_apic_init(void)
{
    ioapic_routing *ioapic_router[] = {
        &(ioapic_routing) {IRQ_32, 0 }, // Timer
        &(ioapic_routing) {IRQ_33, 1 }, // Keyboard
        &(ioapic_routing) {IRQ_34, 12}, // Mouse
        &(ioapic_routing) {IRQ_46, 14}, // IDE0
        &(ioapic_routing) {IRQ_47, 15}, // IDE1
        NULL,
    };

    ioapic_routing **routing = ioapic_router;

    while (*routing != NULL) {
        ioapic_add(*routing);
        routing++;
    }
}

/* Send EOI signal */
void send_eoi(void)
{
    lapic_write(0xb0, 0);
}

/* Stop the local APIC timer */
void lapic_timer_stop(void)
{
    lapic_write(LAPIC_REG_TIMER_INITCNT, 0);
    lapic_write(LAPIC_REG_TIMER, (1 << 16));
}

/* Send interrupt handling instruction */
void send_ipi(uint32_t apic_id, uint32_t command)
{
    if (x2apic_mode) {
        lapic_write(APIC_ICR_LOW, (((uint64_t)apic_id) << 32) | command);
    } else {
        lapic_write(APIC_ICR_HIGH, apic_id << 24);
        lapic_write(APIC_ICR_LOW, command);
    }
}

/* Initialize APIC */
void apic_init(MADT *madt)
{
    lapic_ptr.ptr = phys_to_virt(madt->local_apic_address);
    plogk("ACPI: LAPIC Base address %p\n", lapic_ptr.ptr);

    uint8_t *entries_base = (uint8_t *)&madt->entries;
    size_t current        = 0;

    while (current < madt->h.Length - sizeof(MADT)) {
        MadtHeader *header = (MadtHeader *)(entries_base + current);
        if (header->entry_type == MADT_APIC_IO) {
            MadtIOApic *ioapic = (MadtIOApic *)(entries_base + current);
            ioapic_ptr.ptr     = phys_to_virt(ioapic->address);
            plogk("ACPI: IOAPIC Found at address %p\n", ioapic_ptr.ptr);
        }
        current += header->length;
    }

    disable_pic();
    local_apic_init();
    io_apic_init();
}
