/*
 *
 *      apic.h
 *      Advanced programmable interrupt controller header files
 *
 *      2025/2/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_APIC_H_
#define INCLUDE_APIC_H_

#include <drivers/firmware/acpi.h>
#include <libs/std/stdint.h>

#define MADT_APIC_LOCAL_CPU    0x00
#define MADT_APIC_IO           0x01
#define MADT_APIC_IO_INT       0x02
#define MADT_APIC_IO_NMI       0x03
#define MADT_APIC_LOCAL_NMI    0x04
#define MADT_APIC_LOCAL_ADDR   0x05
#define MADT_APIC_LOCAL_X2_CPU 0x09

#define LAPIC_REG_ID            0x20
#define LAPIC_REG_TIMER_CURCNT  0x390
#define LAPIC_REG_TIMER_INITCNT 0x380
#define LAPIC_REG_TIMER         0x320
#define LAPIC_REG_SPURIOUS      0xf0
#define LAPIC_REG_TIMER_DIV     0x3e0

/* LVT timer mode bits */
#define APIC_LVT_MASKED       (1U << 16)
#define APIC_LVT_PERIODIC     (1U << 17)
#define APIC_LVT_TSC_DEADLINE (1U << 18)

/* IA32_TSC_DEADLINE: one-shot deadline in absolute TSC cycles (SDM Ch.10.5.4) */
#define MSR_IA32_TSC_DEADLINE 0x6e0

#define APIC_ICR_LOW  0x300
#define APIC_ICR_HIGH 0x310

#define IPI_RESCHEDULE    0x52
#define IPI_HALT          0x53
#define IPI_TLB_SHOOTDOWN 0x54
#define IPI_PANIC         0x55

#define IPI_FIXED         0x0
#define IPI_LOWEST        0x100
#define IPI_SMI           0x200
#define IPI_NMI           0x400
#define IPI_REMOTE        0x4000
#define IPI_EDGE          0x8000
#define IPI_DEASSERT      0x0
#define IPI_ASSERT        0x4000
#define IPI_PHYSICAL      0x0
#define IPI_LOGICAL       0x800
#define APIC_ICR_INIT     0x4500
#define APIC_ICR_STARTUP  0x4600
#define APIC_ICR_PHYSICAL 0x0

typedef struct {
        acpi_sdt_header_t header;
        uint32_t          local_apic_address;
        uint32_t          flags;
        void             *entries;
} __attribute__((packed)) madt_t;

typedef struct {
        uint8_t entry_type;
        uint8_t length;
} __attribute__((packed)) madt_header_t;

typedef struct {
        madt_header_t header;
        uint8_t       apic_id;
        uint8_t       reserved;
        uint32_t      address;
        uint32_t      gsib;
} __attribute__((packed)) madt_io_apic_t;

typedef struct {
        madt_header_t header;
        uint8_t       acpi_processor_uid;
        uint8_t       local_apic_id;
        uint32_t      flags;
} __attribute__((packed)) madt_local_apic_t;

typedef struct {
        madt_header_t header;
        uint16_t      reserved;
        uint64_t      address;
} __attribute__((packed)) madt_local_apic_addr_t;

typedef struct {
        madt_header_t header;
        uint16_t      reserved;
        uint32_t      local_x2_apic_id;
        uint32_t      flags;
        uint32_t      acpi_processor_uid;
} __attribute__((packed)) madt_local_x2_cpu_t;

typedef struct {
        madt_header_t header;
        uint8_t       bus;
        uint8_t       source;
        uint32_t      global_system_interrupt;
        uint16_t      flags;
} __attribute__((packed)) madt_io_apic_int_t;

typedef struct {
        madt_header_t header;
        uint8_t       acpi_processor_uid;
        uint16_t      flags;
        uint8_t       lint;
} __attribute__((packed)) madt_io_apic_nmi_t;

typedef struct {
        uint8_t  vector;
        uint32_t irq;
} ioapic_routing_t;

/* Turn off PIC */
void disable_pic(void);

/* Write I/O APIC register */
void ioapic_write(uint32_t reg, uint32_t value);

/* Read I/O APIC registers */
uint32_t ioapic_read(uint32_t reg);

/* Configuring I/O APIC interrupt routing */
void ioapic_add(ioapic_routing_t *routing);

/* Write local APIC register */
void lapic_write(uint32_t reg, uint32_t value);

/* Read local APIC register */
uint32_t lapic_read(uint32_t reg);

/* Get the local APIC ID of the current processor */
uint64_t lapic_id(void);

/* Initialize local APIC */
void local_apic_init(void);

/* Whether this CPU's LAPIC timer runs in TSC-deadline one-shot mode */
int lapic_timer_is_tsc_deadline(void);

/* Re-arm the LAPIC timer for the next periodic tick (TSC-deadline mode only; no-op in legacy periodic mode) */
void lapic_timer_rearm_tick(void);

/*
 * Switch the BSP LAPIC timer to TSC-deadline mode after tsc_init(); must run
 * between tsc_init() and smp_init() so APs boot directly into deadline mode.
 */
void lapic_timer_try_upgrade(void);

/* Initialize I/O APIC */
void io_apic_init(void);

/* Send EOI signal */
void send_eoi(void);

/* Stop the local APIC timer */
void lapic_timer_stop(void);

/* Send interrupt handling instruction */
void send_ipi(uint32_t apic_id, uint32_t command);

/* Initialize APIC */
void apic_init(madt_t *madt);

#endif // INCLUDE_APIC_H_
