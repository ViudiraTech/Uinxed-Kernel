/*
 *
 *      simd.c
 *      SIMD related
 *
 *      2025/9/6 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "cpuid.h"
#include "stdint.h"

/* Initialize the FPU, including MMX (if any) */
void init_fpu(void)
{
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0)::"memory");

    cr0 &= ~(1 << 2); // EM = 0
    cr0 |= (1 << 1);  // MP = 1
    cr0 &= ~(1 << 3); // TS = 0

    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
    __asm__ volatile("fninit");
}

/* Initialize the SSE, including SSE2 (if any) */
void init_sse(void)
{
    if (!cpu_support_sse()) return;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4)::"memory");

    cr4 |= (1 << 9);  // OSFXSR = 1
    cr4 |= (1 << 10); // OSXMMEXCPT = 1

    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
}

/* Initialize the AVX, including AVX2 (if any) */
void init_avx(void)
{
    if (!cpu_support_avx()) return;

    uint64_t cr4;
    uint64_t xcr0 = 0x7;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4)::"memory");

    cr4 |= (1 << 18); // OSXSAVE = 1

    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
    __asm__ volatile("xsetbv" ::"a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)), "c"(0) : "memory");
}
