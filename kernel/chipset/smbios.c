/*
 *
 *      smbios.c
 *      System Management BIOS
 *
 *      2025/3/8 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "smbios.h"
#include "hhdm.h"
#include "limine.h"
#include "uinxed.h"

/* Query SMBIOS table */
static const struct Header *find_smbios_type(uint8_t target_type)
{
    uint8_t *table;
    uint32_t length;

    if (!smbios_request.response) return 0;

    if (smbios_request.response->entry_64) {
        struct EntryPoint64 *ep = (struct EntryPoint64 *)smbios_entry();
        table                   = (uint8_t *)phys_to_virt(ep->StructureTableAddress);
        length                  = ep->MaxStructureSize;
    } else {
        struct EntryPoint32 *ep = (struct EntryPoint32 *)smbios_entry();
        table                   = (uint8_t *)phys_to_virt(ep->StructureTableAddress);
        length                  = ep->StructureTableLength;
    }

    uint8_t *end = table + length;
    while (table + sizeof(struct Header) <= end) {
        struct Header *hdr = (struct Header *)table;
        if (hdr->length < sizeof(struct Header)) break;
        if (hdr->type == target_type) return hdr;

        uint8_t *next = table + hdr->length;
        while (next + 1 < end && (next[0] != 0 || next[1] != 0)) next++;
        next += 2;
        table = next;
    }
    return 0;
}

/* Parsing table string */
static const char *smbios_get_string(const struct Header *hdr, int index)
{
    if (!hdr || !index) return "";

    const char *str = (const char *)hdr + hdr->length;
    while (--index > 0 && *str) {
        while (*str) str++;
        str++;
    }

    return (*str) ? str : "";
}

/* Get SMBIOS entry point */
void *smbios_entry(void)
{
    if (smbios_request.response == 0) return 0;
    if (smbios_request.response->entry_64)
        return (void *)smbios_request.response->entry_64;
    else if (smbios_request.response->entry_32)
        return (void *)smbios_request.response->entry_32;
    return 0;
}

/* Get the SMBIOS major version */
int smbios_major_version(void)
{
    if (smbios_request.response->entry_64)
        return ((struct EntryPoint64 *)smbios_entry())->MajorVersion;
    else
        return ((struct EntryPoint32 *)smbios_entry())->MajorVersion;
}

/* Get SMBIOS minor version */
int smbios_minor_version(void)
{
    if (smbios_request.response->entry_64)
        return ((struct EntryPoint64 *)smbios_entry())->MinorVersion;
    else
        return ((struct EntryPoint32 *)smbios_entry())->MinorVersion;
}

/* Type-0 */

/* Get BIOS Vendor string */
const char *smbios_bios_vendor(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 4));
}

/* Get BIOS Version string */
const char *smbios_bios_version(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 5));
}

/* Get BIOS Starting Address Segment */
uint16_t smbios_bios_starting_address_segment(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return 0;
    return *(uint16_t *)((uint8_t *)hdr + 6);
}

/* Get BIOS Release Date string */
const char *smbios_bios_release_date(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 8));
}

/* Get BIOS ROM Size in bytes */
uint32_t smbios_bios_rom_size(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return 0;
    return ((*((uint8_t *)hdr + 9)) + 1) * 64 * 1024;
}

/* Get BIOS Characteristics */
uint64_t smbios_bios_characteristics(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return 0;
    return *(uint64_t *)((uint8_t *)hdr + 10);
}

/* Get BIOS Characteristics Extension Bytes */
uint16_t smbios_bios_characteristics_ext(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return 0;
    if (hdr->length < 0x18) return 0;
    return *(uint16_t *)((uint8_t *)hdr + 24);
}

/* Get BIOS System BIOS Major Release */
uint8_t smbios_bios_major_release(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return 0;
    if (hdr->length < 0x19) return 0;
    return *((uint8_t *)hdr + 26);
}

/* Get BIOS System BIOS Minor Release */
uint8_t smbios_bios_minor_release(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return 0;
    if (hdr->length < 0x1a) return 0;
    return *((uint8_t *)hdr + 27);
}

/* Get BIOS Embedded Controller Firmware Major Release */
uint8_t smbios_bios_ec_major_release(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return 0;
    if (hdr->length < 0x1b) return 0;
    return *((uint8_t *)hdr + 28);
}

/* Get BIOS Embedded Controller Firmware Minor Release */
uint8_t smbios_bios_ec_minor_release(void)
{
    const struct Header *hdr = find_smbios_type(0);
    if (!hdr) return 0;
    if (hdr->length < 0x1c) return 0;
    return *((uint8_t *)hdr + 29);
}

/* Type-1 */

/* Get System Manufacturer string */
const char *smbios_sys_manufacturer(void)
{
    const struct Header *hdr = find_smbios_type(1);
    if (!hdr) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 4));
}

/* Get System Product Name string */
const char *smbios_sys_product_name(void)
{
    const struct Header *hdr = find_smbios_type(1);
    if (!hdr) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 5));
}

/* Get System Version string */
const char *smbios_sys_version(void)
{
    const struct Header *hdr = find_smbios_type(1);
    if (!hdr) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 6));
}

/* Get System Serial Number string */
const char *smbios_sys_serial_number(void)
{
    const struct Header *hdr = find_smbios_type(1);
    if (!hdr) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 7));
}

/* Get System UUID (16 bytes) */
void smbios_sys_uuid(uint8_t uuid[16])
{
    const struct Header *hdr = find_smbios_type(1);
    if (!hdr) {
        for (int i = 0; i < 16; i++) uuid[i] = 0;
        return;
    }
    const uint8_t *ptr = (const uint8_t *)hdr + 8;
    for (int i = 0; i < 16; i++) uuid[i] = ptr[i];
}

/* Get System Wake-up Type */
uint8_t smbios_sys_wakeup_type(void)
{
    const struct Header *hdr = find_smbios_type(1);
    if (!hdr) return 0;
    return *((uint8_t *)hdr + 24);
}

/* Get System SKU Number string */
const char *smbios_sys_sku_number(void)
{
    const struct Header *hdr = find_smbios_type(1);
    if (!hdr) return "";
    if (hdr->length < 0x20) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 25));
}

/* Get System Family string */
const char *smbios_sys_family(void)
{
    const struct Header *hdr = find_smbios_type(1);
    if (!hdr) return "";
    if (hdr->length < 0x21) return "";
    return smbios_get_string(hdr, *((uint8_t *)hdr + 26));
}
