/*
 *
 *      apic.c
 *      Advanced programmable interrupt controller
 *
 *      2025/2/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/cpuid.h>
#include <arch/idt.h>
#include <boot/limine.h>
#include <drivers/firmware/acpi.h>
#include <drivers/firmware/apic.h>
#include <drivers/time/tsc.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <kernel/uinxed.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/hhdm.h>

#define CPUID_FEAT_EDX_APIC   (1 << 9)
#define CPUID_FEAT_ECX_X2APIC (1 << 21)

int x2apic_mode = -1;

/*
 * LAPIC timer mode shared by every CPU (written once on the BSP before APs
 * boot, read from timer IRQ context): -1 undetermined, 0 legacy calibrated
 * periodic, 1 TSC-deadline one-shot.
 */
int apic_lapic_timer_mode = -1;

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
        uint32_t eax, ebx, ecx, edx;
        cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
        if (!(edx & CPUID_FEAT_EDX_APIC)) {
            plogk("apic: Local APIC not supported.\n");
            return;
        }
        x2apic_mode = smp_request.response && (smp_request.response->flags & 1) && (ecx & CPUID_FEAT_ECX_X2APIC);
        plogk("apic: Local APIC: %s\n", x2apic_mode ? "x2APIC" : "xAPIC");

        /*
         * CPUID.01H:ECX[24] TSC_DEADLINE: the LVTT can select deadline mode,
         * where a write of an absolute TSC value to IA32_TSC_DEADLINE arms a
         * one-shot interrupt (Linux "lapic-deadline" clockevent).  Requires
         * the calibrated TSC frequency; tsc_init() runs before SMP boot.
         */
        uint32_t eax2, ebx2, ecx2, edx2;
        cpuid(0x00000001, &eax2, &ebx2, &ecx2, &edx2);
        apic_lapic_timer_mode = ((ecx2 >> 24) & 1U) && tsc_get_cpu_frequency() != 0 ? 1 : 0;
        plogk("apic: LAPIC timer mode: %s\n", apic_lapic_timer_mode == 1 ? "TSC-deadline one-shot" : "calibrated periodic");
    }

    lapic_write(LAPIC_REG_SPURIOUS, 0xff | 1 << 8);

    /*
     * Mask the external LVT entries (LINT0=0x350, LINT1=0x370).  On reset
     * LINT1 is programmed for NMI delivery; if the pin floats or a device
     * asserts it, a spurious NMI fires (observed as a "Kernel fatal error:
     * NMI" panic as soon as userspace starts and devices go active).  The
     * TLB-shootdown NMI used by flush_tlb_all() is delivered through the
     * ICR, not these pins, so masking them is safe.
     */
    lapic_write(0x350, 1U << 16); // LVT LINT0: masked
    lapic_write(0x370, 1U << 16); // LVT LINT1: masked

    if (apic_lapic_timer_mode == 1) {
        /*
         * Deadline mode: no counter calibration is needed and every tick is
         * exactly TIMER_TICK_NS regardless of the LAPIC input clock.  The
         * handler re-arms via lapic_timer_rearm_tick().  The divide-configuration
         * register is ignored in this mode.
         *
         * In xAPIC mode the LVTT write and the TSC_DEADLINE MSR write are not
         * serialized with each other; order them LVTT-first like Linux's
         * __setup_APIC_LVTT so the MSR write always lands after the mode is
         * selected.
         */
        lapic_write(LAPIC_REG_TIMER_DIV, 11);
        lapic_write(LAPIC_REG_TIMER, IRQ_0 | APIC_LVT_TSC_DEADLINE);
        wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc_serialized() + tsc_get_cpu_frequency() / TIMER_HZ);
        return;
    }

    lapic_write(LAPIC_REG_TIMER, IRQ_0);
    lapic_write(LAPIC_REG_TIMER_DIV, 11);
    lapic_write(LAPIC_REG_TIMER_INITCNT, ~((uint32_t)0));

    /* Let the counter free-run for 1 ms to calibrate the timer frequency. */
    for (uint64_t start = nano_time(); nano_time() - start < 1000000;);

    uint64_t lapic_timer              = (~(uint32_t)0) - lapic_read(LAPIC_REG_TIMER_CURCNT);
    uint64_t calibrated_timer_initial = (uint64_t)((uint64_t)(lapic_timer * 1000) / TIMER_HZ);

    lapic_write(LAPIC_REG_TIMER, lapic_read(LAPIC_REG_TIMER) | APIC_LVT_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_INITCNT, calibrated_timer_initial);
}

/* Whether this CPU's LAPIC timer runs in TSC-deadline one-shot mode */
int lapic_timer_is_tsc_deadline(void)
{
    /*
     * Written once on the BSP before APs boot and never changed afterwards;
     * timer IRQ context reads it without locking.
     */
    return __atomic_load_n(&apic_lapic_timer_mode, __ATOMIC_RELAXED) == 1;
}

/* Re-arm the LAPIC timer for the next periodic tick (TSC-deadline mode only) */
void lapic_timer_rearm_tick(void)
{
    if (!lapic_timer_is_tsc_deadline()) return;
    /* Absolute TSC deadline one tick out; a late re-arm fires immediately, self-correcting drift. */
    wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc_serialized() + tsc_get_cpu_frequency() / TIMER_HZ);
}

