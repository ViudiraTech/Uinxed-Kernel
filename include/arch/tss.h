/*
 *
 *      tss.h
 *      Task state segment header file
 *
 *      2026/7/21 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TSS_H_
#define INCLUDE_TSS_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

typedef struct {
        uint32_t unused0;
        uint64_t rsp[3];
        uint64_t unused1;
        uint64_t ist[7];
        uint64_t unused2;
        uint16_t unused3;
        uint16_t iopb;
} __attribute__((packed)) tss_t;

_Static_assert(offsetof(tss_t, rsp[0]) == 4, "x86_64 TSS RSP0 offset");
_Static_assert(sizeof(tss_t) == 104, "x86_64 TSS size");

/*
 * Per-CPU interrupt stacks.  IST slots live here; 16 KiB gives a #DF (or any
 * future IST user) room for a full register frame plus a panic/dump path even
 * when the interrupted kernel stack is gone.
 */
#define TSS_IST_STACK_SIZE 0x4000UL

typedef uint8_t tss_stack_t[TSS_IST_STACK_SIZE];

extern tss_t       tss0;
extern tss_stack_t tss_stack;

/* Initialize TSS */
void tss_init(void);

/* Setting up the kernel stack */
void set_kernel_stack(uint64_t rsp);

#endif // INCLUDE_TSS_H_
