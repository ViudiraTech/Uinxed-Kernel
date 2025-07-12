/*
 *
 *      apic.h
 *      Advanced Programmable Interrupt Controller Header Files
 *
 *      2025/2/17 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_APIC_H_
#define INCLUDE_APIC_H_

#include "acpi.h"
#include "stdint.h"

#define MADT_APIC_CPU 0x00
#define MADT_APIC_IO  0x01
#define MADT_APIC_INT 0x02
#define MADT_APIC_NMI 0x03

#define LAPIC_REG_ID            32
#define LAPIC_REG_TIMER_CURCNT  0x390
#define LAPIC_REG_TIMER_INITCNT 0x380
#define LAPIC_REG_TIMER         0x320
#define LAPIC_REG_SPURIOUS      0xf0
#define LAPIC_REG_TIMER_DIV     0x3e0

#define APIC_ICR_LOW  0x300
#define APIC_ICR_HIGH 0x310

#define IPI_RESCHEDULE    0x52
#define IPI_HALT          0x53
#define IPI_TLB_SHOOTDOWN 0x54
#define IPI_PANIC         0x55

#define IPI_FIXED         0x0
#define IPI_LOWEST        0x100
#define IPI_SMI           0x200
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
        struct ACPISDTHeader h;
        uint32_t local_apic_address;
        uint32_t flags;
        void *entries;
} __attribute__((packed)) MADT;

struct madt_header {
        uint8_t entry_type;
        uint8_t length;
} __attribute__((packed));

struct madt_io_apic {
        struct madt_header h;
        uint8_t apic_id;
        uint8_t reserved;
        uint32_t address;
        uint32_t gsib;
} __attribute__((packed));

struct madt_local_apic {
        struct madt_header h;
        uint8_t ACPI_Processor_UID;
        uint8_t local_apic_id;
        uint32_t flags;
};

typedef struct {
        uint8_t vector;
        uint32_t irq;
} ioapic_routing;

typedef struct madt_header MadtHeader;
typedef struct madt_io_apic MadtIOApic;
typedef struct madt_local_apic MadtLocalApic;

/* Turn off PIC */
void disable_pic(void);

/* Write I/O APIC register */
void ioapic_write(uint32_t reg, uint32_t value);

/* Read I/O APIC registers */
uint32_t ioapic_read(uint32_t reg);

/* Configuring I/O APIC interrupt routing */
void ioapic_add(ioapic_routing *routing);

/* Write local APIC register */
void lapic_write(uint32_t reg, uint32_t value);

/* Read local APIC register */
uint32_t lapic_read(uint32_t reg);

/* Get the local APIC ID of the current processor */
uint64_t lapic_id(void);

/* Initialize local APIC */
void local_apic_init(void);

/* Initialize I/O APIC */
void io_apic_init(void);

/* Send EOI signal */
void send_eoi(void);

/* Stop the local APIC timer */
void lapic_timer_stop(void);

/* Send interrupt handling instruction */
void send_ipi(uint32_t apic_id, uint32_t command);

/* Initialize APIC */
void apic_init(MADT *madt);

#endif // INCLUDE_APIC_H_
