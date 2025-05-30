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

#include "acpi.h"
#include "stddef.h"
#include "stdint.h"

#define PCI_HEADER_TYPE_MASK 0x7F

#define PCI_CONF_VENDOR      0x0 // Vendor ID
#define PCI_CONF_DEVICE      0x2 // Device ID
#define PCI_CONF_COMMAND     0x4 // Command
#define PCI_CONF_STATUS      0x6 // Status
#define PCI_CONF_REVISION    0x8 // Revision ID
#define PCI_CONF_HEADER_TYPE 0xe // Header Type

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

enum header_type {
    HEADER_TYPE_GENERAL = 0,
    HEADER_TYPE_BRIDGE  = 1,
    HEADER_TYPE_CARDBUS = 2,
};

/* Used to offset in ECAM */
enum ecam_area {
    ECAM_AREA_ID    = 4 * 0, // Device and vendor id
    ECAM_AREA_OPS   = 4 * 1, // Status and command
    ECAM_AREA_FIELD = 4 * 2, // Class code, subclass, prog IF and revision ID
    ECAM_AREA_OPS2  = 4 * 3, // BIST, header type, latency timer, cache line size
    ECAM_OTHERS     = 4 * 4, // Other registers
};

typedef struct pci_device_ecam {
        volatile void *id_ecam;
        volatile void *ops_ecam;
        volatile void *field_ecam;
        volatile void *ops2_ecam;
        volatile void **others;
} pci_device_ecam;

typedef struct pci_device_cache {
        pci_device *device;
        uint32_t value_c;
        uint32_t vendor_id;
        uint32_t device_id;
        uint32_t class_code;
        uint32_t header_type;
        struct pci_device_cache *next;
        pci_device_ecam ecam; // Only works in MCFG mode
} pci_device_cache;

typedef struct pci_device_reg {
        pci_device_cache *parent;
        uint32_t offset;
} pci_device_reg;

typedef struct pci_devices_cache {
        pci_device_cache *head;
        size_t devices_count;
} pci_devices_cache;

/* MCFG initialization */
void mcfg_init(void *mcfg);

/* Search MCFG entry by bus */
mcfg_entry *mcfg_search_entry(uint16_t bus);

/* Get ECAM address of register */
void *mcfg_ecam_addr(mcfg_entry *entry, pci_device_reg reg);

/* Update ECAM addresses to cache */
pci_device_ecam mcfg_update_ecam(mcfg_entry *entry, pci_device_cache *cache);

/* Reading values ​​from PCI device registers */
uint32_t read_pci(pci_device_reg reg);

/* Write values ​​to PCI device registers */
void write_pci(pci_device_reg reg, uint32_t value);

/* Read the value from the PCI device command status register */
uint32_t pci_read_command_status(pci_device_cache *device);

/* Write a value to the PCI device command status register */
void pci_write_command_status(pci_device_cache *device, uint32_t value);

/* Get detailed information about the base address register */
base_address_register get_base_address_register(pci_device_cache *device, uint32_t bar);

/* Get the I/O port base address of the PCI device */
uint32_t pci_get_port_base(pci_device_cache *device);

/* Read the value of the nth base address register */
uint32_t read_bar_n(pci_device_cache *device, uint32_t bar_n);

/* Get the interrupt number of the PCI device */
uint32_t pci_get_irq(pci_device_cache *device);

/* Configuring PCI Devices */
void pci_config(pci_device_cache *cache, uint32_t addr);

/* Find PCI devices by vendor ID and device ID */
int pci_found_device(uint32_t vendor_id, uint32_t device_id, pci_device_cache **device);

/* Find PCI devices by class code */
int pci_found_class(uint32_t class_code, pci_device_cache **device);

/* Returns the device name based on the class code */
const char *pci_classname(uint32_t classcode);

/* Returns a chached PCI devices table */
pci_devices_cache *pci_get_devices_cache(void);

/* Free the PCI devices cache */
void free_devices_cache(void);

/* Flush the PCI devices cache */
void pci_flush_devices_cache(void);

/* Found PCI devices cache by vender ID and device ID */
pci_device_cache *pci_found_device_cache(uint32_t vendor_id, uint32_t device_id);

/* Found PCI devices cache by class code */
pci_device_cache *pci_found_class_cache(uint32_t class_code);

/* PCI device initialization */
void pci_init(void);

#endif // INCLUDE_PCI_H_
