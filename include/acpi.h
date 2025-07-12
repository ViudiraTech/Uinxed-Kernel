/*
 *
 *      acpi.h
 *      Advanced Configuration and Power Management Interface Header File
 *
 *      2025/2/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_ACPI_H_
#define INCLUDE_ACPI_H_

#include "stddef.h"
#include "stdint.h"

#define load_table(tblname, func)                          \
    do {                                                   \
        void *tblname##_ptr = find_table(#tblname);        \
        if ((tblname##_ptr) != 0) (func)((tblname##_ptr)); \
    } while (0)

struct ACPISDTHeader {
        char Signature[4];
        uint32_t Length;
        uint8_t Revision;
        uint8_t Checksum;
        char OEMID[6];
        char OEMTableID[8];
        uint32_t OEMRevision;
        uint32_t CreatorID;
        uint32_t CreatorRevision;
};

typedef struct {
        char signature[8];         // Sign
        uint8_t checksum;          // Checksum
        char oem_id[6];            // OEM ID
        uint8_t revision;          // Version
        uint32_t rsdt_address;     // V1: RSDT address (32-bit)
        uint32_t length;           // Structure length
        uint64_t xsdt_address;     // V2: XSDT address (64-bit)
        uint8_t extended_checksum; // Extended Checksum
        uint8_t reserved[3];       // Reserved Fields
} __attribute__((packed)) RSDP;

typedef struct {
        struct ACPISDTHeader h;
        uint32_t PointerToOtherSDT;
} __attribute__((packed)) RSDT;

typedef struct {
        struct ACPISDTHeader h;
        uint64_t PointerToOtherSDT;
} __attribute__((packed)) XSDT;

struct generic_address {
        uint8_t address_space;
        uint8_t bit_width;
        uint8_t bit_offset;
        uint8_t access_size;
        uint64_t address;
} __attribute__((packed));

struct hpet {
        struct ACPISDTHeader h;
        uint32_t event_block_id;
        struct generic_address base_address;
        uint16_t clock_tick_unit;
        uint8_t page_oem_flags;
} __attribute__((packed));

typedef struct {
        uint64_t configurationAndCapability;
        uint64_t comparatorValue;
        uint64_t fsbInterruptRoute;
        uint64_t unused;
} __attribute__((packed)) HpetTimer;

typedef struct {
        uint64_t generalCapabilities;
        uint64_t reserved0;
        uint64_t generalConfiguration;
        uint64_t reserved1;
        uint64_t generalIntrruptStatus;
        uint8_t reserved3[0xc8];
        uint64_t mainCounterValue;
        uint64_t reserved4;
        HpetTimer timers[];
} __attribute__((packed)) volatile HpetInfo;

typedef struct dsdt_table {
        uint8_t signature[4];
        uint32_t length;
        uint8_t revision;
        uint8_t checksum;
        uint8_t oem_id[6];
        uint8_t oem_tableid[8];
        uint32_t oem_revision;
        uint32_t creator_id;
        uint8_t definition_block;
} __attribute__((packed)) dsdt_table_t;

typedef struct facp_table {
        struct ACPISDTHeader h;
        uint32_t firmware_ctrl;
        uint32_t dsdt;
        uint8_t reserved;
        uint8_t preferred_pm_profile;
        uint16_t sci_int;
        uint32_t smi_cmd;
        uint8_t acpi_enable;
        uint8_t acpi_disable;
        uint8_t s4bios_req;
        uint8_t pstate_cnt;
        uint32_t pm1a_evt_blk;
        uint32_t pm1b_evt_blk;
        uint32_t pm1a_cnt_blk;
        uint32_t pm1b_cnt_blk;
        uint32_t pm2_cnt_blk;
        uint32_t pm_tmr_blk;
        uint32_t gpe0_blk;
        uint32_t gpe1_blk;
        uint8_t pm1_evt_len;
        uint8_t pm1_cnt_len;
        uint8_t pm2_cnt_len;
        uint8_t pm_tmr_len;
        uint8_t gpe0_blk_len;
        uint8_t gpe1_blk_len;
        uint8_t gpe1_base;
        uint8_t cst_cnt;
        uint16_t p_lvl2_lat;
        uint16_t p_lvl3_lat;
        uint16_t flush_size;
        uint16_t flush_stride;
        uint8_t duty_offset;
        uint8_t duty_width;
        uint8_t day_alrm;
        uint8_t mon_alrm;
        uint8_t century;
        uint16_t iapc_boot_arch;
        uint8_t reserved2;
        uint32_t flags;
        struct generic_address reset_reg;
        uint8_t reset_value;
        uint8_t reserved3[3];
        uint64_t x_firmware_ctrl;
        uint64_t x_dsdt;
        struct generic_address x_pm1a_evt_blk;
        struct generic_address x_pm1b_evt_blk;
        struct generic_address x_pm1a_cnt_blk;
        struct generic_address x_pm1b_cnt_blk;
        struct generic_address x_pm2_cnt_blk;
        struct generic_address x_pm_tmr_blk;
        struct generic_address x_gpe0_blk;
        struct generic_address x_gpe1_blk;
} __attribute__((packed)) acpi_facp_t;

typedef struct hpet Hpet;

typedef struct mcfg_entry {
        uint64_t base_addr;
        uint16_t segment;
        uint8_t start_bus;
        uint8_t end_bus;
        uint32_t reserved;
} __attribute__((packed)) mcfg_entry;

typedef struct MCFG {
        struct ACPISDTHeader h;
        char reserved[8];
        mcfg_entry entries[]; // Length is dynamic
} __attribute__((packed)) MCFG;

typedef struct MCFGInfo {
        MCFG *mcfg;
        size_t count;
        int enabled;
} MCFGInfo;

/* Find the corresponding ACPI table in XSDT */
void *find_table(const char *name);

/* Initialize ACPI */
void acpi_init(void);

/* Returns the nanosecond value of the current time */
uint64_t nano_time(void);

/* Initialize high-precision event timer */
void hpet_init(Hpet *hpet);

/* Initialize facp */
void facp_init(acpi_facp_t *facp0);

/* Get MCFG information */
MCFGInfo *get_mcfg(void);

/* Cycle the power */
void power_reset(void);

/* Power off */
void power_off(void);

/* Obtain ACPI major version */
uint8_t get_acpi_version_major(void);

/* Obtain ACPI minor version */
uint16_t get_acpi_version_minor(void);

#endif // INCLUDE_ACPI_H_
