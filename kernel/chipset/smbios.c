/*
 *
 *		smbios.c
 *		系统管理BIOS
 *
 *		2025/3/8 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "smbios.h"
#include "limine.h"

__attribute__((used, section(".limine_requests")))
static __volatile__ struct limine_smbios_request smbios_request = {
	.id = LIMINE_SMBIOS_REQUEST
};

/* 获取SMBIOS主版本 */
int smbios_major_version(void)
{
	struct EntryPoint64 *entry = (struct EntryPoint64 *)smbios_request.response->entry_64;
	return entry->SMBIOSMajorVersion;
}

/* 获取SMBIOS次版本 */
int smbios_minor_version(void)
{
	struct EntryPoint64 *entry = (struct EntryPoint64 *)smbios_request.response->entry_64;
	return entry->SMBIOSMinorVersion;
}
