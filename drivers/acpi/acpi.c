/*
 *
 *      acpi.c
 *      Advanced configuration and power management interface
 *
 *      2025/2/16 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <acpi.h>
#include <apic.h>
#include <hhdm.h>
#include <limine.h>
#include <printk.h>
#include <stdint.h>
#include <uinxed.h>

xsdt_t *xsdt = 0;
rsdt_t *rsdt = 0;

/* Find the corresponding ACPI table in XSDT */
void *find_table(const char *name)
{
    int use_xsdt = xsdt != 0;
    if (!use_xsdt && !rsdt) {
        plogk("acpi: No RSDT/XSDT available.\n");
        return 0;
    }

    uint32_t len = use_xsdt ? xsdt->header.length : rsdt->header.length;
    if (len < sizeof(acpi_sdt_header_t)) {
        plogk("acpi: Bogus SDT length %u\n", len);
        return 0;
    }

    const uint32_t entry_size  = use_xsdt ? 8 : 4;
    const uint32_t entry_count = (len - sizeof(acpi_sdt_header_t)) / entry_size;
    const char    *entry_base  = (char *)(use_xsdt ? (void *)xsdt : (void *)rsdt) + sizeof(acpi_sdt_header_t);
    const uint32_t target_sig  = *(const uint32_t *)name;

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t           phys_addr = (entry_size == 8) ? ((const uint64_t *)entry_base)[i] : ((const uint32_t *)entry_base)[i];
        acpi_sdt_header_t *header    = (acpi_sdt_header_t *)phys_to_virt(phys_addr);
        if (*(const uint32_t *)header->signature == target_sig) {
            plogk("acpi: %.4s found at %p\n", name, header);
            return header;
        }
    }
    plogk("acpi: Table %.4s not found in %s\n", name, use_xsdt ? "XSDT" : "RSDT");
    return 0;
}

/* Initialize ACPI */
void acpi_init(void)
{
    rsdp_t *rsdp = (rsdp_t *)rsdp_request.response->address;
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
        }
        pointer_cast_t xsdt_ptr = {.val = rsdp->xsdt_address};
        xsdt                    = (xsdt_t *)phys_to_virt(xsdt_ptr.val);
        plogk("acpi: XSDT found at %p\n", xsdt);
    } else {
        if (!rsdp->rsdt_address) {
            plogk("acpi: RSDT pointer in RSDP is null.\n");
            return;
        }
        pointer_cast_t rsdt_ptr = {.val = rsdp->rsdt_address};
        rsdt                    = (rsdt_t *)phys_to_virt(rsdt_ptr.val);
        plogk("acpi: RSDT found at %p\n", rsdt);
    }
    load_table(HPET, hpet_init);
    load_table(APIC, apic_init);
    load_table(FACP, facp_init);
    load_table(MCFG, mcfg_init);
}
