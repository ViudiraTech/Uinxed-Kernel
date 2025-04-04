/*
 *
 *		pci.c
 *		Peripheral Component Interconnect Standard Driver
 *
 *		2025/3/9 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "pci.h"
#include "common.h"
#include "printk.h"
#include "hhdm.h"

struct
{
	uint32_t classcode;
	const char *name;
} pci_classnames[] = {
	{0x000000, "Non-VGA-Compatible Unclassified Device"},
	{0x000100, "VGA-Compatible Unclassified Device"},

	{0x010000, "SCSI Bus Controller"},
	{0x010100, "IDE Controller"},
	{0x010200, "Floppy Disk Controller"},
	{0x010300, "IPI Bus Controller"},
	{0x010400, "RAID Controller"},
	{0x010500, "ATA Controller"},
	{0x010600, "Serial ATA Controller"},
	{0x010700, "Serial Attached SCSI Controller"},
	{0x010800, "Non-Volatile Memory Controller"},
	{0x018000, "Other Mass Storage Controller"},

	{0x020000, "Ethernet Controller"},
	{0x020100, "Token Ring Controller"},
	{0x020200, "FDDI Controller"},
	{0x020300, "ATM Controller"},
	{0x020400, "ISDN Controller"},
	{0x020500, "WorldFip Controller"},
	{0x020600, "PICMG 2.14 Multi Computing Controller"},
	{0x020700, "Infiniband Controller"},
	{0x020800, "Fabric Controller"},
	{0x028000, "Other Network Controller"},

	{0x030000, "VGA Compatible Controller"},
	{0x030100, "XGA Controller"},
	{0x030200, "3D Controller (Not VGA-Compatible)"},
	{0x038000, "Other Display Controller"},

	{0x040000, "Multimedia Video Controller"},
	{0x040100, "Multimedia Audio Controller"},
	{0x040200, "Computer Telephony Device"},
	{0x040300, "Audio Device"},
	{0x048000, "Other Multimedia Controller"},

	{0x050000, "RAM Controller"},
	{0x050100, "Flash Controller"},
	{0x058000, "Other Memory Controller"},

	{0x060000, "Host Bridge"},
	{0x060100, "ISA Bridge"},
	{0x060200, "EISA Bridge"},
	{0x060300, "MCA Bridge"},
	{0x060400, "PCI-to-PCI Bridge"},
	{0x060500, "PCMCIA Bridge"},
	{0x060600, "NuBus Bridge"},
	{0x060700, "CardBus Bridge"},
	{0x060800, "RACEway Bridge"},
	{0x060900, "PCI-to-PCI Bridge"},
	{0x060A00, "InfiniBand-to-PCI Host Bridge"},
	{0x068000, "Other Bridge"},

	{0x070000, "Serial Controller"},
	{0x070100, "Parallel Controller"},
	{0x070200, "Multiport Serial Controller"},
	{0x070300, "Modem"},
	{0x070400, "IEEE 488.1/2 (GPIB) Controller"},
	{0x070500, "Smart Card Controller"},
	{0x078000, "Other Simple Communication Controller"},

	{0x080000, "PIC"},
	{0x080100, "DMA Controller"},
	{0x080200, "Timer"},
	{0x080300, "RTC Controller"},
	{0x080400, "PCI Hot-Plug Controller"},
	{0x080500, "SD Host controller"},
	{0x080600, "IOMMU"},
	{0x088000, "Other Base System Peripheral"},

	{0x090000, "Keyboard Controller"},
	{0x090100, "Digitizer Pen"},
	{0x090200, "Mouse Controller"},
	{0x090300, "Scanner Controller"},
	{0x090400, "Gameport Controller"},
	{0x098000, "Other Input Device Controller"},

	{0x0A0000, "Generic"},
	{0x0A8000, "Other Docking Station"},

	{0x0B0000, "386"},
	{0x0B0100, "486"},
	{0x0B0200, "Pentium"},
	{0x0B0300, "Pentium Pro"},
	{0x0B1000, "Alpha"},
	{0x0B2000, "PowerPC"},
	{0x0B3000, "MIPS"},
	{0x0B4000, "Co-Processor"},
	{0x0B8000, "Other Processor"},

	{0x0C0000, "FireWire (IEEE 1394) Controller"},
	{0x0C0100, "ACCESS Bus Controller"},
	{0x0C0200, "SSA"},
	{0x0C0300, "USB Controller"},
	{0x0C0400, "Fibre Channel"},
	{0x0C0500, "SMBus Controller"},
	{0x0C0600, "InfiniBand Controller"},
	{0x0C0700, "IPMI Interface"},
	{0x0C0800, "SERCOS Interface (IEC 61491)"},
	{0x0C0900, "CANbus Controller"},
	{0x0C8000, "Other Serial Bus Controller"},

	{0x0D0000, "iRDA Compatible Controlle"},
	{0x0D0100, "Consumer IR Controller"},
	{0x0D1000, "RF Controller"},
	{0x0D1100, "Bluetooth Controller"},
	{0x0D1200, "Broadband Controller"},
	{0x0D2000, "Ethernet Controller (802.1a)"},
	{0x0D2100, "Ethernet Controller (802.1b)"},
	{0x0D8000, "Other Wireless Controller"},

	{0x0E0000, "I20"},

	{0x0F0000, "Satellite TV Controller"},
	{0x0F0100, "Satellite Audio Controller"},
	{0x0F0300, "Satellite Voice Controller"},
	{0x0F0400, "Satellite Data Controller"},

	{0x100000, "Network and Computing Encrpytion/Decryption"},
	{0x101000, "Entertainment Encryption/Decryption"},
	{0x108000, "Other Encryption Controller"},

	{0x110000, "DPIO Modules"},
	{0x110100, "Performance Counters"},
	{0x111000, "Communication Synchronizer"},
	{0x112000, "Signal Processing Management"},
	{0x118000, "Other Signal Processing Controller"},
	{0x000000, 0}
};

/* Reading values ​​from PCI device registers */
uint32_t read_pci(uint32_t bus, uint32_t slot, uint32_t func, uint32_t register_offset)
{
	uint32_t id = 1 << 31 | ((bus & 0xff) << 16) | ((slot & 0x1f) << 11) |
                            ((func & 0x07) << 8) | (register_offset & 0xfc);
	outl(PCI_COMMAND_PORT, id);
	uint32_t result = inl(PCI_DATA_PORT);
	return result >> (8 * (register_offset % 4));
}

