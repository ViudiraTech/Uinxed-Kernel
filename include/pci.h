/*
 *
 *		pci.h
 *		外设部件互连标准驱动头文件
 *
 *		2025/3/9 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_PCI_H_
#define INCLUDE_PCI_H_

#include "stdint.h"

#define PCI_CONF_VENDOR		0X0 // Vendor ID
#define PCI_CONF_DEVICE		0X2 // Device ID
#define PCI_CONF_COMMAND	0x4 // Command
#define PCI_CONF_STATUS		0x6 // Status
#define PCI_CONF_REVISION	0x8 // Revision ID

#define PCI_COMMAND_PORT	0xCF8
#define PCI_DATA_PORT		0xCFC

#define mem_mapping			0
#define input_output		1

typedef struct base_address_register {
	int prefetchable;
	uint32_t *address;
	uint32_t size;
	int type;
} base_address_register;

/* 从PCI设备寄存器读取值 */
uint32_t read_pci(uint32_t bus, uint32_t slot, uint32_t func, uint32_t register_offset);

/* 向PCI设备寄存器写入值 */
void write_pci(uint32_t bus, uint32_t slot, uint32_t func, uint32_t register_offset, uint32_t value);

/* 从PCI设备命令状态寄存器读取值 */
uint32_t pci_read_command_status(uint32_t bus, uint32_t slot, uint32_t func);

/* 向PCI设备命令状态寄存器写入值 */
void pci_write_command_status(uint32_t bus, uint32_t slot, uint32_t func, uint32_t value);

/* 获取基地址寄存器的详细信息 */
base_address_register get_base_address_register(uint32_t bus, uint32_t slot, uint32_t func, uint32_t bar);

/* 获取PCI设备的I/O端口基地址 */
uint32_t pci_get_port_base(uint32_t bus, uint32_t slot, uint32_t func);

/* 读取第n个基地址寄存器的值 */
uint32_t read_bar_n(uint32_t bus, uint32_t slot, uint32_t func, uint32_t bar_n);

/* 获取PCI设备的中断号 */
uint32_t pci_get_irq(uint32_t bus, uint32_t slot, uint32_t func);

/* 配置PCI设备 */
void pci_config(uint32_t bus, uint32_t slot, uint32_t func, uint32_t addr);

/* 根据供应商ID和设备ID查找PCI设备 */
void pci_found_device(uint32_t vendor_id, uint32_t device_id, uint32_t *bus, uint32_t *slot, uint32_t *func);

/* 根据类代码返回设备名称 */
char *pci_classname(uint32_t classcode);

/* 按ClassCode查找相应设备 */
int pci_find_class(uint32_t class_code);

/* PCI设备初始化 */
void pci_init(void);

#endif // INCLUDE_PCI_H_
