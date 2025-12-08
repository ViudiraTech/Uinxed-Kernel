/*
 *
 *      smbios.h
 *      System management BIOS header file
 *
 *      2025/3/8 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SMBIOS_H_
#define INCLUDE_SMBIOS_H_

#include <stdint.h>

/* 32-bit entry point structure */
typedef struct {
        uint8_t  anchor_string[4];              // Anchor string, value is "_SM_"
        uint8_t  checksum;                      // Checksum
        uint8_t  entry_length;                  // Entry point structure length
        uint8_t  major_version;                 // SMBIOS major version number
        uint8_t  minor_version;                 // SMBIOS minor version number
        uint16_t max_structure_size;            // Maximum structure size
        uint8_t  entry_point_revision;          // Entry point revision number
        uint8_t  formatted_area[5];             // Formatting Area
        uint8_t  intermediate_anchor_string[5]; // Middle anchor string, value is "_DMI_"
        uint8_t  intermediate_checksum;         // Intermediate Checksum
        uint16_t structure_table_length;        // Structure table length
        uint32_t structure_table_address;       // Structure table address
        uint16_t number_of_structures;          // Number of structures
        uint8_t  bcd_revision;                  // BCD revision number
} entry_point_32_t;

/* 64-bit entry point structure */
typedef struct {
        uint8_t  anchor_string[5];        // Anchor string, value is "_SM3_"
        uint8_t  checksum;                // Checksum
        uint8_t  entry_length;            // Entry point structure length
        uint8_t  major_version;           // SMBIOS major version number
        uint8_t  minor_version;           // SMBIOS minor version number
        uint8_t  docrev;                  // Document revision number
        uint8_t  entry_point_revision;    // Entry point revision number
        uint8_t  reserved;                // Reserved Bytes
        uint32_t max_structure_size;      // Maximum structure size
        uint64_t structure_table_address; // Structure table address
} entry_point_64_t;

/* Structure table header */
typedef struct {
        uint8_t  type;   // Structure Type
        uint8_t  length; // Structure length (excluding string table)
        uint16_t handle; // Structure Handle
} header_t;

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
