/*
 *
 *      acpi.h
 *      Advanced configuration and power management interface header file
 *
 *      2025/2/16 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ACPI_H_
#define INCLUDE_ACPI_H_

#include <stddef.h>
#include <stdint.h>

#define load_table(tblname, func)                          \
    do {                                                   \
        void *tblname##_ptr = find_table(#tblname);        \
        if ((tblname##_ptr) != 0) (func)((tblname##_ptr)); \
    } while (0)

typedef struct {
        char     signature[4];
        uint32_t length;
        uint8_t  revision;
        uint8_t  checksum;
        char     oem_id[6];
        char     oem_table_id[8];
        uint32_t oem_revision;
        uint32_t creator_id;
        uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct {
        char     signature[8];      // Sign
        uint8_t  checksum;          // Checksum
        char     oem_id[6];         // OEM ID
        uint8_t  revision;          // Version
        uint32_t rsdt_address;      // V1: RSDT address (32-bit)
        uint32_t length;            // Structure length
        uint64_t xsdt_address;      // V2: XSDT address (64-bit)
        uint8_t  extended_checksum; // Extended Checksum
        uint8_t  reserved[3];       // Reserved Fields
} __attribute__((packed)) rsdp_t;

typedef struct {
        acpi_sdt_header_t header;
        uint32_t          PointerToOtherSDT;
} __attribute__((packed)) rsdt_t;

typedef struct {
        acpi_sdt_header_t header;
        uint64_t          PointerToOtherSDT;
} __attribute__((packed)) xsdt_t;

typedef struct {
        uint8_t  address_space;
        uint8_t  bit_width;
        uint8_t  bit_offset;
        uint8_t  access_size;
        uint64_t address;
} __attribute__((packed)) generic_address_t;

typedef struct {
        acpi_sdt_header_t header;
        uint32_t          event_block_id;
        generic_address_t base_address;
        uint16_t          clock_tick_unit;
        uint8_t           page_oem_flags;
} __attribute__((packed)) hpet_t;

typedef struct {
        uint64_t configuration_and_capability;
        uint64_t comparator_value;
        uint64_t fsb_interrupt_route;
        uint64_t unused;
} __attribute__((packed)) hpet_timer_t;

typedef struct {
        uint64_t     general_capabilities;
        uint64_t     reserved_0;
        uint64_t     general_configuration;
        uint64_t     reserved_1;
        uint64_t     general_intrrupt_status;
        uint8_t      reserved_3[0xc8];
        uint64_t     main_counter_value;
        uint64_t     reserved_4;
        hpet_timer_t timers[];
} __attribute__((packed)) volatile hpet_info_t;

typedef struct {
        uint8_t  signature[4];
        uint32_t length;
        uint8_t  revision;
        uint8_t  checksum;
        uint8_t  oem_id[6];
        uint8_t  oem_tableid[8];
        uint32_t oem_revision;
        uint32_t creator_id;
        uint8_t  definition_block;
} __attribute__((packed)) dsdt_table_t;

typedef struct {
        acpi_sdt_header_t header;
        uint32_t          firmware_ctrl;
        uint32_t          dsdt;
        uint8_t           reserved;
        uint8_t           preferred_pm_profile;
        uint16_t          sci_int;
        uint32_t          smi_cmd;
        uint8_t           acpi_enable;
        uint8_t           acpi_disable;
        uint8_t           s4bios_req;
        uint8_t           pstate_cnt;
        uint32_t          pm1a_evt_blk;
        uint32_t          pm1b_evt_blk;
        uint32_t          pm1a_cnt_blk;
        uint32_t          pm1b_cnt_blk;
        uint32_t          pm2_cnt_blk;
        uint32_t          pm_tmr_blk;
        uint32_t          gpe0_blk;
        uint32_t          gpe1_blk;
        uint8_t           pm1_evt_len;
        uint8_t           pm1_cnt_len;
        uint8_t           pm2_cnt_len;
        uint8_t           pm_tmr_len;
        uint8_t           gpe0_blk_len;
        uint8_t           gpe1_blk_len;
        uint8_t           gpe1_base;
        uint8_t           cst_cnt;
        uint16_t          p_lvl2_lat;
        uint16_t          p_lvl3_lat;
        uint16_t          flush_size;
        uint16_t          flush_stride;
        uint8_t           duty_offset;
        uint8_t           duty_width;
        uint8_t           day_alrm;
        uint8_t           mon_alrm;
        uint8_t           century;
        uint16_t          iapc_boot_arch;
        uint8_t           reserved2;
        uint32_t          flags;
        generic_address_t reset_reg;
        uint8_t           reset_value;
        uint8_t           reserved3[3];
        uint64_t          x_firmware_ctrl;
        uint64_t          x_dsdt;
        generic_address_t x_pm1a_evt_blk;
        generic_address_t x_pm1b_evt_blk;
        generic_address_t x_pm1a_cnt_blk;
        generic_address_t x_pm1b_cnt_blk;
        generic_address_t x_pm2_cnt_blk;
        generic_address_t x_pm_tmr_blk;
        generic_address_t x_gpe0_blk;
        generic_address_t x_gpe1_blk;
} __attribute__((packed)) acpi_facp_t;

typedef struct {
        uint64_t base_addr;
        uint16_t segment;
        uint8_t  start_bus;
        uint8_t  end_bus;
        uint32_t reserved;
} __attribute__((packed)) mcfg_entry_t;

typedef struct {
        acpi_sdt_header_t header;
        char              reserved[8];
        mcfg_entry_t      entries[]; // Length is dynamic
} __attribute__((packed)) mcfg_info_t;

typedef struct {
        mcfg_info_t *mcfg;
        size_t       count;
        int          enabled;
} mcfg_t;

/* Find the corresponding ACPI table in XSDT */
void *find_table(const char *name);

/* Initialize ACPI */
void acpi_init(void);

/* Returns the nanosecond value of the current time */
uint64_t nano_time(void);

/* Get the HPET structure */
hpet_info_t *get_acpi_hpet(void);

/* Initialize high-precision event timer */
void hpet_init(hpet_t *hpet);

/* Initialize facp */
void facp_init(acpi_facp_t *facp0);

/* Get the FACP structure */
acpi_facp_t *get_acpi_facp(void);

/* Cycle the power */
void power_reset(void);

/* Power off */
void power_off(void);

/* Obtain ACPI major version */
uint8_t get_acpi_version_major(void);

/* Obtain ACPI minor version */
uint16_t get_acpi_version_minor(void);

/* MCFG initialization */
void mcfg_init(mcfg_info_t *mcfg);

/* Get the MCFG structure */
mcfg_info_t *get_acpi_mcfg(void);

#endif // INCLUDE_ACPI_H_
