/*
 *
 *		gdt.h
 *		Global Descriptor Header File
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_GDT_H_
#define INCLUDE_GDT_H_

#include "stdint.h"

struct gdt_register {
		uint16_t size;
		void *ptr;
} __attribute__((packed));

typedef struct tss {
		uint32_t unused0;
		uint64_t rsp[3];
		uint64_t unused1;
		uint64_t ist[7];
		uint64_t unused2;
		uint16_t unused3;
		uint16_t iopb;
} __attribute__((packed)) tss_t;

typedef uint8_t tss_stack_t[1024];
typedef uint64_t gdt_entries_t[7];

/* Initialize the global descriptor table */
void init_gdt(void);

/* Initialize TSS */
void tss_init(void);

/* Setting up the kernel stack */
void set_kernel_stack(uint64_t rsp);

#endif // INCLUDE_GDT_H_
