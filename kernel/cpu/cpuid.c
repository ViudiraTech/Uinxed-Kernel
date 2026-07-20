/*
 *
 *      cpuid.c
 *      CPUID related operations
 *
 *      2024/8/21 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cpuid.h>
#include <string.h>

/* Get CPUID */
void cpuid(uint32_t code, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{ __asm__ volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(code) : "memory"); }

/* Get CPUID with a subleaf */
void cpuid_count(uint32_t code, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{ __asm__ volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(code), "c"(subleaf) : "memory"); }

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

/* Check CPU supports XSAVE/XRSTOR */
int cpu_support_xsave(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 26)) != 0);
}

/* Check CPU supports OSXSAVE */
int cpu_support_osxsave(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    return ((ecx & (1 << 27)) != 0);
}

/* Check whether XCR0 supports the requested state bits */
int cpu_xcr0_supports(uint64_t mask)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid_count(0x0000000d, 0, &eax, &ebx, &ecx, &edx);
    uint64_t supported = ((uint64_t)edx << 32) | eax;
    return (supported & mask) == mask;
}

/* Check CPU supports AVX2 */
int cpu_support_avx2(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid_count(0x00000007, 0, &eax, &ebx, &ecx, &edx);
    return ((ebx & (1 << 5)) != 0);
}

/* Safe CPUID wrapper – uses local temporaries to avoid register clobber issues */
void cpuid_safe(uint32_t leaf, uint32_t sub,
                uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t _a, _b, _c, _d;
    __asm__ volatile("cpuid" : "=a"(_a), "=b"(_b), "=c"(_c), "=d"(_d)
                     : "a"(leaf), "c"(sub) : "memory");
    if (a) *a = _a;
    if (b) *b = _b;
    if (c) *c = _c;
    if (d) *d = _d;
}

/* Build a space-separated CPU feature flag string from real CPUID bits */
void cpu_build_flags(char *buf, size_t size)
{
    size_t pos = 0;
    uint32_t a, b, c, d;
    size_t i;

    /* Leaf 1, EDX (d) */
    cpuid_safe(0x00000001, 0, &a, &b, &c, &d);
    static const struct { uint32_t bit; const char *name; } ed1[] = {
        { 0, "fpu"},{ 1, "vme"},{ 2, "de"},{ 3, "pse"},{ 4, "tsc"},
        { 5, "msr"},{ 6, "pae"},{ 7, "mce"},{ 8, "cx8"},{ 9, "apic"},
        {11, "sep"},{12, "mtrr"},{13, "pge"},{14, "mca"},{15, "cmov"},
        {16, "pat"},{17, "pse36"},{19, "clflush"},{23, "mmx"},{24, "fxsr"},
        {25, "sse"},{26, "sse2"},{27, "ss"},{28, "ht"},{29, "tm"},
        {30, "ia64"},{31, "pbe"},
    };
    for (i = 0; i < sizeof(ed1)/sizeof(ed1[0]) && pos + 12 < size; i++)
        if (d & (1U << ed1[i].bit)) { buf[pos++] = ' '; size_t l = strlen(ed1[i].name); memcpy(buf+pos, ed1[i].name, l); pos += l; }

    /* Leaf 1, ECX (c) */
    static const struct { uint32_t bit; const char *name; } ec1[] = {
        { 0, "pni"},{ 1, "pclmulqdq"},{ 3, "monitor"},{ 5, "vmx"},
        { 6, "smx"},{ 7, "est"},{ 9, "ssse3"},{12, "fma"},{13, "cx16"},
        {17, "pcid"},{19, "sse4_1"},{20, "sse4_2"},{21, "x2apic"},
        {22, "movbe"},{23, "popcnt"},{24, "tsc_deadline"},{25, "aes"},
        {26, "xsave"},{27, "osxsave"},{28, "avx"},{29, "f16c"},
        {30, "rdrand"},{31, "hypervisor"},
    };
    for (i = 0; i < sizeof(ec1)/sizeof(ec1[0]) && pos + 12 < size; i++)
        if (c & (1U << ec1[i].bit)) { buf[pos++] = ' '; size_t l = strlen(ec1[i].name); memcpy(buf+pos, ec1[i].name, l); pos += l; }

    /* Leaf 7 (sub 0), EBX (b) */
    cpuid_safe(0x00000007, 0, &a, &b, &c, &d);
    static const struct { uint32_t bit; const char *name; } eb7[] = {
        { 0, "fsgsbase"},{ 1, "tsc_adjust"},{ 2, "sgx"},{ 3, "bmi1"},
        { 4, "hle"},{ 5, "avx2"},{ 7, "smep"},{ 8, "bmi2"},{ 9, "erms"},
        {10, "invpcid"},{11, "rtm"},{14, "mpx"},{16, "avx512f"},
        {17, "avx512dq"},{18, "rdseed"},{19, "adx"},{20, "smap"},
        {21, "avx512ifma"},{23, "clflushopt"},{24, "clwb"},{26, "avx512pf"},
        {27, "avx512er"},{28, "avx512cd"},{29, "sha_ni"},{30, "avx512bw"},
        {31, "avx512vl"},
    };
    for (i = 0; i < sizeof(eb7)/sizeof(eb7[0]) && pos + 12 < size; i++)
        if (b & (1U << eb7[i].bit)) { buf[pos++] = ' '; size_t l = strlen(eb7[i].name); memcpy(buf+pos, eb7[i].name, l); pos += l; }

    /* Leaf 7 (sub 0), ECX (c) */
    static const struct { uint32_t bit; const char *name; } ec7[] = {
        { 0, "prefetchwt1"},{ 1, "avx512vbmi"},{ 2, "umip"},{ 3, "pku"},
        { 4, "ospke"},{ 6, "avx512vbmi2"},{ 8, "gfni"},{ 9, "vaes"},
        {10, "vpclmulqdq"},{11, "avx512vnni"},{12, "avx512bitalg"},
        {14, "avx512vpopcntdq"},{22, "rdpid"},
    };
    for (i = 0; i < sizeof(ec7)/sizeof(ec7[0]) && pos + 12 < size; i++)
        if (c & (1U << ec7[i].bit)) { buf[pos++] = ' '; size_t l = strlen(ec7[i].name); memcpy(buf+pos, ec7[i].name, l); pos += l; }

    /* Leaf 0x80000001, EDX (d) */
    cpuid_safe(0x80000001, 0, &a, &b, &c, &d);
    static const struct { uint32_t bit; const char *name; } ed8[] = {
        {11, "syscall"},{20, "nx"},{26, "pdpe1gb"},{27, "rdtscp"},{29, "lm"},
    };
    for (i = 0; i < sizeof(ed8)/sizeof(ed8[0]) && pos + 12 < size; i++)
        if (d & (1U << ed8[i].bit)) { buf[pos++] = ' '; size_t l = strlen(ed8[i].name); memcpy(buf+pos, ed8[i].name, l); pos += l; }

    /* Leaf 0x80000001, ECX (c) */
    static const struct { uint32_t bit; const char *name; } ec8[] = {
        { 0, "lahf_lm"},{ 5, "abm"},{ 6, "sse4a"},{ 8, "3dnowprefetch"},
        {11, "xop"},{16, "fma4"},{22, "topoext"},
    };
    for (i = 0; i < sizeof(ec8)/sizeof(ec8[0]) && pos + 12 < size; i++)
        if (c & (1U << ec8[i].bit)) { buf[pos++] = ' '; size_t l = strlen(ec8[i].name); memcpy(buf+pos, ec8[i].name, l); pos += l; }

    if (pos) buf[pos] = '\0'; else buf[0] = '\0';
}
