/*
 *
 *      cpuid.h
 *      CPUID related operation header file
 *
 *      2024/8/21 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CPUID_H_
#define INCLUDE_CPUID_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Safe CPUID wrapper (avoids register clobber issues with pointer params) */
void cpuid_safe(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);

/* Build a space-separated CPU feature flag string from real CPUID bits */
void cpu_build_flags(char *buf, size_t size);

/* Get CPUID */
void cpuid(uint32_t code, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

/* Get CPUID with a subleaf */
void cpuid_count(uint32_t code, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

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

/* Enable NX page-table entries on the current logical CPU */
int cpu_enable_nx(void);

/* Check whether NX is enabled on the current logical CPU */
int cpu_nx_enabled(void);

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

/* Check CPU supports XSAVE/XRSTOR */
int cpu_support_xsave(void);

/* Check CPU supports OSXSAVE */
int cpu_support_osxsave(void);

/* Check whether XCR0 supports the requested state bits */
int cpu_xcr0_supports(uint64_t mask);

/* Check CPU supports AVX2 */
int cpu_support_avx2(void);

#endif // INCLUDE_CPUID_H_
