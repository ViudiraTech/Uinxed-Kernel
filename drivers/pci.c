/*
 *
 *		pci.c
 *		PCI设备驱动
 *
 *		2024/7/3 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "pci.h"
#include "memory.h"
#include "common.h"
#include "printk.h"

unsigned int PCI_ADDR_BASE;

/* 获取PCI设备的中断号 */
uint8_t pci_get_drive_irq(uint8_t bus, uint8_t slot, uint8_t func)
{
	return (uint8_t)read_pci(bus, slot, func, 0x3c);
}

/* 获取PCI设备的I/O端口基地址 */
uint32_t pci_get_port_base(uint8_t bus, uint8_t slot, uint8_t func)
{
	uint32_t io_port = 0;
	for(int i = 0;i<6;i++) {
		base_address_register bar = get_base_address_register(bus,slot,func,i);
		if(bar.type == input_output) {
			io_port = (uint32_t)bar.address;
		}
	}
	return io_port;
}

/* 根据供应商ID和设备ID查找PCI设备 */
void PCI_GET_DEVICE(uint16_t vendor_id, uint16_t device_id, uint8_t* bus, uint8_t* slot, uint8_t* func)
{
	unsigned char* pci_drive = (unsigned char*)PCI_ADDR_BASE;
	for (;; pci_drive += 0x110 + 4) {
		if (pci_drive[0] == 0xff) {
			struct pci_config_space_public* pci_config_space_puclic;
			pci_config_space_puclic = (struct pci_config_space_public*)(pci_drive + 0x0c);
			if (pci_config_space_puclic->VendorID == vendor_id &&
				pci_config_space_puclic->DeviceID == device_id) {
				*bus = pci_drive[1];
				*slot = pci_drive[2];
				*func = pci_drive[3];
				return;
			}
		} else {
			break;
		}
	}
}

/* 读取第n个基地址寄存器的值 */
uint32_t read_bar_n(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_n)
{
	uint32_t bar_offset = 0x10 + 4 * bar_n;
	return read_pci(bus, device, function, bar_offset);
}

/* 向PCI设备寄存器写入值 */
void write_pci(uint8_t bus, uint8_t device, uint8_t function, uint8_t registeroffset, uint32_t value)
{
	uint32_t id = 1 << 31 | ((bus & 0xff) << 16) | ((device & 0x1f) << 11) |
                        ((function & 0x07) << 8) | (registeroffset & 0xfc);
	outl(PCI_COMMAND_PORT, id);
	outl(PCI_DATA_PORT, value);
}

/* 读取PCI设备的命令状态寄存器 */
uint32_t pci_read_command_status(uint8_t bus, uint8_t slot, uint8_t func)
{
	return read_pci(bus, slot, func, 0x04);
}

/* 向PCI设备的命令状态寄存器写入值 */
void pci_write_command_status(uint8_t bus, uint8_t slot, uint8_t func, uint32_t value)
{
	write_pci(bus, slot, func, 0x04, value);
}

/* 从PCI设备寄存器读取值 */
uint32_t read_pci(uint8_t bus, uint8_t device, uint8_t function, uint8_t registeroffset)
{
	uint32_t id = 1 << 31 | ((bus & 0xff) << 16) | ((device & 0x1f) << 11) |
                        ((function & 0x07) << 8) | (registeroffset & 0xfc);
	outl(PCI_COMMAND_PORT, id);
	uint32_t result = inl(PCI_DATA_PORT);
	return result >> (8 * (registeroffset % 4));
}

/* 获取基地址寄存器的详细信息 */
base_address_register get_base_address_register(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar)
{
	base_address_register result;

	uint32_t headertype = read_pci(bus, device, function, 0x0e) & 0x7e;
	int max_bars = 6 - 4 * headertype;
	if (bar >= max_bars)
		return result;

	uint32_t bar_value = read_pci(bus, device, function, 0x10 + 4 * bar);
	result.type = (bar_value & 1) ? input_output : mem_mapping;

	if (result.type == mem_mapping) {
		switch ((bar_value >> 1) & 0x3) {
			case 0: // 32
			case 1: // 20
			case 2: // 64
				break;
		}
		result.address = (uint8_t*)(bar_value & ~0x3);
		result.prefetchable = 0;
	} else {
		result.address = (uint8_t*)(bar_value & ~0x3);
		result.prefetchable = 0;
	}
	return result;
}

/* 配置PCI设备 */
void pci_config(unsigned int bus, unsigned int f, unsigned int equipment, unsigned int adder)
{
	unsigned int cmd = 0;
	cmd = 0x80000000 + (unsigned int)adder + ((unsigned int)f << 8) +
           ((unsigned int)equipment << 11) + ((unsigned int)bus << 16);
	outl(PCI_COMMAND_PORT, cmd);
}

/* 初始化PCI设备 */
void init_pci(void)
{
	print_busy("Initializing PCI device...\r"); // 提示用户正在初始化PCI设备，并回到行首等待覆盖
	int PCI_NUM = 0;

	PCI_ADDR_BASE = kmalloc(1 * 1024 * 1024);
	unsigned int i, BUS, Equipment, F, ADDER, *i1;
	unsigned char *PCI_DATA = (unsigned char *)PCI_ADDR_BASE, *PCI_DATA1;

	for (BUS = 0; BUS < 256; BUS++) {						// 查询总线
		for (Equipment = 0; Equipment < 32; Equipment++) {	// 查询设备
			for (F = 0; F < 8; F++) {						// 查询功能
				pci_config(BUS, F, Equipment, 0);
				if (inl(PCI_DATA_PORT) != 0xFFFFFFFF) {
					/* 当前插槽有设备 */
					/* 把当前设备信息映射到PCI数据区 */
					int key = 1;
					while (key) {
						PCI_DATA1 = PCI_DATA;
						*PCI_DATA1 = 0xFF;		// 表占用标志
						PCI_DATA1++;
						*PCI_DATA1 = BUS;		// 总线号
						PCI_DATA1++;
						*PCI_DATA1 = Equipment;	// 设备号
						PCI_DATA1++;
						*PCI_DATA1 = F;			// 功能号
						PCI_DATA1++;
						PCI_DATA1 = PCI_DATA1 + 8;
						for (ADDER = 0; ADDER < 256; ADDER = ADDER + 4) {
							pci_config(BUS, F, Equipment, ADDER);
							i = inl(PCI_DATA_PORT);
							i1 = (unsigned int *)i;
							memcpy(PCI_DATA1, (const uint8_t *)&i, 4);
							PCI_DATA1 = PCI_DATA1 + 4;
						}
						for (uint8_t barNum = 0; barNum < 6; barNum++) {
							base_address_register bar = get_base_address_register(BUS, Equipment, F, barNum);
							if (bar.address && (bar.type == input_output)) {
								PCI_DATA1 += 4;
								int i = ((uint32_t)(bar.address));
								memcpy(PCI_DATA1, (const uint8_t *)&i, 4);
								PCI_NUM++;
							}
						}
						PCI_DATA = PCI_DATA + 0x110 + 4;
						key = 0;
					}
				}
			}
		}
	}
	print_succ("PCI devices initialized successfully,Total devices loaded: ");
	printk("%d\n", PCI_NUM);
}
