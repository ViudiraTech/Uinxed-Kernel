/*
 *
 *      smp.h
 *      Symmetric Multi-Processing Header File
 *
 *      2025/7/6 By W9pi3cZ1
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SMP_H_
#define INCLUDE_SMP_H_

#include "gdt.h"
#include "limine.h"
#include "stdint.h"

typedef struct cpu_processor {
        uint64_t id;
        uint64_t lapic_id;
        struct {
                gdt_entries_t entries;
                uint16_t limit;
                uint64_t base;
        } gdt __attribute__((aligned(16)));
        struct gdt_register gdt_pointer;
        tss_stack_t *tss_stack;
        tss_t *tss;
} cpu_processor;

/* Send an IPI to all CPUs */
void send_ipi_all(uint8_t vector);

/* Send an IPI to the specified CPU */
void send_ipi_cpu(uint32_t cpu_id, uint8_t vector);

/* Flush TLBs of all CPUs */
void flush_tlb_all(void);

/* Flushing TLB by address range */
void flush_tlb_range(uint64_t start, uint64_t end);

/* Get the number of CPUs */
uint32_t get_cpu_count(void);

/* Get the ID of the current CPU */
uint32_t get_current_cpu_id(void);

/* Multi-core boot entry */
void ap_entry(struct limine_smp_info *info);

/* Initializing Symmetric Multi-Processing */
void smp_init(void);

#endif // INCLUDE_SMP_H_
