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
    void *table_ptr          = use_xsdt ? (void *)xsdt : (void *)rsdt;
    struct ACPISDTHeader sdt = use_xsdt ? xsdt->h : rsdt->h;

    uint32_t entry_size  = use_xsdt ? 8 : 4;
    uint32_t entry_count = (sdt.Length - sizeof(struct ACPISDTHeader)) / entry_size;
    char *entry_base     = (char *)table_ptr + sizeof(struct ACPISDTHeader);

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t phys_addr = (entry_size == 8) ? ((uint64_t *)entry_base)[i] : (uint64_t)((uint32_t *)entry_base)[i];
        struct ACPISDTHeader *ptr = (struct ACPISDTHeader *)phys_to_virt(phys_addr);
        if (memcmp(ptr->Signature, name, 4) == 0) {
            plogk("acpi: %.4s found at %p\n", name, ptr);
            return (void *)ptr;
        }
    }
    plogk("acpi: Table %.4s not found in %s\n", name, use_xsdt ? "XSDT" : "RSDT");
    return 0;
}

/* Initialize ACPI */
void acpi_init(void)
{
    struct limine_rsdp_response *response = rsdp_request.response;

    RSDP *rsdp = (RSDP *)response->address;
    if (rsdp == 0) {
        plogk("acpi: RSDP not found.\n");
        return;
    }
    plogk("acpi: Version %s\n", rsdp->revision ? "2.0+" : "1.0");
    plogk("acpi: RSDP found at %p\n", rsdp);

    if (rsdp->revision >= 2) {
        if (rsdp->xsdt_address == 0) {
            plogk("acpi: XSDT pointer in RSDP is null.\n");
        } else {
            PointerCast xsdt_ptr = {.val = rsdp->xsdt_address};
            xsdt                 = (XSDT *)phys_to_virt(xsdt_ptr.val);
            plogk("acpi: XSDT found at %p\n", xsdt);
        }
    } else {
        if (rsdp->rsdt_address == 0) {
            plogk("acpi: RSDT pointer in RSDP is null.\n");
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
    return;
}
