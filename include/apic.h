/*
 *
 *		apic.h
 *		Advanced Programmable Interrupt Controller Header Files
 *
 *		2025/2/17 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
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

#define LAPIC_REG_ID			32
#define LAPIC_REG_TIMER_CURCNT	0x390
#define LAPIC_REG_TIMER_INITCNT 0x380
#define LAPIC_REG_TIMER			0x320
#define LAPIC_REG_SPURIOUS		0xf0
#define LAPIC_REG_TIMER_DIV		0x3e0

#define APIC_ICR_LOW  0x300
#define APIC_ICR_HIGH 0x310

typedef struct {
	struct ACPISDTHeader h;
	uint32_t			 local_apic_address;
	uint32_t			 flags;
	void				*entries;
} __attribute__((packed)) MADT;

struct madt_hander {
	uint8_t entry_type;
	uint8_t length;
} __attribute__((packed));

struct madt_io_apic {
	struct madt_hander h;
	uint8_t			   apic_id;
	uint8_t			   reserved;
	uint32_t		   address;
	uint32_t		   gsib;
} __attribute__((packed));

struct madt_local_apic {
	struct madt_hander h;
	uint8_t			   ACPI_Processor_UID;
	uint8_t			   local_apic_id;
	uint32_t		   flags;
};

typedef struct madt_hander	   MadtHeader;
typedef struct madt_io_apic	   MadtIOApic;
typedef struct madt_local_apic MadtLocalApic;

/* Turn off PIC */
void disable_pic(void);

/* Write I/O APIC register */
void ioapic_write(uint32_t reg, uint32_t value);

/* Read I/O APIC registers */
uint32_t ioapic_read(uint32_t reg);

/* Configuring I/O APIC interrupt routing */
void ioapic_add(uint8_t vector, uint32_t irq);

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
