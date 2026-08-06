/*
 *
 *      crc32c.c
 *      CRC32C (Castagnoli) checksum
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpuid.h>
#include <libs/data/crc32c.h>

/* SSE4.2 `crc32` is a scalar GPR instruction (CRC32C / Castagnoli, same
 * polynomial as this software loop).  It touches no XMM state, so it can
 * be used without a kernel_fpu_begin()/end() section. */
__attribute__((target("crc32"))) static inline uint32_t crc32c_hw_byte(uint32_t crc, uint8_t value)
{
    return __builtin_ia32_crc32qi(crc, value);
}

__attribute__((target("crc32"))) static inline uint32_t crc32c_hw_dword(uint32_t crc, uint32_t value)
{
    return __builtin_ia32_crc32si(crc, value);
}

__attribute__((target("crc32"))) static inline uint32_t crc32c_hw_qword(uint32_t crc, uint64_t value)
{
    return __builtin_ia32_crc32di(crc, value);
}

/* Bit-by-bit software fallback for CPUs without SSE4.2 */
static uint32_t crc32c_software(uint32_t crc, const uint8_t *bytes, size_t size)
{
    while (size--) {
        crc ^= *bytes++;
        for (uint32_t bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0x82F63B78U & (uint32_t) - (int32_t)(crc & 1));
    }
    return crc;
}

uint32_t crc32c_update(uint32_t crc, const void *data, size_t size)
{
    static uint8_t hw_checked;
    static uint8_t hw_ok;

    if (!hw_checked) {
        hw_ok      = cpu_support_sse42() != 0;
        hw_checked = 1;
    }
    if (!hw_ok) return crc32c_software(crc, data, size);

    const uint8_t *p = data;
    while (size && ((uintptr_t)p & 7)) {
        crc = crc32c_hw_byte(crc, *p++);
        size--;
    }
    while (size >= 8) {
        uint64_t chunk;
        __builtin_memcpy(&chunk, p, 8);
        crc = crc32c_hw_qword(crc, chunk);
        p += 8;
        size -= 8;
    }
    while (size >= 4) {
        uint32_t chunk;
        __builtin_memcpy(&chunk, p, 4);
        crc = crc32c_hw_dword(crc, chunk);
        p += 4;
        size -= 4;
    }
    while (size--) crc = crc32c_hw_byte(crc, *p++);
    return crc;
}
