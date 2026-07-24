/*
 *
 *      pci.h
 *      Peripheral component interconnect standard driver header file
 *
 *      2025/3/9 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PCI_H_
#define INCLUDE_PCI_H_

#include <drivers/acpi.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define PCI_HEADER_TYPE_MASK 0x7F

#define PCI_CONF_VENDOR      0x0  // Vendor ID
#define PCI_CONF_DEVICE      0x2  // Device ID
#define PCI_CONF_COMMAND     0x4  // Command
#define PCI_CONF_STATUS      0x6  // Status
#define PCI_CONF_REVISION    0x8  // Revision ID
#define PCI_CONF_HEADER_TYPE 0xe  // Header Type
#define PCI_CONF_BAR0        0x10 // Base Address Register 0

#define PCI_COMMAND_PORT 0xCF8
#define PCI_DATA_PORT    0xCFC

#define mem_mapping  0
#define input_output 1

/* PCI Capability IDs */
#define PCI_CAP_ID_MSI  0x05
#define PCI_CAP_ID_MSIX 0x11

/* MSI Capability Register Offsets */
#define PCI_MSI_FLAGS         0x02
#define PCI_MSI_FLAGS_ENABLE  0x0001
#define PCI_MSI_FLAGS_64BIT   0x0080
#define PCI_MSI_FLAGS_MASKBIT 0x0100
#define PCI_MSI_FLAGS_QMASK   0x0E00
#define PCI_MSI_FLAGS_QSIZE   0x0070
#define PCI_MSI_ADDRESS_LO    0x04
#define PCI_MSI_ADDRESS_HI    0x08
#define PCI_MSI_DATA_32       0x08
#define PCI_MSI_DATA_64       0x0C
#define PCI_MSI_MASK_32       0x0C
#define PCI_MSI_MASK_64       0x10

/* MSI-X Capability Register Offsets */
#define PCI_MSIX_FLAGS              0x02
#define PCI_MSIX_FLAGS_ENABLE       0x8000
#define PCI_MSIX_FLAGS_MASKALL      0x4000
#define PCI_MSIX_FLAGS_QSIZE        0x07FF
#define PCI_MSIX_TABLE              0x04
#define PCI_MSIX_TABLE_BIR          0x00000007
#define PCI_MSIX_TABLE_OFFSET       0xFFFFFFF8
#define PCI_MSIX_PBA                0x08
#define PCI_MSIX_PBA_BIR            0x00000007
#define PCI_MSIX_PBA_OFFSET         0xFFFFFFF8
#define PCI_MSIX_ENTRY_SIZE         16
#define PCI_MSIX_ENTRY_LOWER_ADDR   0x00
#define PCI_MSIX_ENTRY_UPPER_ADDR   0x04
#define PCI_MSIX_ENTRY_DATA         0x08
#define PCI_MSIX_ENTRY_VECTOR_CTRL  0x0C
#define PCI_MSIX_ENTRY_CTRL_MASKBIT 0x0001

/* MSI Message address for x86 APIC */
#define MSI_ADDRESS_BASE       0xFEE00000
#define MSI_ADDRESS_DEST(dest) (MSI_ADDRESS_BASE | ((dest) << 12))

typedef enum {
    BAR_S32      = 0x0,
    BAR_Reserved = 0x1,
    BAR_S64      = 0x2,
} bar_size_t;

/* Flag bit in base_address_register_t.size: set for 64-bit BARs */
#define BAR_64BIT_FLAG 0x80000000

typedef struct {
        uint8_t  prefetchable;
        void    *address;
        uint32_t size;
        int      type;
} base_address_register_t;

typedef struct {
        uint16_t domain;
        uint16_t bus;
        uint16_t slot;
        uint16_t func;
} pci_device_t;

typedef enum {
    HEADER_TYPE_GENERAL = 0,
    HEADER_TYPE_BRIDGE  = 1,
    HEADER_TYPE_CARDBUS = 2,
} header_type_t;

/* Maximum MSI/MSI-X vectors per device */
#define PCI_MAX_MSI_VECTORS 32

/* MSI state stored per device */
typedef struct {
        int   msi_cap;                           /* MSI capability offset, 0 if none */
        int   msix_cap;                          /* MSI-X capability offset, 0 if none */
        int   msi_nvec;                          /* Number of MSI vectors allocated */
        int   msix_nvec;                         /* Number of MSI-X vectors allocated */
        int   msi_vectors[PCI_MAX_MSI_VECTORS];  /* Allocated MSI vectors */
        int   msix_vectors[PCI_MAX_MSI_VECTORS]; /* Allocated MSI-X vectors */
        void *msix_table;                        /* Mapped MSI-X table MMIO virtual address */
} pci_msi_state_t;

/* PCI cached searching */
typedef struct pci_device_cache {
        pci_device_t            *device;
        mcfg_entry_t            *entry;
        uint32_t                 value_c;
        uint32_t                 vendor_id;
        uint32_t                 device_id;
        uint32_t                 class_code;
        uint32_t                 header_type;
        struct pci_device_cache *next;

        /* *(ecam_ptr | (offset & 0xffc)) = ecam_addr */
        volatile void *ecam_ptr;

        pci_msi_state_t msi; /* MSI/MSI-X state */
} pci_device_cache_t;

typedef struct {
        pci_device_cache_t *parent;
        uint32_t            offset;
} pci_device_reg_t;

typedef struct {
        pci_device_cache_t *head;
        size_t              devices_count;
} pci_devices_cache_t;

