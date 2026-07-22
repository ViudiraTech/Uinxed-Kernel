/*
 *
 *      tpm.c
 *      Trusted platform module
 *
 *      2026/7/21 By MicroFish
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/acpi.h>
#include <drivers/tpm.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <mem/hhdm.h>

/* Verify TPM MMIO */
static uint32_t tpm_verify_mmio(void *virt_addr)
{
    volatile uint32_t *did_vid_ptr = (volatile uint32_t *)((uintptr_t)virt_addr + 0xF00);
    uint32_t           did_vid     = *did_vid_ptr;

    if (did_vid == 0x00000000 || did_vid == 0xFFFFFFFF) return 0;
    return did_vid;
}

/* ACPI-based TPM initialization */
void acpi_tpm_init(void)
{
    tpm2_table_t *tpm2 = (tpm2_table_t *)find_table("TPM2");
    if (tpm2) {
        plogk("tpm: Found TPM2 (v2.0) ACPI table at %p\n", tpm2);

        uint32_t method = tpm2->start_method;
        if (method == 6) {
            plogk("tpm: interface type: TIS (method: 0x%x)\n", method);
        } else if (method == 7 || method == 8) {
            plogk("tpm: interface type: CRB (method: 0x%x)\n", method);
        } else {
            plogk("tpm: unknown interface (method: 0x%x)\n", method);
        }
        return;
    }

    tcpa_table_t *tcpa = find_table("TCPA");
    if (tcpa) {
        plogk("tpm: Found TCPA (v1.2) ACPI table at %p\n", tcpa);
        plogk("tpm: Event log physical address: 0x%lx\n", tcpa->log_area_start_address);
        plogk("tpm: interface type: TIS\n");
        return;
    }

    void    *tpm_virt = phys_to_virt(TPM_LEGACY_BASE_PHYS);
    uint32_t did_vid  = tpm_verify_mmio(tpm_virt);
    if (did_vid) {
        plogk("tpm: Found TCPA (v1.2) at %p\n", tpm_virt);
        plogk("tpm: Detected valid Chip ID: 0x%08x\n", did_vid);
        plogk("tpm: interface type: TIS\n");
        return;
    }

    plogk("tpm: No TPM module found.\n");
}