/* Write values ​​to PCI device registers */
void write_pci(uint32_t bus, uint32_t slot, uint32_t func, uint32_t register_offset, uint32_t value)
{
	uint32_t id = 1 << 31 | ((bus & 0xff) << 16) | ((slot & 0x1f) << 11) |
                            ((func & 0x07) << 8) | (register_offset & 0xfc);
	outl(PCI_COMMAND_PORT, id);
	outl(PCI_DATA_PORT, value);
}

/* Read the value from the PCI device command status register */
uint32_t pci_read_command_status(uint32_t bus, uint32_t slot, uint32_t func)
{
	return read_pci(bus, slot, func, 0x04);
}

/* Write a value to the PCI device command status register */
void pci_write_command_status(uint32_t bus, uint32_t slot, uint32_t func, uint32_t value)
{
	write_pci(bus, slot, func, 0x04, value);
}

/* Get detailed information about the base address register */
base_address_register get_base_address_register(uint32_t bus, uint32_t slot, uint32_t func, uint32_t bar)
{
	base_address_register result = {0};

	uint32_t headertype = read_pci(bus, slot, func, 0x0e) & 0x7e;
	int max_bars = 6 - 4 * headertype;
	if ((int)bar >= max_bars)
		return result;

	uint32_t bar_value = read_pci(bus, slot, func, 0x10 + 4 * bar);
	result.type = (bar_value & 1) ? input_output : mem_mapping;

	if (result.type == mem_mapping) {
		switch ((bar_value >> 1) & 0x3) {
			case 0: // 32
			case 1: // 20
			case 2: // 64
				break;
		}
		result.address = (uint32_t *)(uintptr_t)phys_to_virt(bar_value & ~0x3);
		result.prefetchable = 0;
	} else {
		result.address = (uint32_t *)(uintptr_t)phys_to_virt(bar_value & ~0x3);
		result.prefetchable = 0;
	}
	return result;
}

/* Get the I/O port base address of the PCI device */
uint32_t pci_get_port_base(uint32_t bus, uint32_t slot, uint32_t func)
{
	uint32_t io_port = 0;
	for (int i = 0; i < 6; i++) {
		base_address_register bar = get_base_address_register(bus, slot, func, i);
		if (bar.type == input_output) {
			io_port = (uintptr_t)bar.address;
		}
	}
	return io_port;
}

