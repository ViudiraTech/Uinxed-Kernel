/*
 *
 *      pci.h
 *      Peripheral component interconnect standard driver header file
 *
 *      2025/3/9 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PCI_H_
#define INCLUDE_PCI_H_

#include <drivers/firmware/acpi.h>

#define PCI_HEADER_TYPE_MASK 0x7F

#define PCI_CONF_VENDOR      0x0  // Vendor ID
#define PCI_CONF_DEVICE      0x2  // Device ID
#define PCI_CONF_COMMAND     0x4  // Command
#define PCI_CONF_STATUS      0x6  // Status
#define PCI_CONF_REVISION    0x8  // Revision ID
#define PCI_CONF_HEADER_TYPE 0xe  // Header Type
#define PCI_CONF_BAR0        0x10 // Base Address Register 0

/* PCI Command register bits (PCI_CONF_COMMAND) */
#define PCI_CMD_IO        0x1 // I/O space decode
#define PCI_CMD_MEM       0x2 // Memory space decode
#define PCI_CMD_BUSMASTER 0x4 // Bus mastering

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

/* Flag bit in base_address_register_t.size: set for 64-bit BARs */
#define BAR_64BIT_FLAG 0x80000000

/* Maximum MSI/MSI-X vectors per device */
#define PCI_MAX_MSI_VECTORS 32

/* Interrupt request modes for pci_request_irq() */
#define PCI_IRQ_MSI    0x1 // Try MSI first
#define PCI_IRQ_MSIX   0x2 // Try MSI-X if MSI is unavailable
#define PCI_IRQ_LEGACY 0x4 // Fall back to the INTx line

typedef enum {
    BAR_S32      = 0x0,
    BAR_Reserved = 0x1,
    BAR_S64      = 0x2,
} bar_size_t;

typedef struct {
        uint8_t  prefetchable;
        void    *address;
        uint64_t size;
        int      type;
} base_address_register_t;

/* Mapped PCI BAR resource */
typedef struct {
        void    *virt; // Virtual address of the mapped BAR region
        uint64_t phys; // Physical address of the BAR region
        uint64_t size; // Size of the BAR region in bytes
} pci_bar_t;

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

/* MSI state stored per device */
typedef struct {
        int   msi_cap;                           // MSI capability offset, 0 if none
        int   msix_cap;                          // MSI-X capability offset, 0 if none
        int   msi_nvec;                          // Number of MSI vectors allocated
        int   msix_nvec;                         // Number of MSI-X vectors allocated
        int   msi_vectors[PCI_MAX_MSI_VECTORS];  // Allocated MSI vectors
        int   msix_vectors[PCI_MAX_MSI_VECTORS]; // Allocated MSI-X vectors
        void *msix_table;                        // Mapped MSI-X table MMIO virtual address
} pci_msi_state_t;

/* Interrupt state set up by pci_request_irq() */
typedef struct {
        int     vector;        // IDT vector in use, or -1 (shared dispatcher)
        uint8_t irq;           // INTx line in legacy mode
        int     mode;          // PCI_IRQ_* mode in use
        int     legacy_ioapic; // Legacy direct route: 1 = IO-APIC routing was added
} pci_irq_state_t;

/* PCI cached searching */
typedef struct pci_device_cache {
        pci_device_t            *device;
        struct device           *sysfs_dev; // /sys/bus/pci/devices/<BDF> device object
        mcfg_entry_t            *entry;
        uint32_t                 vendor_id;
        uint32_t                 device_id;
        uint32_t                 class_code;
        uint32_t                 header_type;
        struct pci_device_cache *next;

        /* *(ecam_ptr | (offset & 0xffc)) = ecam_addr */
        volatile void *ecam_ptr;

        pci_msi_state_t msi; // MSI/MSI-X state
} pci_device_cache_t;

typedef struct {
        pci_device_cache_t *parent;
        uint32_t            offset;
} pci_device_reg_t;

typedef struct {
        pci_device_cache_t *head;
        size_t              devices_count;
} pci_devices_cache_t;

/* Optional MSI-X post-enable setup hook; return 0 on success or a negative errno */
typedef int (*pci_msix_setup_fn)(pci_device_cache_t *dev, void *context);

