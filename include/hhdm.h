/*
 *
 *		hhdm.h
 *		High half memory map header file
 *
 *		2025/2/16 By XIAOYI12
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_HHDM_H_
#define INCLUDE_HHDM_H_

#include "stdint.h"

/* Initialize the upper half memory mapping */
void init_hhdm(void);

/* Get physical memory offset */
uint64_t get_physical_memory_offset(void);

/* Convert physical memory to virtual memory */
void *phys_to_virt(uint64_t phys_addr);

/* Convert virtual memory to physical memory */
void *virt_to_phys(uint64_t virt_addr);

#endif // INCLUDE_HHDM_H_
