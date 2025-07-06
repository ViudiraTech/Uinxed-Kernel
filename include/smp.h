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

void smp_init(void);
void ap_entry(struct limine_smp_info *info);
uint32_t get_cpu_count(void);
uint32_t get_current_cpu_id(void);
void send_ipi_cpu(uint32_t cpu_id, uint8_t vector);

#endif // INCLUDE_SMP_H_