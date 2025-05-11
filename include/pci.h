/*
 *
 *      pci.h
 *      Peripheral Component Interconnect Standard Driver Header File
 *
 *      2025/3/9 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_PCI_H_
#define INCLUDE_PCI_H_

#include "stdint.h"

#define PCI_CONF_VENDOR   0X0 // Vendor ID
#define PCI_CONF_DEVICE   0X2 // Device ID
#define PCI_CONF_COMMAND  0x4 // Command
#define PCI_CONF_STATUS   0x6 // Status
#define PCI_CONF_REVISION 0x8 // Revision ID

#define PCI_COMMAND_PORT 0xCF8
#define PCI_DATA_PORT    0xCFC

#define mem_mapping  0
#define input_output 1

typedef struct base_address_register {
        int prefetchable;
        uint32_t *address;
        uint32_t size;
        int type;
} base_address_register;

typedef struct pci_device {
        uint16_t bus;
        uint16_t slot;
        uint16_t func;
} pci_device;

typedef struct pci_device_reg {
        pci_device *parent;
        uint32_t offset;
} pci_device_reg;

/* Reading values ​​from PCI device registers */
uint32_t read_pci(pci_device_reg reg);

/* Write values ​​to PCI device registers */
void write_pci(pci_device_reg reg, uint32_t value);

/* Read the value from the PCI device command status register */
uint32_t pci_read_command_status(pci_device *device);

/* Write a value to the PCI device command status register */
void pci_write_command_status(pci_device *device, uint32_t value);

/* Get detailed information about the base address register */
base_address_register get_base_address_register(pci_device *device, uint32_t bar);

/* Get the I/O port base address of the PCI device */
uint32_t pci_get_port_base(pci_device *device);

/* Read the value of the nth base address register */
uint32_t read_bar_n(pci_device *device, uint32_t bar_n);

/* Get the interrupt number of the PCI device */
uint32_t pci_get_irq(pci_device *device);

/* Configuring PCI Devices */
void pci_config(pci_device *device, uint32_t addr);

/* Find PCI devices by vendor ID and device ID */
int pci_found_device(uint32_t vendor_id, uint32_t device_id, pci_device *device);

/* Find PCI devices by class code */
int pci_found_class(uint32_t class_code, pci_device *device);

/* Returns the device name based on the class code */
const char *pci_classname(uint32_t classcode);

/* PCI device initialization */
void pci_init(void);

#endif // INCLUDE_PCI_H_
