/*
 *
 *      eis.c
 *      Extended instruction set
 *
 *      2025/9/6 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cpuid.h>
#include <eis.h>
#include <stdint.h>

/* Initialize the FPU, including MMX (if any) */
void init_fpu(void)
{
#if CPU_FEATURE_FPU
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0)::"memory");

    cr0 &= ~(1UL << 2); // EM = 0
    cr0 |= (1UL << 1);  // MP = 1
    cr0 &= ~(1UL << 3); // TS = 0

    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
    __asm__ volatile("fninit");
#else
    return;
#endif
}

/* Initialize the SSE, including SSE2 (if any) */
void init_sse(void)
{
#if CPU_FEATURE_SSE
    if (!cpu_support_sse()) return;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4)::"memory");

    cr4 |= (1UL << 9);  // OSFXSR = 1
    cr4 |= (1UL << 10); // OSXMMEXCPT = 1

    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
#else
    return;
#endif
}

/* Initialize the AVX, including AVX2 (if any) */
void init_avx(void)
{
#if CPU_FEATURE_AVX
    if (!cpu_support_avx()) return;

    uint64_t cr4;
    uint64_t xcr0 = 0xE7;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4)::"memory");

    cr4 |= (1UL << 18); // OSXSAVE = 1

    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
    __asm__ volatile("xsetbv" ::"a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)), "c"(0) : "memory");
#else
    return;
#endif
}
