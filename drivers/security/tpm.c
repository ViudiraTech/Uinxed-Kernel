/*
 *
 *      tpm.c
 *      Trusted platform module
 *
 *      2026/7/21 By MicroFish
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <acpi.h>
#include <hhdm.h>
#include <printk.h>
#include <stdint.h>
#include <tpm.h>

/* Verify TPM MMIO */
static uint32_t tpm_verify_mmio(void *virt_addr)
{
    volatile uint32_t *did_vid_ptr = (volatile uint32_t *)((uintptr_t)virt_addr + 0xF00);
    uint32_t           did_vid     = *did_vid_ptr;

    if (did_vid == 0x00000000 || did_vid == 0xFFFFFFFF) { return 0; }

    return did_vid;
}

/* ACPI-based TPM initialization */
void acpi_tpm_init(void)
{
    tpm2_table_t *tpm2 = (tpm2_table_t *)find_table("TPM2");
    if (tpm2) {
        plogk("tpm: Found TPM2 (v2.0) ACPI table at %p\n", tpm2);
        return;
    }

    tcpa_table_t *tcpa = find_table("TCPA");
    if (tcpa) {
        plogk("tpm: Found TCPA (v1.2) ACPI table at %p\n", tcpa);
        return;
    }

    void    *tpm_virt = phys_to_virt(TPM_LEGACY_BASE_PHYS);
    uint32_t did_vid  = tpm_verify_mmio(tpm_virt);

    if (did_vid) {
        plogk("tpm: Found TCPA (v1.2) at %p\n", tpm_virt);
        plogk("tpm: Detected valid Chip ID: 0x%08x\n", did_vid);
        return;
    }

    plogk("tpm: No TPM module found.\n");
}
