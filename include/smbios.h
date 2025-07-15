/*
 *
 *      smbios.h
 *      System Management BIOS Header File
 *
 *      2025/3/8 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SMBIOS_H_
#define INCLUDE_SMBIOS_H_

#include "stdint.h"

/* 32-bit entry point structure */
struct EntryPoint32 {
        uint8_t AnchorString[4];             // Anchor string, value is "_SM_"
        uint8_t Checksum;                    // Checksum
        uint8_t EntryLength;                 // Entry point structure length
        uint8_t MajorVersion;                // SMBIOS major version number
        uint8_t MinorVersion;                // SMBIOS minor version number
        uint16_t MaxStructureSize;           // Maximum structure size
        uint8_t EntryPointRevision;          // Entry point revision number
        uint8_t FormattedArea[5];            // Formatting Area
        uint8_t IntermediateAnchorString[5]; // Middle anchor string, value is
                                             // "_DMI_"
        uint8_t IntermediateChecksum;        // Intermediate Checksum
        uint16_t StructureTableLength;       // Structure table length
        uint32_t StructureTableAddress;      // Structure table address
        uint16_t NumberOfStructures;         // Number of structures
        uint8_t BCDRevision;                 // BCD revision number
};

/* 64-bit entry point structure */
struct EntryPoint64 {
        uint8_t AnchorString[5];        // Anchor string, value is "_SM3_"
        uint8_t Checksum;               // Checksum
        uint8_t EntryLength;            // Entry point structure length
        uint8_t MajorVersion;           // SMBIOS major version number
        uint8_t MinorVersion;           // SMBIOS minor version number
        uint8_t Docrev;                 // Document revision number
        uint8_t EntryPointRevision;     // Entry point revision number
        uint8_t Reserved;               // Reserved Bytes
        uint32_t MaxStructureSize;      // Maximum structure size
        uint64_t StructureTableAddress; // Structure table address
};

/* Structure table header */
struct Header {
        uint8_t type;    // Structure Type
        uint8_t length;  // Structure length (excluding string table)
        uint16_t handle; // Structure Handle
};

/* Get SMBIOS entry point */
void *smbios_entry(void);

/* Get the SMBIOS major version */
int smbios_major_version(void);

/* Get SMBIOS minor version */
int smbios_minor_version(void);

/* Type-0 */

/* Get BIOS Vendor string */
const char *smbios_bios_vendor(void);

/* Get BIOS Version string */
const char *smbios_bios_version(void);

/* Get BIOS Starting Address Segment */
uint16_t smbios_bios_starting_address_segment(void);

/* Get BIOS Release Date string */
const char *smbios_bios_release_date(void);

/* Get BIOS ROM Size in bytes */
uint32_t smbios_bios_rom_size(void);

/* Get BIOS Characteristics */
uint64_t smbios_bios_characteristics(void);

/* Get BIOS Characteristics Extension Bytes */
uint16_t smbios_bios_characteristics_ext(void);

/* Get BIOS System BIOS Major Release */
uint8_t smbios_bios_major_release(void);

/* Get BIOS System BIOS Minor Release */
uint8_t smbios_bios_minor_release(void);

/* Get BIOS Embedded Controller Firmware Major Release */
uint8_t smbios_bios_ec_major_release(void);

/* Get BIOS Embedded Controller Firmware Minor Release */
uint8_t smbios_bios_ec_minor_release(void);

/* Type-1 */

/* Get System Manufacturer string */
const char *smbios_sys_manufacturer(void);

/* Get System Product Name string */
const char *smbios_sys_product_name(void);

/* Get System Version string */
const char *smbios_sys_version(void);

/* Get System Serial Number string */
const char *smbios_sys_serial_number(void);

/* Get System UUID (16 bytes) */
void smbios_sys_uuid(uint8_t uuid[16]);

/* Get System Wake-up Type */
uint8_t smbios_sys_wakeup_type(void);

/* Get System SKU Number string */
const char *smbios_sys_sku_number(void);

/* Get System Family string */
const char *smbios_sys_family(void);

#endif // INCLUDE_SMBIOS_H_
