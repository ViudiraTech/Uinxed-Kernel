/*
 *
 *      cpuid.h
 *      CPUID related operation header file
 *
 *      2024/8/21 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CPUID_H_
#define INCLUDE_CPUID_H_

#include <stdint.h>

/* Get CPUID */
void cpuid(uint32_t code, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

/* Get CPU manufacturer name */
char *get_vendor_name(void);

/* Get the CPU model name */
char *get_model_name(void);

/* Get the CPU physical address size */
uint32_t get_cpu_phys_bits(void);

/* Get CPU virtual address size */
uint32_t get_cpu_virt_bits(void);

/* Check CPU supports NX/XD */
int cpu_supports_nx(void);

/* Check CPU supports 64bit */
int cpu_support_64bit(void);

/* Check CPU supports rdtsc */
int cpu_support_rdtsc(void);

/* Check CPU supports rdtscp */
int cpu_support_rdtscp(void);

/* Check CPU supports MMX */
int cpu_support_mmx(void);

/* Check CPU supports SSE */
int cpu_support_sse(void);

/* Check CPU supports SSE2 */
int cpu_support_sse2(void);

/* Check CPU supports SSE3 */
int cpu_support_sse3(void);

/* Check CPU supports SSSE3 */
int cpu_support_ssse3(void);

/* Check CPU supports SSE4.1 */
int cpu_support_sse41(void);

/* Check CPU supports SSE4.2 */
int cpu_support_sse42(void);

/* Check CPU supports SSE4a (AMD-specific) */
int cpu_support_sse4a(void);

/* Check CPU supports XOP (AMD-specific) */
int cpu_support_xop(void);

/* Check CPU supports FMA4 (AMD-specific) */
int cpu_support_fma4(void);

/* Check CPU supports AVX */
int cpu_support_avx(void);

/* Check CPU supports AVX2 */
int cpu_support_avx2(void);

#endif // INCLUDE_CPUID_H_
