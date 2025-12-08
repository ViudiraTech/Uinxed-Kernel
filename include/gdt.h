/*
 *
 *      gdt.h
 *      Global descriptor header file
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_GDT_H_
#define INCLUDE_GDT_H_

#include <stdint.h>

typedef struct {
        uint16_t size;
        void    *ptr;
} __attribute__((packed)) gdt_register_t;

typedef struct {
        uint32_t unused0;
        uint64_t rsp[3];
        uint64_t unused1;
        uint64_t ist[7];
        uint64_t unused2;
        uint16_t unused3;
        uint16_t iopb;
} __attribute__((packed)) tss_t;

typedef uint8_t  tss_stack_t[1024];
typedef uint64_t gdt_entries_t[8];

typedef struct {
        gdt_entries_t  entries;
        gdt_register_t pointer;
} __attribute__((aligned(16))) gdt_t;

extern gdt_t       gdt0;
extern tss_t       tss0;
extern tss_stack_t tss_stack;

/* Initialize the global descriptor table */
void init_gdt(void);

/* Initialize TSS */
void tss_init(void);

/* Setting up the kernel stack */
void set_kernel_stack(uint64_t rsp);

#endif // INCLUDE_GDT_H_
