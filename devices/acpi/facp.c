/*
 *
 *      facp.c
 *      Fixed ACPI Description Table
 *
 *      2025/2/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "acpi.h"
#include "common.h"
#include "hhdm.h"
#include "printk.h"
#include "stdint.h"
#include "string.h"
#include "timer.h"

uint16_t SLP_TYPa;
uint16_t SLP_TYPb;
uint16_t SLP_EN;
uint16_t SCI_EN;
acpi_facp_t *facp;

/* Initialize facp */
void facp_init(acpi_facp_t *facp0)
{
    uint8_t *S5Addr;
    uint32_t dsdtlen;
    facp = facp0;

    PointerCast dsdt;
    dsdt.val                = (uintptr_t)facp->dsdt;
    dsdt_table_t *dsdtTable = (dsdt_table_t *)dsdt.ptr;

    if (dsdtTable == 0) {
        plogk("facp: DSDT table not found.\n");
        return;
    } else {
        dsdtTable = phys_to_virt((uint64_t)dsdtTable);
        plogk("facp: DSDT found at %p\n", dsdtTable);
    }
    if (!memcmp(dsdtTable->signature, "DSDT", 4)) {
        S5Addr  = &(dsdtTable->definition_block);
        dsdtlen = dsdtTable->length - 36;
        while (dsdtlen--) {
            if (!memcmp(S5Addr, "_S5_", 4)) break;
            S5Addr++;
        }
        SLP_EN = 1 << 13;
        SCI_EN = 1;

        if (dsdtlen && S5Addr > &dsdtTable->definition_block + 2) {
            if (*(S5Addr - 1) == 0x08 || (*(S5Addr - 2) == 0x08 && *(S5Addr - 1) == '\\')) {
                S5Addr += 5;
                S5Addr += ((*S5Addr & 0xc0) >> 6) + 2;
                if (*S5Addr == 0x0a) S5Addr++;
                SLP_TYPa = *(S5Addr) << 10;
                S5Addr++;
                if (*S5Addr == 0x0a) S5Addr++;
                SLP_TYPb = *(S5Addr) << 10;
                S5Addr++;
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

/* Cycle the power */
void power_reset(void)
{
    if (!SCI_EN) return;
    while (1) {
        outb(0x92, 0x01);
        outb((uint32_t)facp->reset_reg.address, facp->reset_value);
    }
}

/* Power off */
void power_off(void)
{
    if (!SCI_EN) return;
    while (1) {
        outw((uint32_t)facp->pm1a_cnt_blk, SLP_TYPa | SLP_EN);
        if (facp->pm1b_cnt_blk) outw((uint32_t)facp->pm1b_cnt_blk, SLP_TYPb | SLP_EN);
    }
}

/* Obtain ACPI major version */
uint8_t get_acpi_version_major(void)
{
    if (!facp) return 0;
    return facp->h.Revision;
}

/* Obtain ACPI minor version */
uint16_t get_acpi_version_minor(void)
{
    if (!facp) return 0;
    if (facp->h.Length < sizeof(acpi_facp_t)) return 0;
    return *(uint16_t *)((uint8_t *)facp + facp->h.Length - 2);
}
