/*
 *
 *		pci.h
 *		PCI设备驱动头文件
 *
 *		2024/7/3 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_PCI_H_
#define INCLUDE_PCI_H_

#define PCI_CONF_VENDOR		0X0 // Vendor ID
#define PCI_CONF_DEVICE		0X2 // Device ID
#define PCI_CONF_COMMAND	0x4 // Command
#define PCI_CONF_STATUS		0x6 // Status
#define PCI_CONF_REVISION	0x8 // Revision ID

#define PCI_COMMAND_PORT	0xCF8
#define PCI_DATA_PORT		0xCFC
#define mem_mapping			0
#define input_output		1

#include "types.h"

typedef struct base_address_register {
	int prefetchable;
	uint8_t* address;
	uint32_t size;
	int type;
} base_address_register;

struct pci_config_space_public {
	unsigned short	VendorID;
	unsigned short	DeviceID;
	unsigned short	Command;
	unsigned short	Status;
	unsigned char	RevisionID;
	unsigned char	ProgIF;
	unsigned char	SubClass;
	unsigned char	BaseClass;
	unsigned char	CacheLineSize;
	unsigned char	LatencyTimer;
	unsigned char	HeaderType;
	unsigned char	BIST;
	unsigned int	BaseAddr[6];
	unsigned int	CardbusCIS;
	unsigned short	SubVendorID;
	unsigned short	SubSystemID;
	unsigned int	ROMBaseAddr;
	unsigned char	CapabilitiesPtr;
	unsigned char	Reserved[3];
	unsigned int	Reserved1;
	unsigned char	InterruptLine;
	unsigned char	InterruptPin;
	unsigned char	MinGrant;
	unsigned char	MaxLatency;
};

/* 获取PCI设备的中断号 */
uint8_t pci_get_drive_irq(uint8_t bus, uint8_t slot, uint8_t func);

/* 获取PCI设备的I/O端口基地址 */
uint32_t pci_get_port_base(uint8_t bus, uint8_t slot, uint8_t func);

/* 根据供应商ID和设备ID查找PCI设备 */
void PCI_GET_DEVICE(uint16_t vendor_id, uint16_t device_id, uint8_t* bus, uint8_t* slot, uint8_t* func);

/* 读取第n个基地址寄存器的值 */
uint32_t read_bar_n(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_n);

/* 向PCI设备寄存器写入值 */
void write_pci(uint8_t bus, uint8_t device, uint8_t function, uint8_t registeroffset, uint32_t value);

/* 读取PCI设备的命令状态寄存器 */
uint32_t pci_read_command_status(uint8_t bus, uint8_t slot, uint8_t func);

/* 向PCI设备的命令状态寄存器写入值 */
void pci_write_command_status(uint8_t bus, uint8_t slot, uint8_t func, uint32_t value);

/* 从PCI设备寄存器读取值 */
uint32_t read_pci(uint8_t bus, uint8_t device, uint8_t function, uint8_t registeroffset);

/* 获取基地址寄存器的详细信息 */
base_address_register get_base_address_register(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar);

/* 配置PCI设备 */
void pci_config(unsigned int bus, unsigned int f, unsigned int equipment, unsigned int adder);

/* 根据类代码返回设备类别名称 */
const char *pci_classname(uint32_t classcode);

/* 按ClassCode查找相应设备 */
int pci_find_class(uint32_t class_code);

/* 按名查找相应设备 */
int pci_find_name(const char *name);

/* PCI设备信息 */
void pci_device_info(void);

/* 初始化PCI设备 */
void init_pci(void);

#endif // INCLUDE_PCI_H_
