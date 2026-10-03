/*
 *
 *      gdt.h
 *      Global descriptor header file
 *
 *      2024/6/27 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_GDT_H_
#define INCLUDE_GDT_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

typedef struct {
        uint16_t size;
        void    *ptr;
} __attribute__((packed)) gdt_register_t;

/* The exact 10-byte operand lgdt expects: 2-byte limit followed by a 64-bit base. */
_Static_assert(sizeof(gdt_register_t) == 10, "gdt_register_t descriptor size");
_Static_assert(offsetof(gdt_register_t, ptr) == 2, "gdt_register_t base offset");

typedef uint64_t gdt_entries_t[10];

typedef struct {
        gdt_entries_t  entries;
        gdt_register_t pointer;
} __attribute__((aligned(16))) gdt_t;

_Static_assert(sizeof(gdt_t) == 96, "gdt_t size (10 entries plus the 10-byte pointer, 16-byte aligned)");
_Static_assert(_Alignof(gdt_t) == 16, "gdt_t alignment");

extern gdt_t gdt0;

/* Initialize the global descriptor table */
void init_gdt(void);

#endif // INCLUDE_GDT_H_