/*
 * Switch the BSP's LAPIC timer to TSC-deadline mode once tsc_init() has
 * produced a calibrated frequency.
 *
 * acpi_init() arms the BSP timer before tsc_init() runs, so local_apic_init()
 * always sees an uncalibrated TSC on its first call and selects legacy
 * periodic mode.  This must be invoked between tsc_init() and smp_init():
 * APs then observe the upgraded mode and program deadline directly, skipping
 * their own calibration busy-wait entirely.
 */
void lapic_timer_try_upgrade(void)
{
    if (__atomic_load_n(&apic_lapic_timer_mode, __ATOMIC_RELAXED) == 1) return;
    if (!tsc_get_cpu_frequency()) return;

    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    if (!((ecx >> 24) & 1U)) {
        plogk("apic: TSC-deadline mode not supported by this CPU; staying periodic.\n");
        return;
    }

    /*
     * Publish the new mode BEFORE reprogramming: the timer IRQ handler reads
     * it via lapic_timer_is_tsc_deadline() to decide whether to re-arm.  The
     * BSP timer may fire during the switch (interrupts are still disabled or
     * the vector not yet unmasked this early), and a handler racing here with
     * the old periodic hardware simply finds the deadline MSR armed and no
     * pending INITCNT - both states produce exactly one tick.
     */
    __atomic_store_n(&apic_lapic_timer_mode, 1, __ATOMIC_RELEASE);

    /* Disarm the legacy counter, then select deadline mode and arm one tick out. */
    lapic_write(LAPIC_REG_TIMER_INITCNT, 0);
    lapic_write(LAPIC_REG_TIMER_DIV, 11);
    lapic_write(LAPIC_REG_TIMER, IRQ_0 | APIC_LVT_TSC_DEADLINE);
    wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc_serialized() + tsc_get_cpu_frequency() / TIMER_HZ);

    plogk("apic: LAPIC timer upgraded to TSC-deadline one-shot (%llu Hz TSC)\n", (unsigned long long)tsc_get_cpu_frequency());
}

/* Initialize I/O APIC */
void io_apic_init(void)
{
    ioapic_routing_t *ioapic_router[] = {
        &(ioapic_routing_t) {IRQ_0,  0 }, // Timer IRQ_0 = 32
        &(ioapic_routing_t) {IRQ_1,  1 }, // Keyboard IRQ_1 = 33
        &(ioapic_routing_t) {IRQ_3,  3 }, // Serial COM2/COM4 IRQ_3 = 35
        &(ioapic_routing_t) {IRQ_4,  4 }, // Serial COM1/COM3 IRQ_4 = 36
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
    if (lapic_timer_is_tsc_deadline()) {
        /* A deadline in the past disarms the timer; the LVT mask below keeps it silent. */
        wrmsr(MSR_IA32_TSC_DEADLINE, 0);
        lapic_write(LAPIC_REG_TIMER, IRQ_0 | APIC_LVT_TSC_DEADLINE | APIC_LVT_MASKED);
        return;
    }
    lapic_write(LAPIC_REG_TIMER_INITCNT, 0);
    lapic_write(LAPIC_REG_TIMER, IRQ_0 | APIC_LVT_MASKED);
}

/* Send interrupt handling instruction */
void send_ipi(uint32_t apic_id, uint32_t command)
{
    if (x2apic_mode) {
        /*
         * IA32_X2APIC_ICR carries the complete 32-bit destination in
         * bits 63:32.  Limiting it to four bits routes an IPI for APIC IDs
         * 16, 32, ... to the wrong logical CPU (or to nobody), which makes
         * the TLB shootdown wait forever on larger SMP machines.
         */
        wrmsr(0x800 + (APIC_ICR_LOW >> 4), ((uint64_t)apic_id << 32) | command);
    } else {
        lapic_write(APIC_ICR_HIGH, apic_id << 24);
        lapic_write(APIC_ICR_LOW, command);
    }
    int tout = 1000000;
    while (lapic_read(APIC_ICR_LOW) & (1 << 12)) {
        if (--tout <= 0) {
            plogk("apic: IPI to APIC %u still pending (ICR=0x%x)\n", apic_id, command);
            return;
        }
    }
}

/* Initialize APIC */
void apic_init(madt_t *madt)
{
    if (!madt) return;
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
                plogk("apic: Local X2 APIC id %03u, ACPI processor uid %03u, Flags %x\n", x2cpu->local_x2_apic_id, x2cpu->acpi_processor_uid, x2cpu->flags);
                break;
            }
            case MADT_APIC_IO_INT : {
                madt_io_apic_int_t *int_override = (madt_io_apic_int_t *)(entries_base + current);
                plogk("apic: IO/APIC interrupt source override: bus %u, source %u -> GSI %u, flags %x\n", int_override->bus, int_override->source, int_override->global_system_interrupt,
                      int_override->flags);
                break;
            }
            case MADT_APIC_IO_NMI : {
                madt_io_apic_nmi_t *nmi = (madt_io_apic_nmi_t *)(entries_base + current);
                plogk("apic: IO/APIC NMI: ACPI processor uid %u, flags %x, LINT %u\n", nmi->acpi_processor_uid, nmi->flags, nmi->lint);
                break;
            }
            default :
                /* Unhandled MADT entry type (Maybe it's reserved) */
                break;
        }
        current += header->length;
    }
    if (!ioapic_ptr.ptr) {
        plogk("apic: IOAPIC not found.\n");
        return;
    }
    disable_pic();
    local_apic_init();
    io_apic_init();
}
