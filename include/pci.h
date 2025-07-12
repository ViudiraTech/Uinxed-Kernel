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

enum BarSize {
    BAR_S32      = 0x0,
    BAR_Reserved = 0x1,
    BAR_S64      = 0x2,
};

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

/* PCI cached searching */
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

/* PCI device finding */
enum pci_finding_type {
    PCI_FOUND_CLASS,  // Search by class code
    PCI_FOUND_DEVICE, // Search by vendor ID and device ID
};

typedef struct pci_class_request {
        uint32_t class_code; // Class code
} pci_class_request;

typedef struct pci_device_request {
        uint32_t vendor_id; // Vendor ID
        uint32_t device_id; // Device ID
} pci_device_request;

enum pci_finding_error {
    PCI_FINDING_SUCCESS = 0, // Success
    PCI_FINDING_NOT_FOUND,   // Device not found
    PCI_FINDING_ERROR,       // Other error
};

typedef struct pci_finding_response {
        pci_device_cache *device;     // Found device cache
        enum pci_finding_error error; // Error code, 0 if no error
} pci_finding_response;

typedef struct pci_finding_request {
        enum pci_finding_type type;
        union {
                pci_class_request class_req;
                pci_device_request device_req;
        } req;
        volatile pci_finding_response *response; // Response pointer
} pci_finding_request;

typedef struct pci_usable_node {
        pci_finding_request *request; // Pointer to the request
        struct pci_usable_node *next; // Pointer to the next node
} pci_usable_node;

typedef struct pci_usable_list {
        pci_usable_node *head; // Head of the queue
        size_t count;          // Number of requests in the queue
} pci_usable_list;

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

/* Finding PCI devices */
void pci_device_find(pci_finding_request *request);

/* Update the usable list */
void pci_update_usable_list(void);

/* Returns the device name based on the class code */
const char *pci_classname(uint32_t classcode);

/* Returns a chached PCI devices table */
pci_devices_cache *pci_get_devices_cache(void);

/* Free the PCI devices cache */
void pci_free_devices_cache(void);

/* Flush the PCI devices cache and update the responses of each `pci_finding_request` */
void pci_flush_devices_cache(void);

/* Found PCI devices cache by vender ID and device ID */
pci_device_cache *pci_found_device_cache(pci_device_request device_req);

/* Found PCI devices cache by class code */
pci_device_cache *pci_found_class_cache(pci_class_request class_req);

/* PCI device initialization */
void pci_init(void);

#endif // INCLUDE_PCI_H_
