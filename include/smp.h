/*
 *
 *      smp.h
 *      Symmetric Multi-Processing (SMP) support
 *
 *      2025/7/6 By W9pi3cZ1
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SMP_H_
#define INCLUDE_SMP_H_

#include "limine.h"
#include "stdint.h"
typedef struct cpu_processor {
        uint64_t id;
        uint64_t lapic_id;
        uint64_t stack;
        uint64_t gdt;
        uint64_t tss;
} cpu_processor;

/* Initialize SMP */
void smp_init(void);

/* Initialize the AP */
void ap_entry(struct limine_smp_info *info);

/* Get the number of CPUs */
uint32_t get_cpu_count(void);

/* Get the current CPU ID */
uint32_t get_current_cpu_id(void);

/* Send an IPI to a specific CPU */
void send_ipi_cpu(uint32_t cpu_id, uint8_t vector);

#endif // INCLUDE_SMP_H_