/* Interrupt request options for pci_request_irq() */
typedef struct {
        int               modes;         // PCI_IRQ_* modes to try, in order
        void             *idt_handler;   // Handler registered at the IDT vector
        pci_msix_setup_fn msix_setup;    // Optional MSI-X setup, NULL = none
        void             *msix_context;  // Context passed to msix_setup
        int               legacy_base;   // Direct route: vector = base + irq
        int               legacy_ioapic; // Direct route: route through the IO-APIC
} pci_irq_request_t;

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
    PCI_FINDING_ERROR,       // Other error
} pci_finding_error_t;

typedef struct pci_finding_response_iter {
        pci_device_cache_t *device; // Found device cache
        pci_finding_error_t error;  // Error code, 0 if no error
} pci_finding_response_iter_t;

typedef struct {
        pci_finding_type_t type;
        union {
                pci_class_request_t  class_req;
                pci_device_request_t device_req;
        } req;
        volatile pci_finding_response_iter_t *response; // Response pointer
} pci_finding_request_t;

/* Get ECAM address of register */
void *mcfg_ecam_addr(mcfg_entry_t *entry, pci_device_reg_t reg);

/* Reading values from PCI device registers */
uint32_t read_pci(pci_device_reg_t reg);

/* Write values to PCI device registers */
void write_pci(pci_device_reg_t reg, uint32_t value);

/* Write exactly 1, 2 or 4 aligned bytes without modifying adjacent registers. */
int pci_write_config(pci_device_reg_t reg, uint32_t value, uint8_t size);

/* Size of this function's configuration space (256 or 4096 bytes). */
uint32_t pci_config_size(pci_device_cache_t *cache);

/* Read the value from the PCI device command status register */
uint32_t pci_read_command_status(pci_device_cache_t *device);

/* Write a value to the PCI device command status register */
void pci_write_command_status(pci_device_cache_t *device, uint32_t value);

/* Set the PCI command register bits */
void pci_enable_device(pci_device_cache_t *dev, uint16_t cmd_flags);

/* Clear the PCI command register bits */
void pci_disable_device(pci_device_cache_t *dev, uint16_t cmd_flags);

/* Get detailed information about the base address register */
base_address_register_t get_base_address_register(pci_device_cache_t *device, uint32_t bar);

/* Map a memory BAR of the PCI device into the kernel address space */
int pci_map_bar(pci_device_cache_t *dev, uint32_t bar, pci_bar_t *out);

/* Get the I/O port base address of the PCI device */
uint32_t pci_get_port_base(pci_device_cache_t *device);

/* Read the value of the nth base address register */
uint32_t read_bar_n(pci_device_cache_t *device, uint32_t bar_n);

/* Get the interrupt number of the PCI device */
uint32_t pci_get_irq(pci_device_cache_t *device);

/* Finding PCI devices */
void pci_device_find(pci_finding_request_t *request);

/* Returns the device name based on the class code */
const char *pci_classname(uint32_t classcode);

/* Returns a chached PCI devices table */
pci_devices_cache_t *pci_get_devices_cache(void);

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

/* Enable MSI with a single vector. Returns the allocated vector number, or negative errno. */
int pci_enable_msi(pci_device_cache_t *dev);

/* Enable MSI with up to nvec vectors. Returns number of vectors allocated, or negative errno. */
int pci_enable_msi_range(pci_device_cache_t *dev, int nvec);

/* Disable MSI */
void pci_disable_msi(pci_device_cache_t *dev);

/* Enable MSI-X with nvec vectors. Returns the number of vectors allocated, or negative errno. */
int pci_enable_msix(pci_device_cache_t *dev, int nvec);

/* Disable MSI-X */
void pci_disable_msix(pci_device_cache_t *dev);

/* Get interrupt vector for MSI/MSI-X (index 0..nvec-1). For MSI, use index 0. */
int pci_irq_vector(pci_device_cache_t *dev, int index);

/* Request a device interrupt, trying MSI, MSI-X and INTx in order */
int pci_request_irq(pci_device_cache_t *dev, pci_irq_request_t *request, pci_irq_state_t *out);

/* Release the device interrupt requested by pci_request_irq() */
void pci_free_irq(pci_device_cache_t *dev, pci_irq_state_t *state);

#endif // INCLUDE_PCI_H_
