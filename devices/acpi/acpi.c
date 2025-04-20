/*
 *
 *		acpi.c
 *		Advanced Configuration and Power Management Interface
 *
 *		2025/2/16 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "acpi.h"

#include "apic.h"
#include "hhdm.h"
#include "limine.h"
#include "printk.h"
#include "string.h"

XSDT *xsdt;

__attribute__((used, section(".limine_requests"))) static volatile struct limine_rsdp_request rsdp_request = {
	.id		  = LIMINE_RSDP_REQUEST,
	.revision = 0,
};

/* Find the corresponding ACPI table in XSDT */
void *find_table(const char *name)
{
	uint64_t entry_count = (xsdt->h.Length - 32) / 8;
	uint64_t *t			 = (uint64_t *)((char *)xsdt + __builtin_offsetof(XSDT, PointerToOtherSDT));

	for (uint64_t i = 0; i < entry_count; i++) {
		uint64_t ptr = (uint64_t)phys_to_virt((uint64_t) * (t + i));
		if (memcmp(((struct ACPISDTHeader *)ptr)->Signature, name, 4) == 0) {
			plogk("ACPI: %.4s 0x%016x\n", name, ptr);
			return (void *)ptr;
		}
	}
	plogk("ACPI: Table '%.4s' not found in XSDT\n", name);
	return 0;
}

/* Initialize ACPI */
void acpi_init(void)
{
	struct limine_rsdp_response *response = rsdp_request.response;

	RSDP *rsdp = (RSDP *)response->address;
	if (rsdp == 0) {
		plogk("ACPI: RSDP not found.\n");
		return;
	}
	plogk("ACPI: RSDP 0x%016x\n", rsdp);

	xsdt = (XSDT *)rsdp->xsdt_address;
	if (xsdt == 0) {
		plogk("ACPI: XSDT not found.\n");
		return;
	}
	xsdt = (XSDT *)phys_to_virt((uint64_t)xsdt);
	plogk("ACPI: XSDT 0x%016x\n", xsdt);

	void *hpet = find_table("HPET");
	if (hpet == 0) {
		plogk("ACPI: HPET table not found.\n");
		return;
	} else
		hpet_init(hpet);

	void *apic = find_table("APIC");
	if (apic == 0) {
		plogk("ACPI: APIC table not found.\n");
		return;
	} else
		apic_init(apic);

	void *facp = find_table("FACP");
	if (facp == 0) {
		plogk("ACPI: FACP table not found.\n");
		return;
	} else
		facp_init(facp);
}
