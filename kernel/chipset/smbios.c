/*
 *
 *		smbios.c
 *		System Management BIOS
 *
 *		2025/3/8 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "smbios.h"
#include "limine.h"

__attribute__((used, section(".limine_requests")))
static __volatile__ struct limine_smbios_request smbios_request = {
	.id = LIMINE_SMBIOS_REQUEST
};

/* Get SMBIOS entry point */
void *smbios_entry(void)
{
	if (smbios_request.response == 0)
		return 0;
	if (smbios_request.response->entry_64)
		return (void *)smbios_request.response->entry_64;
	else if (smbios_request.response->entry_32)
		return (void *)smbios_request.response->entry_32;
	return 0;
}

/* Get the SMBIOS major version */
int smbios_major_version(void)
{
	if (smbios_request.response->entry_64)
		return ((struct EntryPoint64 *)smbios_entry())->MajorVersion;
	else
		return ((struct EntryPoint32 *)smbios_entry())->MajorVersion;
}

/* Get SMBIOS minor version */
int smbios_minor_version(void)
{
	if (smbios_request.response->entry_64)
		return ((struct EntryPoint64 *)smbios_entry())->MinorVersion;
	else
		return ((struct EntryPoint32 *)smbios_entry())->MajorVersion;
}
