/*
 *
 *      numa.c
 *      Early ACPI NUMA discovery through the Limine direct map
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <drivers/firmware/acpi.h>
#include <kernel/uinxed.h>
#include <libs/std/string.h>
#include <mem/hhdm.h>
#include <mem/numa.h>

/* Only inspect firmware bytes covered by an existing Limine memory range. */
static const void *firmware_map(uint64_t address, size_t size)
{
    if (!address || !size || size > UINT64_MAX - address || !memmap_request.response) return NULL;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        const struct limine_memmap_entry *range = memmap_request.response->entries[i];
        if (range->length > UINT64_MAX - range->base) continue;
        if (address >= range->base && address + size <= range->base + range->length && range->type != LIMINE_MEMMAP_BAD_MEMORY && range->type != LIMINE_MEMMAP_FRAMEBUFFER)
            return phys_to_virt(address);
    }
    return NULL;
}

/* Header-first mapping bounds the checksum scan and all record reads. */
static const acpi_sdt_header_t *firmware_table(uint64_t address)
{
    const acpi_sdt_header_t *header = firmware_map(address, sizeof(*header));
    if (!header || header->length < sizeof(*header) || header->length > 16 * 1024 * 1024) return NULL;
    header = firmware_map(address, header->length);
    if (!header) return NULL;
    const uint8_t *bytes = (const uint8_t *)header;
    uint8_t        sum   = 0;
    for (uint32_t i = 0; i < header->length; i++) sum += bytes[i];
    return sum ? NULL : header;
}

void numa_init(void)
{
    numa_reset();
    if (!CONFIG_NUMA || !rsdp_request.response || !rsdp_request.response->address) return;
    const rsdp_t *rsdp = rsdp_request.response->address;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) return;
    uint8_t        sum   = 0;
    const uint8_t *bytes = (const uint8_t *)rsdp;
    for (size_t i = 0; i < 20; i++) sum += bytes[i];
    if (sum) return;
    if (rsdp->revision >= 2) {
        if (rsdp->length != sizeof(*rsdp)) return;
        for (size_t i = 20; i < sizeof(*rsdp); i++) sum += bytes[i];
        if (sum) return;
    }
    bool                     xsdt = rsdp->revision >= 2 && rsdp->xsdt_address;
    const acpi_sdt_header_t *root = firmware_table(xsdt ? rsdp->xsdt_address : rsdp->rsdt_address);
    if (!root || memcmp(root->signature, xsdt ? "XSDT" : "RSDT", 4) != 0) return;
    size_t width = xsdt ? 8 : 4;
    size_t size  = root->length - sizeof(*root);
    if (size % width) return;
    const acpi_sdt_header_t *srat = NULL, *slit = NULL;
    bytes = (const uint8_t *)(root + 1);
    for (size_t at = 0; at < size; at += width) {
        uint64_t address = 0;
        memcpy(&address, bytes + at, width);
        const acpi_sdt_header_t *table = firmware_table(address);
        if (!table) continue;
        if (!memcmp(table->signature, "SRAT", 4)) srat = table;
        if (!memcmp(table->signature, "SLIT", 4)) slit = table;
    }
    (void)numa_parse_tables(srat, srat ? srat->length : 0, slit, slit ? slit->length : 0);
    if (smp_request.response) numa_bind_cpu(0, smp_request.response->bsp_lapic_id);
}
