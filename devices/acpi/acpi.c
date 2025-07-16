/*
 *
 *      acpi.c
 *      Advanced Configuration and Power Management Interface
 *
 *      2025/2/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "acpi.h"
#include "apic.h"
#include "hhdm.h"
#include "limine.h"
#include "pci.h"
#include "printk.h"
#include "stdint.h"
#include "string.h"
#include "uinxed.h"

XSDT *xsdt = 0;
RSDT *rsdt = 0;

/* Find the corresponding ACPI table in XSDT */
void *find_table(const char *name)
{
    int use_xsdt = xsdt != 0;
    if (!use_xsdt && rsdt == 0) {
        plogk("acpi: No RSDT/XSDT available.\n");
        return 0;
    }

    uint32_t len = use_xsdt ? xsdt->h.Length : rsdt->h.Length;
    if (len < sizeof(struct ACPISDTHeader)) {
        plogk("acpi: Bogus SDT length %u\n", len);
        return 0;
    }

    const uint32_t entry_size  = use_xsdt ? 8 : 4;
    const uint32_t entry_count = (len - sizeof(struct ACPISDTHeader)) / entry_size;
    const char *entry_base     = (char *)(use_xsdt ? (void *)xsdt : (void *)rsdt) + sizeof(struct ACPISDTHeader);
    const uint32_t target_sig  = *(const uint32_t *)name;

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t phys_addr = (entry_size == 8) ? ((const uint64_t *)entry_base)[i] : ((const uint32_t *)entry_base)[i];
        struct ACPISDTHeader *h = (struct ACPISDTHeader *)phys_to_virt(phys_addr);
        if (*(const uint32_t *)h->Signature == target_sig) {
            plogk("acpi: %.4s found at %p\n", name, h);
            return h;
        }
    }
    plogk("acpi: Table %.4s not found in %s\n", name, use_xsdt ? "XSDT" : "RSDT");
    return 0;
}

/* Initialize ACPI */
void acpi_init(void)
{
    RSDP *rsdp = (RSDP *)rsdp_request.response->address;
    if (!rsdp) {
        plogk("acpi: RSDP not found.\n");
        return;
    }
    plogk("acpi: Version %s\n", rsdp->revision ? "2.0+" : "1.0");
    plogk("acpi: RSDP found at %p\n", rsdp);

    if (rsdp->revision >= 2) {
        if (!rsdp->xsdt_address) {
            plogk("acpi: XSDT pointer in RSDP is null.\n");
            return;
        } else {
            PointerCast xsdt_ptr = {.val = rsdp->xsdt_address};
            xsdt                 = (XSDT *)phys_to_virt(xsdt_ptr.val);
            plogk("acpi: XSDT found at %p\n", xsdt);
        }
    } else {
        if (!rsdp->rsdt_address) {
            plogk("acpi: RSDT pointer in RSDP is null.\n");
            return;
        } else {
            PointerCast rsdt_ptr = {.val = rsdp->rsdt_address};
            rsdt                 = (RSDT *)phys_to_virt(rsdt_ptr.val);
            plogk("acpi: RSDT found at %p\n", rsdt);
        }
    }
    load_table(HPET, hpet_init);
    load_table(APIC, apic_init);
    load_table(FACP, facp_init);
    load_table(MCFG, mcfg_init);
}
