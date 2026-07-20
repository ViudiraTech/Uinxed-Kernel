/*
 *
 *      gdt.h
 *      Global descriptor header file
 *
 *      2024/6/27 By Rainy101112
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

typedef uint64_t gdt_entries_t[8];

typedef struct {
        gdt_entries_t  entries;
        gdt_register_t pointer;
} __attribute__((aligned(16))) gdt_t;

extern gdt_t gdt0;

/* Initialize the global descriptor table */
void init_gdt(void);

#endif // INCLUDE_GDT_H_
