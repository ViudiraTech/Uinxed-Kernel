/*
 *
 *      facp.c
 *      Fixed ACPI description table
 *
 *      2025/2/16 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <acpi.h>
#include <common.h>
#include <hhdm.h>
#include <printk.h>
#include <stdint.h>
#include <string.h>
#include <timer.h>

uint16_t     SLP_TYPa;
uint16_t     SLP_TYPb;
uint16_t     SLP_EN;
uint16_t     SCI_EN;
acpi_facp_t *facp;

/* Initialize facp */
void facp_init(acpi_facp_t *facp0)
{
    uint8_t *S5_addr;
    uint32_t dsdtlen;
    facp = facp0;

    pointer_cast_t dsdt;
    dsdt.val                 = (uintptr_t)facp->dsdt;
    dsdt_table_t *dsdt_table = (dsdt_table_t *)dsdt.ptr;

    if (!dsdt_table) {
        plogk("facp: DSDT table not found.\n");
        return;
    }
    dsdt_table = phys_to_virt((uint64_t)dsdt_table);
    plogk("facp: DSDT found at %p\n", dsdt_table);

    if (!memcmp(dsdt_table->signature, "DSDT", 4)) {
        S5_addr = &(dsdt_table->definition_block);
        dsdtlen = dsdt_table->length - 36;
        while (dsdtlen--) {
            if (!memcmp(S5_addr, "_S5_", 4)) break;
            S5_addr++;
        }
        SLP_EN = 1 << 13;
        SCI_EN = 1;

        if (dsdtlen && S5_addr > &dsdt_table->definition_block + 2) {
            if (*(S5_addr - 1) == 0x08 || (*(S5_addr - 2) == 0x08 && *(S5_addr - 1) == '\\')) {
                S5_addr += 5;
                S5_addr += ((*S5_addr & 0xc0) >> 6) + 2;
                if (*S5_addr == 0x0a) S5_addr++;
                SLP_TYPa = *(S5_addr) << 10;
                S5_addr++;
                if (*S5_addr == 0x0a) S5_addr++;
                SLP_TYPb = *(S5_addr) << 10;
                S5_addr++;
                plogk("facp: SLP_TYPa = 0x%04hx, SLP_TYPb = 0x%04hx\n", SLP_TYPa, SLP_TYPb);
            }
        } else if (dsdtlen) {
            plogk("facp: Invalid _S5_ prefix.\n");
        } else {
            plogk("facp: _S5_ not found in DSDT.\n");
        }
    } else {
        plogk("facp: Invalid DSDT signature.\n");
    }
    if (inw(facp->pm1a_cnt_blk) & SCI_EN) {
        plogk("facp: SCI already enabled.\n");
        return;
    }
    if (facp->smi_cmd && facp->acpi_enable) {
        plogk("facp: Enabling ACPI via SMI command.\n");
        outb(facp->smi_cmd, facp->acpi_enable);

        int pm1a_ready = 0;
        int pm1b_ready = 0;

        for (int i = 0; i < 300; i++) {
            if (inw(facp->pm1a_cnt_blk) & SCI_EN) {
                pm1a_ready = 1;
                break;
            }
            nsleep(5);
        }
        if (facp->pm1b_cnt_blk) {
            for (int i = 0; i < 300; i++) {
                if (inw(facp->pm1b_cnt_blk) & SCI_EN) {
                    pm1b_ready = 1;
                    break;
                }
                nsleep(5);
            }
        } else {
            pm1b_ready = 1;
        }
        if (pm1a_ready && pm1b_ready) {
            plogk("facp: ACPI enabled successfully.\n");
        } else {
            plogk("facp: ACPI enablement failed.\n");
        }
    }
}

/* Get the FACP structure */
acpi_facp_t *get_acpi_facp(void)
{
    return facp;
}

/* Cycle the power */
void power_reset(void)
{
    if (!SCI_EN || !facp->reset_reg.address || !facp->reset_value) return;
    while (1) outb((uint32_t)facp->reset_reg.address, facp->reset_value);
}

/* Power off */
void power_off(void)
{
    if (!SCI_EN || !facp->pm1a_cnt_blk) return;
    while (1) {
        outw((uint32_t)facp->pm1a_cnt_blk, SLP_TYPa | SLP_EN);
        if (facp->pm1b_cnt_blk) outw((uint32_t)facp->pm1b_cnt_blk, SLP_TYPb | SLP_EN);
    }
}

/* Obtain ACPI major version */
uint8_t get_acpi_version_major(void)
{
    return facp ? facp->header.revision : 0;
}

/* Obtain ACPI minor version */
uint16_t get_acpi_version_minor(void)
{
    if (!facp || facp->header.length < sizeof(acpi_facp_t)) return 0;
    return *(uint16_t *)((uint8_t *)facp + facp->header.length - 2);
}
