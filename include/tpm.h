/*
 *
 *      tpm.h
 *      Trusted platform module header files
 *
 *      2026/7/21 By MicroFish
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TPM_H_
#define INCLUDE_TPM_H_

#include <acpi.h>
#include <stdint.h>

#define TPM_LEGACY_BASE_PHYS 0xFED40000

typedef struct {
        acpi_sdt_header_t header;
        uint16_t          platform_class;
        uint16_t          reserved;
        uint64_t          control_area_address;
        uint32_t          start_method;
} __attribute__((packed)) tpm2_table_t;

typedef struct {
        acpi_sdt_header_t header;
        uint16_t          platform_class;
        uint32_t          log_area_minimum_length;
        uint64_t          log_area_start_address;
} __attribute__((packed)) tcpa_table_t;

/* ACPI-based TPM initialization */
void acpi_tpm_init(void);

#endif // INCLUDE_TPM_H_