/* PCI device finding */
typedef enum {
    PCI_FOUND_CLASS,  // Search by class code
    PCI_FOUND_DEVICE, // Search by vendor ID and device ID
} pci_finding_type_t;

typedef struct {
        uint32_t class_code; // Class code
} pci_class_request_t;

typedef struct {
        uint32_t vendor_id; // Vendor ID
        uint32_t device_id; // Device ID
} pci_device_request_t;

typedef enum {
    PCI_FINDING_SUCCESS = 0, // Success
    PCI_FINDING_NOT_FOUND,   // Device not found
    PCI_RESULT_EXPIRED,      // It means that iter may be expired
    PCI_FINDING_ERROR,       // Other error
} pci_finding_error_t;

typedef struct pci_finding_response_iter {
        pci_device_cache_t                        *device; // Found device cache
        pci_finding_error_t                        error;  // Error code, 0 if no error
        volatile struct pci_finding_response_iter *next;   // Maybe = NULL (but you can update it by function)
} pci_finding_response_iter_t;

typedef struct {
        pci_finding_type_t type;
        union {
                pci_class_request_t  class_req;
                pci_device_request_t device_req;
        } req;
        volatile pci_finding_response_iter_t *response; // Response pointer
} pci_finding_request_t;

typedef struct pci_usable_node {
        pci_finding_request_t  *request; // Pointer to the request
        struct pci_usable_node *next;    // Pointer to the next node
} pci_usable_node_t;

typedef struct {
        pci_usable_node_t *head;  // Head of the queue
        size_t             count; // Number of requests in the queue
} pci_usable_list_t;

typedef struct {
        uint16_t start;
        uint16_t end;
} bus_range_t;

/* Get ECAM address of register */
void *mcfg_ecam_addr(mcfg_entry_t *entry, pci_device_reg_t reg);

/* Reading values ​​from PCI device registers */
uint32_t read_pci(pci_device_reg_t reg);

/* Write values ​​to PCI device registers */
void write_pci(pci_device_reg_t reg, uint32_t value);

/* Read the value from the PCI device command status register */
uint32_t pci_read_command_status(pci_device_cache_t *device);

/* Write a value to the PCI device command status register */
void pci_write_command_status(pci_device_cache_t *device, uint32_t value);

/* Get detailed information about the base address register */
base_address_register_t get_base_address_register(pci_device_cache_t *device, uint32_t bar);

/* BAR iterator for safe traversal of Base Address Registers */
typedef struct {
        pci_device_cache_t     *device;
        uint32_t                current_bar;
        uint32_t                max_bars;
        base_address_register_t current_value;
        int                     valid;
} pci_bar_iterator_t;

/* Initialize a BAR iterator for a PCI device */
void pci_bar_iterator_init(pci_bar_iterator_t *iter, pci_device_cache_t *device);

/* Move to the next BAR, returns 0 if no more BARs */
int pci_bar_iterator_next(pci_bar_iterator_t *iter);

/* Get the current BAR value from the iterator */
base_address_register_t pci_bar_iterator_get(pci_bar_iterator_t *iter);

/* Get the I/O port base address of the PCI device */
uint32_t pci_get_port_base(pci_device_cache_t *device);

/* Read the value of the nth base address register */
uint32_t read_bar_n(pci_device_cache_t *device, uint32_t bar_n);

/* Get the interrupt number of the PCI device */
uint32_t pci_get_irq(pci_device_cache_t *device);

/* Configuring PCI Devices */
void pci_config(pci_device_cache_t *cache, uint32_t addr);

/* Finding PCI devices */
void pci_device_find(pci_finding_request_t *request);

/* Finding next matching PCI device */
void pci_device_find_next(pci_finding_request_t *request, volatile pci_finding_response_iter_t *response);

/* Update the usable list */
void pci_update_usable_list(void);

/* Returns the device name based on the class code */
const char *pci_classname(uint32_t classcode);

/* Returns a chached PCI devices table */
pci_devices_cache_t *pci_get_devices_cache(void);

/* Free the PCI devices cache */
void pci_free_devices_cache(void);

/* Flush the PCI devices cache and update the responses of each `pci_finding_request` */
void pci_flush_devices_cache(void);

/* Found PCI devices cache by vender ID and device ID */
pci_device_cache_t *pci_found_device_cache(pci_device_cache_t *start, pci_device_request_t device_req);

/* Found PCI devices cache by class code */
pci_device_cache_t *pci_found_class_cache(pci_device_cache_t *start, pci_class_request_t class_req);

/* PCI device initialization */
void pci_init(void);

/* Find a PCI capability in config space */
int pci_find_capability(pci_device_cache_t *dev, int cap_id);

/* Initialize MSI/MSI-X for a device (detect capabilities, disable at boot) */
void pci_msi_init(pci_device_cache_t *dev);

/* Enable MSI with a single vector. Returns the allocated vector number, or -1 on error. */
int pci_enable_msi(pci_device_cache_t *dev);

/* Enable MSI with up to nvec vectors. Returns number of vectors allocated, or -1 on error. */
int pci_enable_msi_range(pci_device_cache_t *dev, int nvec);

/* Disable MSI */
void pci_disable_msi(pci_device_cache_t *dev);

/* Enable MSI-X with nvec vectors. Returns the number of vectors allocated, or -1 on error. */
int pci_enable_msix(pci_device_cache_t *dev, int nvec);

/* Disable MSI-X */
void pci_disable_msix(pci_device_cache_t *dev);

/* Get interrupt vector for MSI/MSI-X (index 0..nvec-1). For MSI, use index 0. */
int pci_irq_vector(pci_device_cache_t *dev, int index);

#endif // INCLUDE_PCI_H_
