/*
 *
 *      apic.c
 *      Advanced programmable interrupt controller
 *
 *      2025/2/17 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <acpi.h>
#include <apic.h>
#include <common.h>
#include <hhdm.h>
#include <idt.h>
#include <limine.h>
#include <printk.h>
#include <stddef.h>
#include <stdint.h>
#include <uinxed.h>

int x2apic_mode = -1;

pointer_cast_t lapic_ptr;
pointer_cast_t ioapic_ptr;

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
    pointer_cast_t reg_ptr;
    reg_ptr.val = ioapic_ptr.val + 0x10;
    mmio_write32(reg_ptr.ptr, value);
}

/* Read I/O APIC registers */
uint32_t ioapic_read(uint32_t reg)
{
    mmio_write32(ioapic_ptr.ptr, reg);
    pointer_cast_t reg_ptr;
    reg_ptr.val = ioapic_ptr.val + 0x10;
    return mmio_read32(reg_ptr.ptr);
}

/* Configuring I/O APIC interrupt routing */
void ioapic_add(ioapic_routing_t *routing)
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
    pointer_cast_t reg_ptr;
    reg_ptr.val = (lapic_ptr.val + reg);
    mmio_write32(reg_ptr.ptr, value);
}

/* Read local APIC register */
uint32_t lapic_read(uint32_t reg)
{
    if (x2apic_mode) return (uint32_t)rdmsr(0x800 + (reg >> 4));
    pointer_cast_t reg_ptr;
    reg_ptr.val = lapic_ptr.val + reg;
    return mmio_read32(reg_ptr.ptr);
}

/* Get the local APIC ID of the current processor */
uint64_t lapic_id(void)
{
    if (x2apic_mode) return rdmsr(0x800 + (LAPIC_REG_ID >> 4));

    /* Must be shifted to the right by 24 bits (refer to the Intel SDM Vol.3 Chapter.12.4.6) */
    return lapic_read(LAPIC_REG_ID) >> 24;
}

/* Initialize local APIC */
void local_apic_init(void)
{
    if (x2apic_mode == -1) { // Run only once
        x2apic_mode = (smp_request.response->flags & 1) != 0;
        plogk("apic: Local APIC: %s\n", x2apic_mode ? "x2APIC" : "xAPIC");
    }

    lapic_write(LAPIC_REG_SPURIOUS, 0xff | 1 << 8);
    lapic_write(LAPIC_REG_TIMER, IRQ_0);
    lapic_write(LAPIC_REG_TIMER_DIV, 11);
    lapic_write(LAPIC_REG_TIMER_INITCNT, ~((uint32_t)0));

    for (uint64_t start = nano_time(); nano_time() - start < 1000000;);

    uint64_t lapic_timer              = (~(uint32_t)0) - lapic_read(LAPIC_REG_TIMER_CURCNT);
    uint64_t calibrated_timer_initial = (uint64_t)((uint64_t)(lapic_timer * 1000) / 250);

    lapic_write(LAPIC_REG_TIMER, lapic_read(LAPIC_REG_TIMER) | 1 << 17);
    lapic_write(LAPIC_REG_TIMER_INITCNT, calibrated_timer_initial);
}

/* Initialize I/O APIC */
void io_apic_init(void)
{
    ioapic_routing_t *ioapic_router[] = {
        &(ioapic_routing_t) {IRQ_0,  0 }, // Timer IRQ_0 = 32
        &(ioapic_routing_t) {IRQ_1,  1 }, // Keyboard IRQ_1 = 33
        &(ioapic_routing_t) {IRQ_12, 12}, // Mouse IRQ_12 = 44
        &(ioapic_routing_t) {IRQ_14, 14}, // IDE0 IRQ_14 = 46
        &(ioapic_routing_t) {IRQ_15, 15}, // IDE1 IRQ_15 = 47
        0,
    };

    ioapic_routing_t **routing = ioapic_router;

    while (*routing != 0) {
        ioapic_add(*routing);
        plogk("apic: IOAPIC has set up routing from Vector %03d --> IRQ %03d\n", (*routing)->vector, (*routing)->irq);
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
        wrmsr(0x800 + (APIC_ICR_LOW >> 4), ((uint64_t)(apic_id & 0b1111) << 32) | command);
    } else {
        lapic_write(APIC_ICR_HIGH, apic_id << 24);
        lapic_write(APIC_ICR_LOW, command);
    }
    while (lapic_read(APIC_ICR_LOW) & (1 << 12));
}

/* Initialize APIC */
void apic_init(madt_t *madt)
{
    lapic_ptr.ptr = phys_to_virt(madt->local_apic_address);
    plogk("apic: Local APIC base %p\n", lapic_ptr.ptr);

    uint8_t *entries_base = (uint8_t *)&madt->entries;
    size_t   current      = 0;

    while (current < madt->header.length - sizeof(madt_t)) {
        madt_header_t *header = (madt_header_t *)(entries_base + current);
        switch (header->entry_type) {
            case MADT_APIC_LOCAL_CPU : {
                madt_local_apic_t *cpu = (madt_local_apic_t *)(entries_base + current);
                plogk("apic: Local APIC id %03u, ACPI processor uid %03u, Flags %x\n", cpu->local_apic_id, cpu->acpi_processor_uid, cpu->flags);
                break;
            }
            case MADT_APIC_IO : {
                madt_io_apic_t *ioapic = (madt_io_apic_t *)(entries_base + current);
                ioapic_ptr.ptr         = phys_to_virt(ioapic->address);
                plogk("apic: IOAPIC found at %p\n", ioapic_ptr.ptr);
                break;
            }
            case MADT_APIC_LOCAL_ADDR : {
                madt_local_apic_addr_t *addr = (madt_local_apic_addr_t *)(entries_base + current);
                lapic_ptr.ptr                = phys_to_virt(addr->address);
                plogk("apic: Local APIC base is overwritten as %p\n", lapic_ptr);
                break;
            }
            case MADT_APIC_LOCAL_X2_CPU : {
                madt_local_x2_cpu_t *x2cpu = (madt_local_x2_cpu_t *)(entries_base + current);
                plogk("apic: Local X2 APIC id %03u, ACPI processor uid %03u, Flags %x\n", x2cpu->local_x2_apic_id, x2cpu->acpi_processor_uid,
                      x2cpu->flags);
                break;
            }
            case MADT_APIC_IO_INT : // TODO: Implement IO/APIC interrupt source override
            case MADT_APIC_IO_NMI : // TODO: Implement IO/APIC Non-maskable interrupt source
            default :
                /* Unhandled MADT entry type (Maybe it's reserved) */
                break;
        }
        current += header->length;
    }
    disable_pic();
    local_apic_init();
    io_apic_init();
}
