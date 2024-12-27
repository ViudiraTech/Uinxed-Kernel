/*
 *
 *		bochs.c
 *		bochs图形模式驱动
 *
 *		2024/12/27 By ywx2012
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "common.h"
#include "pci.h"
#include "bochs.h"

#define VBE_INDEX_PORT 0x01ce
#define VBE_DATA_PORT 0x01cf
#define VBE_INDEX_ID 0x0
#define VBE_INDEX_XRES 0x1
#define VBE_INDEX_YRES 0x2
#define VBE_INDEX_BPP 0x3
#define VBE_INDEX_ENABLE 0x4

static int find_bochs_address(unsigned int BUS, unsigned int Equipment, unsigned int F, void *data)
{
	multiboot_t *info = (multiboot_t *)data;
	uint32_t value_c = read_pci(BUS, Equipment, F, PCI_CONF_REVISION);
	uint32_t class_code = value_c >> 8;
	uint16_t value_v = read_pci(BUS, Equipment, F, PCI_CONF_VENDOR);
	uint16_t value_d = read_pci(BUS, Equipment, F, PCI_CONF_DEVICE);
	uint16_t vendor_id = value_v & 0xffff;
	uint16_t device_id = value_d & 0xffff;
	if (((class_code >> 16) != 0x03) || (vendor_id != 0x1234) || (device_id != 0x1111)) {
		return 0;
	}
	outw(VBE_INDEX_PORT, VBE_INDEX_ID);
	if (inw(VBE_DATA_PORT) < 0xb0c0) {
		return 0;
	}
	outw(VBE_INDEX_PORT, VBE_INDEX_XRES);
	outw(VBE_DATA_PORT, info->framebuffer_width);
	outw(VBE_INDEX_PORT, VBE_INDEX_YRES);
	outw(VBE_DATA_PORT, info->framebuffer_height);
	outw(VBE_INDEX_PORT, VBE_INDEX_BPP);
	outw(VBE_DATA_PORT, 32);
	outw(VBE_INDEX_PORT, VBE_INDEX_ENABLE);
	outw(VBE_DATA_PORT, 1);
	void *addr = get_base_address_register(BUS, Equipment, F, 0).address;
	info->framebuffer_addr = (uint32_t)addr;
	info->framebuffer_pitch = info->framebuffer_width*4;
	info->framebuffer_bpp = 32;
	info->framebuffer_type = MULTIBOOT_FRAMEBUFFER_TYPE_RGB;
	info->framebuffer_red_field_position = 16;
	info->framebuffer_red_mask_size = 8;
	info->framebuffer_green_field_position = 8;
	info->framebuffer_green_mask_size = 8;
	info->framebuffer_blue_field_position = 0;
	info->framebuffer_blue_mask_size = 8;
	return 1;
}

void init_bochs(multiboot_t *info)
{
	if (info->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO) {
		return;
	}
	info->framebuffer_width = 1024;
	info->framebuffer_height = 768;
	if (pointer_pci_find(find_bochs_address, info)) {
		info->flags |= MULTIBOOT_INFO_FRAMEBUFFER_INFO;
	}
}