/* Read the value of the nth base address register */
uint32_t read_bar_n(uint32_t bus, uint32_t slot, uint32_t func, uint32_t bar_n)
{
	uint32_t bar_offset = 0x10 + 4 * bar_n;
	return read_pci(bus, slot, func, bar_offset);
}

/* Get the interrupt number of the PCI device */
uint32_t pci_get_irq(uint32_t bus, uint32_t slot, uint32_t func)
{
	return read_pci(bus, slot, func, 0x3c);
}

/* Configuring PCI Devices */
void pci_config(uint32_t bus, uint32_t slot, uint32_t func, uint32_t addr)
{
	uint32_t cmd = 0;
	cmd = 0x80000000 + addr + (func << 8) + (slot << 11) + (bus << 16);
	outl(PCI_COMMAND_PORT, cmd);
}

/* Find PCI devices by vendor ID and device ID */
int pci_found_device(uint32_t vendor_id, uint32_t device_id, uint32_t *bus, uint32_t *slot, uint32_t *func)
{
	uint32_t f_bus, f_slot, f_func;
	for (f_bus = 0; f_bus < 256; f_bus++) {
		for (f_slot = 0; f_slot < 32; f_slot++) {
			for (f_func = 0; f_func < 8; f_func++) {
				pci_config(f_bus, f_slot, f_func, 0);
				if (inl(PCI_DATA_PORT) != 0xFFFFFFFF) {
					if ((read_pci(f_bus, f_slot, f_func, PCI_CONF_VENDOR) & 0xffff) == vendor_id) {
						if ((read_pci(f_bus, f_slot, f_func, PCI_CONF_DEVICE) & 0xffff) == device_id) {
							*bus = f_bus;
							*slot = f_slot;
							*func = f_func;
							return 1;
						}
					}
				}
			}
		}
	}
	return 0;
}

/* Find PCI devices by class code */
int pci_found_class(uint32_t class_code, uint32_t *bus, uint32_t *slot, uint32_t *func)
{
	uint32_t f_bus, f_slot, f_func;
	for (f_bus = 0; f_bus < 256; f_bus++) {
		for (f_slot = 0; f_slot < 32; f_slot++) {
			for (f_func = 0; f_func < 8; f_func++) {
				pci_config(f_bus, f_slot, f_func, 0);
				if (inl(PCI_DATA_PORT) != 0xFFFFFFFF) {
					uint32_t value_c = read_pci(f_bus, f_slot, f_func, PCI_CONF_REVISION);
					uint32_t found_class_code = value_c >> 8;
					if (class_code == found_class_code) {
						*bus = f_bus;
						*slot = f_slot;
						*func = f_func;
						return 1;
					}
					if (class_code == (found_class_code & 0xFFFF00)) {
						*bus = f_bus;
						*slot = f_slot;
						*func = f_func;
						return 1;
					}
				}
			}
		}
	}
	return 0;
}

/* Returns the device name based on the class code */
const char *pci_classname(uint32_t classcode)
{
	for (unsigned long i = 0; pci_classnames[i].name != 0; i++) {
		if (pci_classnames[i].classcode == classcode)
			return pci_classnames[i].name;
		if (pci_classnames[i].classcode == (classcode & 0xFFFF00))
			return pci_classnames[i].name;
	}
	return "Unknown device";
}

/* PCI device initialization */
void pci_init(void)
{
	unsigned long PCI_NUM = 0;
	uint32_t bus, slot, func;
	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				pci_config(bus, slot, func, 0);
				if (inl(PCI_DATA_PORT) != 0xFFFFFFFF) {
					uint32_t value_c = read_pci(bus, slot, func, PCI_CONF_REVISION);
					uint32_t vendor_id = (read_pci(bus, slot, func, PCI_CONF_VENDOR) & 0xffff);
					uint32_t device_id = (read_pci(bus, slot, func, PCI_CONF_DEVICE) & 0xffff);
					plogk("PCI: %03d:%02d.%01d: [0x%04x:0x%04x] %s\n",
                          bus, slot, func, vendor_id, device_id, pci_classname(value_c >> 8));
					PCI_NUM++;
				}
			}
		}
	}
	plogk("PCI: Found %d devices.\n", PCI_NUM);
}
