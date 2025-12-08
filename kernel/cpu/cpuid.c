/*
 *
 *      cpuid.c
 *      CPUID related operations
 *
 *      2024/8/21 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cpuid.h>

/* Get CPUID */
void cpuid(uint32_t code, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(code) : "memory");
}

/* Get CPU manufacturer name */
char *get_vendor_name(void)
{
    int         cpuid_level;
    static char x86_vendor_id[16] = {0};
    cpuid(0x00000000, (uint32_t *)&cpuid_level, (uint32_t *)&x86_vendor_id[0], (uint32_t *)&x86_vendor_id[8], (uint32_t *)&x86_vendor_id[4]);
    return x86_vendor_id;
}

/* Get the CPU model name */
char *get_model_name(void)
{
    static char model_name[64];
    uint32_t   *p = (uint32_t *)model_name;

    cpuid(0x80000002, &p[0], &p[1], &p[2], &p[3]);
    cpuid(0x80000003, &p[4], &p[5], &p[6], &p[7]);
    cpuid(0x80000004, &p[8], &p[9], &p[10], &p[11]);
    model_name[48] = 0;
    return model_name;
}

/* Get the CPU physical address size */
uint32_t get_cpu_phys_bits(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
    return (eax & 0xff);
}

/* Get CPU virtual address size */
uint32_t get_cpu_virt_bits(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
    return ((eax >> 8) & 0xff);
}

/* Check CPU supports NX/XD */
int cpu_supports_nx(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return ((edx & (1 << 20)) != 0);
}

/* Check CPU supports 64bit */
int cpu_support_64bit(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return ((edx & (1 << 29)) != 0);
}

/* Check CPU supports rdtsc */
int cpu_support_rdtsc(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((edx & (1 << 4)) != 0);
}

/* Check CPU supports rdtscp */
int cpu_support_rdtscp(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return ((edx & (1 << 27)) != 0);
}

/* Check CPU supports MMX */
int cpu_support_mmx(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((edx & (1 << 23)) != 0);
}

/* Check CPU supports SSE */
int cpu_support_sse(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((edx & (1 << 25)) != 0);
}

/* Check CPU supports SSE2 */
int cpu_support_sse2(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((edx & (1 << 26)) != 0);
}

/* Check CPU supports SSE3 */
int cpu_support_sse3(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 0)) != 0);
}

/* Check CPU supports SSSE3 */
int cpu_support_ssse3(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 9)) != 0);
}

/* Check CPU supports SSE4.1 */
int cpu_support_sse41(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 19)) != 0);
}

/* Check CPU supports SSE4.2 */
int cpu_support_sse42(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 20)) != 0);
}

/* Check CPU supports SSE4a (AMD specific) */
int cpu_support_sse4a(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx); // SSE4a info in extended leaf
    return ((ecx & (1 << 6)) != 0);
}

/* Check CPU supports XOP (AMD specific) */
int cpu_support_xop(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 11)) != 0);
}

/* Check CPU supports FMA4 (AMD specific) */
int cpu_support_fma4(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 16)) != 0);
}

/* Check CPU supports AVX */
int cpu_support_avx(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 28)) != 0);
}

/* Check CPU supports AVX2 */
int cpu_support_avx2(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000007, &eax, &ebx, &ecx, &edx);
    return ((ebx & (1 << 5)) != 0);
}
