/*
 *
 *      pci.c
 *      Peripheral component interconnect standard driver
 *
 *      2025/3/9 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/idt.h>
#include <drivers/bus/pci.h>
#include <drivers/firmware/apic.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>

/* Available MSI vector tracking */
#define MSI_VECTOR_MIN       48
#define MSI_VECTOR_MAX       247
#define MSI_VECTOR_BMAP_SIZE ((MSI_VECTOR_MAX - MSI_VECTOR_MIN + 7) / 8)

/* PCI operations (For MCFG and legacy mode) */
typedef struct PCIOps {
        uint32_t (*read)(pci_device_reg_t reg);
        void (*write)(pci_device_reg_t reg, uint32_t value);
} pci_ops_t;

mcfg_t mcfg_info;

pci_devices_cache_t pci_cache = {
    .head          = 0,
    .devices_count = 0,
};

/* Serialise legacy CF8/CFC access (one transaction at a time on SMP) */
static spinlock_t pci_legacy_lock;

/* PCI legacy read. */
static uint32_t pci_legacy_read(pci_device_reg_t reg);

/* PCI legacy write. */
static void pci_legacy_write(pci_device_reg_t reg, uint32_t value);

/* PCI mcfg read. */
static uint32_t pci_mcfg_read(pci_device_reg_t reg);

/* PCI mcfg write. */
static void pci_mcfg_write(pci_device_reg_t reg, uint32_t value);

/* PCI scan bus. */
static void pci_scan_bus(pci_device_cache_t *cache, uint16_t bus, uint16_t end_bus);

pci_ops_t pci_ops = {
    .read  = pci_legacy_read,
    .write = pci_legacy_write,
};

struct {
        uint32_t    classcode;
        const char *name;
} pci_classnames[] = {
    {0x000000, "Non-VGA-Compatible Unclassified Device"     },
    {0x000100, "VGA-Compatible Unclassified Device"         },

    {0x010000, "SCSI Bus Controller"                        },
    {0x010100, "IDE Controller"                             },
    {0x010200, "Floppy Disk Controller"                     },
    {0x010300, "IPI Bus Controller"                         },
    {0x010400, "RAID Controller"                            },
    {0x010500, "ATA Controller"                             },
    {0x010600, "Serial ATA Controller"                      },
    {0x010700, "Serial Attached SCSI Controller"            },
    {0x010800, "Non-Volatile Memory Controller"             },
    {0x018000, "Other Mass Storage Controller"              },

    {0x020000, "Ethernet Controller"                        },
    {0x020100, "Token Ring Controller"                      },
    {0x020200, "FDDI Controller"                            },
    {0x020300, "ATM Controller"                             },
    {0x020400, "ISDN Controller"                            },
    {0x020500, "WorldFip Controller"                        },
    {0x020600, "PICMG 2.14 Multi Computing Controller"      },
    {0x020700, "Infiniband Controller"                      },
    {0x020800, "Fabric Controller"                          },
    {0x028000, "Other Network Controller"                   },

    {0x030000, "VGA Compatible Controller"                  },
    {0x030100, "XGA Controller"                             },
    {0x030200, "3D Controller (Not VGA-Compatible)"         },
    {0x038000, "Other Display Controller"                   },

    {0x040000, "Multimedia Video Controller"                },
    {0x040100, "Multimedia Audio Controller"                },
    {0x040200, "Computer Telephony Device"                  },
    {0x040300, "Audio Device"                               },
    {0x048000, "Other Multimedia Controller"                },

    {0x050000, "RAM Controller"                             },
    {0x050100, "Flash Controller"                           },
    {0x058000, "Other Memory Controller"                    },

    {0x060000, "Host Bridge"                                },
    {0x060100, "ISA Bridge"                                 },
    {0x060200, "EISA Bridge"                                },
    {0x060300, "MCA Bridge"                                 },
    {0x060400, "PCI-to-PCI Bridge"                          },
    {0x060500, "PCMCIA Bridge"                              },
    {0x060600, "NuBus Bridge"                               },
    {0x060700, "CardBus Bridge"                             },
    {0x060800, "RACEway Bridge"                             },
    {0x060900, "PCI-to-PCI Bridge"                          },
    {0x060a00, "InfiniBand-to-PCI Host Bridge"              },
    {0x068000, "Other Bridge"                               },

    {0x070000, "Serial Controller"                          },
    {0x070100, "Parallel Controller"                        },
    {0x070200, "Multiport Serial Controller"                },
    {0x070300, "Modem"                                      },
    {0x070400, "IEEE 488.1/2 (GPIB) Controller"             },
    {0x070500, "Smart Card Controller"                      },
    {0x078000, "Other Simple Communication Controller"      },

    {0x080000, "PIC"                                        },
    {0x080100, "DMA Controller"                             },
    {0x080200, "Timer"                                      },
    {0x080300, "RTC Controller"                             },
    {0x080400, "PCI Hot-Plug Controller"                    },
    {0x080500, "SD Host controller"                         },
    {0x080600, "IOMMU"                                      },
    {0x088000, "Other Base System Peripheral"               },

    {0x090000, "Keyboard Controller"                        },
    {0x090100, "Digitizer Pen"                              },
    {0x090200, "Mouse Controller"                           },
    {0x090300, "Scanner Controller"                         },
    {0x090400, "Gameport Controller"                        },
    {0x098000, "Other Input Device Controller"              },

    {0x0a0000, "Generic"                                    },
    {0x0a8000, "Other Docking Station"                      },

    {0x0b0000, "386"                                        },
    {0x0b0100, "486"                                        },
    {0x0b0200, "Pentium"                                    },
    {0x0b0300, "Pentium Pro"                                },
    {0x0b1000, "Alpha"                                      },
    {0x0b2000, "PowerPC"                                    },
    {0x0b3000, "MIPS"                                       },
    {0x0b4000, "Co-Processor"                               },
    {0x0b8000, "Other Processor"                            },

    {0x0c0000, "FireWire (IEEE 1394) Controller"            },
    {0x0c0100, "ACCESS Bus Controller"                      },
    {0x0c0200, "SSA"                                        },
    {0x0c0300, "USB Controller"                             },
    {0x0c0400, "Fibre Channel"                              },
    {0x0c0500, "SMBus Controller"                           },
    {0x0c0600, "InfiniBand Controller"                      },
    {0x0c0700, "IPMI Interface"                             },
    {0x0c0800, "SERCOS Interface (IEC 61491)"               },
    {0x0c0900, "CANbus Controller"                          },
    {0x0c8000, "Other Serial Bus Controller"                },

    {0x0d0000, "iRDA Compatible Controller"                 },
    {0x0d0100, "Consumer IR Controller"                     },
    {0x0d1000, "RF Controller"                              },
    {0x0d1100, "Bluetooth Controller"                       },
    {0x0d1200, "Broadband Controller"                       },
    {0x0d2000, "Ethernet Controller (802.1a)"               },
    {0x0d2100, "Ethernet Controller (802.1b)"               },
    {0x0d8000, "Other Wireless Controller"                  },

    {0x0e0000, "I20"                                        },

    {0x0f0000, "Satellite TV Controller"                    },
    {0x0f0100, "Satellite Audio Controller"                 },
    {0x0f0300, "Satellite Voice Controller"                 },
    {0x0f0400, "Satellite Data Controller"                  },

    {0x100000, "Network and Computing Encryption/Decryption"},
    {0x101000, "Entertainment Encryption/Decryption"        },
    {0x108000, "Other Encryption Controller"                },

    {0x110000, "DPIO Modules"                               },
    {0x110100, "Performance Counters"                       },
    {0x111000, "Communication Synchronizer"                 },
    {0x112000, "Signal Processing Management"               },
    {0x118000, "Other Signal Processing Controller"         },
    {0xFFFFFF, 0                                            },
};

static uint8_t    pci_scanned_buses[256];
static uint8_t    msi_vector_bmap[MSI_VECTOR_BMAP_SIZE];
static int        msi_initialized;
static spinlock_t msi_lock;

/* MCFG initialization */
void mcfg_init(mcfg_info_t *mcfg)
{
    if (mcfg) {
        mcfg_info_t *inner = mcfg;
        mcfg_info.count    = (inner->header.length - sizeof(acpi_sdt_header_t) - 8) / sizeof(mcfg_entry_t);

        plogk("mcfg: MCFG found with %zu entries.\n", mcfg_info.count);
        for (size_t i = 0; i < mcfg_info.count; i++) {
            /* Convert to the virtual address */
            inner->entries[i].base_addr = (uint64_t)phys_to_virt(inner->entries[i].base_addr);
            plogk("mcfg: mcfg->entries[%zu] base: %p\n", i, (void *)inner->entries[i].base_addr);
            plogk("mcfg: mcfg->entries[%zu] segment: %hu\n", i, inner->entries[i].segment);
            plogk("mcfg: mcfg->entries[%zu] start bus: %hhu\n", i, inner->entries[i].start_bus);
            plogk("mcfg: mcfg->entries[%zu] end bus: %hhu\n", i, inner->entries[i].end_bus);
        }
        mcfg_info.mcfg    = inner;
        mcfg_info.enabled = 1;

        /* Set PCI operations */
        pci_ops = (pci_ops_t) {
            .read  = pci_mcfg_read,
            .write = pci_mcfg_write,
        };
    } else {
        /* No MCFG table: fall back to legacy CF8/CFC config-space access. */
        plogk("mcfg: No MCFG table; falling back to legacy PCI config access.\n");
        mcfg_info.enabled = 0;
        pci_ops           = (pci_ops_t) {
                      .read  = pci_legacy_read,
                      .write = pci_legacy_write,
        };
    }
};

/* Get the MCFG structure */
mcfg_info_t *get_acpi_mcfg(void)
{
    return mcfg_info.mcfg;
}

/* Get ECAM address of register */
void *mcfg_ecam_addr(mcfg_entry_t *entry, pci_device_reg_t reg)
{
    pci_device_t *device = reg.parent->device;
    uint32_t      bus    = device->bus & 0xff;
    uint32_t      slot   = device->slot & 0x1f;
    uint32_t      func   = device->func & 0x07;

    /*
     * ECAM address: base + (bus_offset << 20) | (slot << 15) | (func << 12) | offset
     * The segment is used to select the MCFG entry, not part of the address.
     */
    uintptr_t addr = entry->base_addr
                     + (((bus - entry->start_bus) << 20) // Bus
                        | (slot << 15)                   // Slot
                        | (func << 12)                   // Func
                        | (reg.offset & 0xffc));         // Register
    pointer_cast_t cast;
    cast.val = addr;
    return cast.ptr;
}

/* Reading values from PCI device registers in Legacy I/O */
static uint32_t pci_legacy_read(pci_device_reg_t reg)
{
    pci_device_t *device          = reg.parent->device;
    uint32_t      register_offset = reg.offset;
    uint32_t      bus             = device->bus & 0xff;
    uint32_t      slot            = device->slot & 0x1f;
    uint32_t      func            = device->func & 0x07;

    spin_lock(&pci_legacy_lock);
    uint32_t id = (1UL << 31) | (bus << 16) | (slot << 11) | (func << 8) | (register_offset & 0xfc);
    outl(PCI_COMMAND_PORT, id);
    uint32_t val = inl(PCI_DATA_PORT) >> (8 * (register_offset % 4));
    spin_unlock(&pci_legacy_lock);

    return val;
}

/* Write values to PCI device registers in Legacy I/O */
static void pci_legacy_write(pci_device_reg_t reg, uint32_t value)
{
    pci_device_t *device          = reg.parent->device;
    uint32_t      register_offset = reg.offset;
    uint32_t      bus             = device->bus & 0xff;
    uint32_t      slot            = device->slot & 0x1f;
    uint32_t      func            = device->func & 0x07;
    uint32_t      byte_offset     = register_offset % 4;

    spin_lock(&pci_legacy_lock);
    uint32_t id = (1UL << 31) | (bus << 16) | (slot << 11) | (func << 8) | (register_offset & 0xfc);
    outl(PCI_COMMAND_PORT, id);

    if (!byte_offset) {
        outl(PCI_DATA_PORT, value);
    } else {
        uint32_t old            = inl(PCI_DATA_PORT);
        uint32_t bytes_to_write = 4 - byte_offset;
        uint32_t val_mask       = (uint32_t)(0xffffffff >> (32 - 8 * bytes_to_write));
        uint32_t reg_clear      = val_mask << (8 * byte_offset);
        outl(PCI_DATA_PORT, (old & ~reg_clear) | ((value & val_mask) << (8 * byte_offset)));
    }

    spin_unlock(&pci_legacy_lock);
}

/* Write values to PCI device registers from `pci_device_ecam` */
static void pci_mcfg_write(pci_device_reg_t reg, uint32_t value)
{
    uint32_t           offset = reg.offset % 4;
    volatile uint32_t *ptr    = (volatile uint32_t *)(reg.parent->ecam_ptr + (reg.offset & 0xffc));

    if (!offset) {
        *ptr = value;
    } else {
        /*
         * Sub-dword write: RMW to preserve adjacent bytes.
         * Note: registers with W1C semantics should be accessed at
         * their natural alignment to avoid RMW races.
         */
        uint32_t bytes_to_write = 4 - offset;
        uint32_t val_mask       = (uint32_t)(0xffffffff >> (32 - 8 * bytes_to_write));
        uint32_t reg_clear      = val_mask << (8 * offset);
        *ptr                    = (*ptr & ~reg_clear) | ((value & val_mask) << (8 * offset));
    }
}

/* Reading values from PCI device registers and `pci_device_ecam` */
static uint32_t pci_mcfg_read(pci_device_reg_t reg)
{
    uint32_t           offset = reg.offset % 4;
    volatile uint32_t *ptr    = (volatile uint32_t *)(reg.parent->ecam_ptr + (reg.offset & 0xffc));
    return *ptr >> (8 * offset);
}

/* Reading values from PCI device registers */
uint32_t read_pci(pci_device_reg_t reg)
{
    return pci_ops.read(reg);
}

/* Write values to PCI device registers */
void write_pci(pci_device_reg_t reg, uint32_t value)
{
    return pci_ops.write(reg, value);
}

/* 4 KiB for PCI Express over ECAM, 256 bytes otherwise. */
uint32_t pci_config_size(pci_device_cache_t *cache)
{
    return cache->ecam_ptr && pci_find_capability(cache, 0x10) ? 4096 : 256;
}

/* Width-specific configuration writes must not read-modify-write W1C neighbors. */
int pci_write_config(pci_device_reg_t reg, uint32_t value, uint8_t size)
{
    if (!reg.parent || !reg.parent->device || (size != 1 && size != 2 && size != 4)) return -EINVAL;
    uint32_t limit = pci_config_size(reg.parent);
    if ((reg.offset & (size - 1U)) || reg.offset >= limit || size > limit - reg.offset) return -EINVAL;

    if (reg.parent->ecam_ptr) {
        volatile uint8_t *address = (volatile uint8_t *)reg.parent->ecam_ptr + reg.offset;
        if (size == 1)
            mmio_write8(address, (uint8_t)value);
        else if (size == 2)
            mmio_write16(address, (uint16_t)value);
        else
            mmio_write32(address, value);
        return EOK;
    }

    pci_device_t *device  = reg.parent->device;
    uint32_t      address = (1U << 31) | (((uint32_t)device->bus & 0xff) << 16) | (((uint32_t)device->slot & 0x1f) << 11) | (((uint32_t)device->func & 0x07) << 8) | (reg.offset & 0xfc);
    uint64_t      flags   = spin_lock_irqsave(&pci_legacy_lock);
    outl(PCI_COMMAND_PORT, address);
    uint16_t port = PCI_DATA_PORT + (reg.offset & 3);
    if (size == 1)
        outb(port, (uint8_t)value);
    else if (size == 2)
        outw(port, (uint16_t)value);
    else
        outl(port, value);
    spin_unlock_irqrestore(&pci_legacy_lock, flags);
    return EOK;
}

/* Read the value from the PCI device command status register */
uint32_t pci_read_command_status(pci_device_cache_t *device)
{
    pci_device_reg_t reg = {device, PCI_CONF_COMMAND};
    return read_pci(reg);
}

/* Write a value to the PCI device command status register */
void pci_write_command_status(pci_device_cache_t *device, uint32_t value)
{
    pci_device_reg_t reg = (pci_device_reg_t) {device, PCI_CONF_COMMAND};
    write_pci(reg, value);
}

/* Set the PCI command register bits */
void pci_enable_device(pci_device_cache_t *dev, uint16_t cmd_flags)
{
    if (!dev) return;
    uint32_t cmd = pci_read_command_status(dev) & 0xFFFF;
    pci_write_command_status(dev, cmd | cmd_flags);
}

/* Clear the PCI command register bits */
void pci_disable_device(pci_device_cache_t *dev, uint16_t cmd_flags)
{
    if (!dev) return;
    uint32_t cmd = pci_read_command_status(dev) & 0xFFFF;
    pci_write_command_status(dev, cmd & ~cmd_flags);
}

/* Map a memory BAR of the PCI device into the kernel address space */
int pci_map_bar(pci_device_cache_t *dev, uint32_t bar, pci_bar_t *out)
{
    if (!dev || !out) return -EINVAL;

    base_address_register_t bar_info = get_base_address_register(dev, bar);
    if (bar_info.type != mem_mapping || !bar_info.address) return -ENODEV;

    /* Reject uninitialized, I/O and reserved-encoding BARs */
    uint32_t raw = read_bar_n(dev, bar);
    if (raw == 0xffffffff || (raw & 1) || (((raw >> 1) & 0b11) == BAR_Reserved)) return -ENODEV;

    uint64_t bar_physical = (uint64_t)virt_to_phys((uint64_t)bar_info.address);
    if (!bar_physical) return -ENODEV; // Uninitialized/zero BAR
    uint64_t bar_size = bar_info.size & ~BAR_64BIT_FLAG;
    if (!bar_size) bar_size = PAGE_4K_SIZE;

    uint64_t map_start  = bar_physical & ~(PAGE_4K_SIZE - 1);
    uint64_t map_length = (bar_physical + bar_size + PAGE_4K_SIZE - 1) & ~(PAGE_4K_SIZE - 1);
    map_length -= map_start;
    page_map_range_to(get_kernel_pagedir(), map_start, map_length, PTE_MMIO_FLAGS);

    out->virt = bar_info.address;
    out->phys = bar_physical;
    out->size = bar_size;
    return 0;
}

/* Get detailed information about the base address register */
base_address_register_t get_base_address_register(pci_device_cache_t *device, uint32_t bar)
{
    base_address_register_t result = {0};
    pci_device_reg_t        reg    = {device, 0};

    uint32_t headertype = device->header_type & (PCI_HEADER_TYPE_MASK & ~0x1);
    uint32_t max_bars;

    static uint32_t max_bars_table[4] = {6, 2, 1, 0};
    max_bars                          = max_bars_table[headertype < 3 ? headertype : 3];

    if (bar >= max_bars) return result;
    reg.offset        = PCI_CONF_BAR0 + (4 * bar);
    uint32_t bar_orig = read_pci(reg);

    if (bar_orig == 0xFFFFFFFF) return result;
    result.type = (bar_orig & 1) ? input_output : mem_mapping;

    /* Determine BAR type (32-bit vs 64-bit) */
    int bar_type = BAR_S32;
    if (result.type == mem_mapping) {
        bar_type            = (bar_orig >> 1) & 0b11;
        result.prefetchable = (bar_orig >> 3) & 1;
    }

    /* For 64-bit memory BARs, read the upper dword */
    uint64_t bar_full = bar_orig;
    if (bar_type == BAR_S64) {
        if (bar + 1 >= max_bars) return result;
        reg.offset = 0x10 + (4 * (bar + 1));
        bar_full |= (uint64_t)read_pci(reg) << 32;
    }

    /* Save then probe BAR size (write all 1s, read back, invert & mask) */
    uint64_t bar_saved   = bar_full;
    uint64_t region_size = 0;

    reg.offset = PCI_CONF_BAR0 + (4 * bar);
    write_pci(reg, 0xFFFFFFFF);
    uint64_t probe = read_pci(reg);

    if (bar_type == BAR_S64) {
        reg.offset = 0x10 + (4 * (bar + 1));
        write_pci(reg, 0xFFFFFFFF);
        probe |= (uint64_t)read_pci(reg) << 32;
    }
    if (result.type == mem_mapping) {
        region_size = ~(probe & ~0b1111ULL) + 1;
    } else {
        region_size = ~(probe & ~0b11ULL) + 1;
    }

    /* Restore original BAR value */
    reg.offset = PCI_CONF_BAR0 + (4 * bar);
    write_pci(reg, bar_saved);

    if (bar_type == BAR_S64) {
        reg.offset = 0x10 + (4 * (bar + 1));
        write_pci(reg, bar_saved >> 32);
    }

    /*
     * size: region bytes. For 64-bit BARs, the BAR_64BIT_FLAG is set
     * so iterators know to skip the next BAR register slot.
     */
    result.size = region_size;
    if (bar_type == BAR_S64) result.size |= BAR_64BIT_FLAG;
    if (result.type == mem_mapping) {
        result.address = phys_to_virt(bar_full & ~0b1111ULL);
    } else {
        result.address = (void *)(uintptr_t)(bar_full & ~0b11);
    }

    return result;
}

/* Get the I/O port base address of the PCI device */
uint32_t pci_get_port_base(pci_device_cache_t *device)
{
    uint32_t io_port = 0;
    for (int i = 0; i < 6; i++) {
        base_address_register_t bar = get_base_address_register(device, i);
        if (bar.type == input_output) io_port = (uint32_t)(uintptr_t)bar.address;
        if (bar.size & BAR_64BIT_FLAG) i++;
    }
    return io_port;
}

/* Read the value of the nth base address register */
uint32_t read_bar_n(pci_device_cache_t *device, uint32_t bar_n)
{
    pci_device_reg_t reg = {device, PCI_CONF_BAR0 + (4 * bar_n)};
    return read_pci(reg);
}

/* Get the interrupt number of the PCI device (Interrupt Line register, byte at 0x3c) */
uint32_t pci_get_irq(pci_device_cache_t *device)
{
    pci_device_reg_t reg = {device, 0x3c};
    return read_pci(reg) & 0xFF;
}

/* Initialize MSI vector allocator */
static void msi_vector_init(void)
{
    uint64_t rflags = spin_lock_irqsave(&msi_lock);
    if (msi_initialized) {
        spin_unlock_irqrestore(&msi_lock, rflags);
        return;
    }

    msi_initialized = 1;
    int reserved[]  = {
        0x52, 0x53, 0x54, 0x55, // IPIs
        0x80,                   // Syscall
        0xFF,                   // Spurious
    };

    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (reserved[i] >= MSI_VECTOR_MIN && reserved[i] <= MSI_VECTOR_MAX) {
            int idx = reserved[i] - MSI_VECTOR_MIN;
            msi_vector_bmap[idx / 8] |= (1 << (idx % 8));
        }
    }
    spin_unlock_irqrestore(&msi_lock, rflags);
}

/* Allocate a single MSI vector. Returns vector number or negative errno. */
static int msi_vector_alloc(int nvec)
{
    (void)nvec;
    msi_vector_init();

    uint64_t rflags = spin_lock_irqsave(&msi_lock);
    for (int i = MSI_VECTOR_MIN; i <= MSI_VECTOR_MAX; i++) {
        int idx = i - MSI_VECTOR_MIN;
        if (!(msi_vector_bmap[idx / 8] & (1 << (idx % 8)))) {
            msi_vector_bmap[idx / 8] |= (1 << (idx % 8));
            spin_unlock_irqrestore(&msi_lock, rflags);
            return i;
        }
    }
    spin_unlock_irqrestore(&msi_lock, rflags);
    return -ENOSPC;
}

/* Free a previously allocated MSI vector */
static void msi_vector_free(int vector)
{
    if (vector < MSI_VECTOR_MIN || vector > MSI_VECTOR_MAX) return;

    uint64_t rflags = spin_lock_irqsave(&msi_lock);
    int      idx    = vector - MSI_VECTOR_MIN;
    msi_vector_bmap[idx / 8] &= ~(1 << (idx % 8));
    spin_unlock_irqrestore(&msi_lock, rflags);
}

/* Compute MSI message address for targeting local APIC */
static uint32_t msi_message_address(void)
{
    /* MSI destination ID is 8 bits wide (bits 19:12 of address). lapic_id() may return a wider x2APIC ID; mask to 8 bits. */
    return MSI_ADDRESS_DEST(lapic_id() & 0xFF);
}

/* Compute MSI message data for the given vector, fixed delivery mode */
static uint32_t msi_message_data(int vector)
{
    return (uint32_t)vector;
}

/* Find a PCI capability in config space */
int pci_find_capability(pci_device_cache_t *dev, int cap_id)
{
    if (!dev) return 0;
    pci_device_reg_t reg    = {dev, PCI_CONF_STATUS};
    uint32_t         status = read_pci(reg);

    if (!(status & (1 << 4))) return 0;
    reg.offset         = 0x34;
    uint8_t cap_offset = read_pci(reg) & 0xFF;
    if (!cap_offset) return 0;

    int visited = 0;
    while (cap_offset && visited < 48) {
        reg.offset      = cap_offset;
        uint32_t header = read_pci(reg);
        uint8_t  id     = header & 0xFF;
        uint8_t  next   = (header >> 8) & 0xFF;

        if (id == cap_id) return cap_offset;
        cap_offset = next;
        visited++;
    }
    return 0;
}

/* Initialize MSI for a device: detect capability and ensure it's disabled */
void pci_msi_init(pci_device_cache_t *dev)
{
    if (!dev) return;

    dev->msi.msi_cap    = pci_find_capability(dev, PCI_CAP_ID_MSI);
    dev->msi.msix_cap   = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    dev->msi.msi_nvec   = 0;
    dev->msi.msix_nvec  = 0;
    dev->msi.msix_table = 0;

    /* Disable MSI if left enabled by firmware */
    if (dev->msi.msi_cap) {
        pci_device_reg_t reg  = {dev, dev->msi.msi_cap + PCI_MSI_FLAGS};
        uint16_t         ctrl = read_pci(reg) & 0xFFFF;
        if (ctrl & PCI_MSI_FLAGS_ENABLE) write_pci(reg, ctrl & ~PCI_MSI_FLAGS_ENABLE);
    }

    /* Disable MSI-X if left enabled by firmware */
    if (dev->msi.msix_cap) {
        pci_device_reg_t reg  = {dev, dev->msi.msix_cap + PCI_MSIX_FLAGS};
        uint16_t         ctrl = read_pci(reg) & 0xFFFF;
        if (ctrl & PCI_MSIX_FLAGS_ENABLE) write_pci(reg, ctrl & ~(PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL));
    }
}

/* Enable MSI with a single vector. Returns the allocated vector number, or negative errno. */
int pci_enable_msi(pci_device_cache_t *dev)
{
    if (pci_enable_msi_range(dev, 1) != 1) return -ENOSPC;
    return dev->msi.msi_vectors[0];
}

/* Enable MSI with up to nvec vectors. Returns number of vectors allocated, or negative errno. */
int pci_enable_msi_range(pci_device_cache_t *dev, int nvec)
{
    if (!dev || !dev->msi.msi_cap || nvec < 1) return -EINVAL;
    if (dev->msi.msi_nvec) return -EBUSY;
    if (nvec > PCI_MAX_MSI_VECTORS) nvec = PCI_MAX_MSI_VECTORS;

    int              cap       = dev->msi.msi_cap;
    pci_device_reg_t flags_reg = {dev, cap + PCI_MSI_FLAGS};
    uint16_t         flags     = read_pci(flags_reg) & 0xFFFF;
    int              is_64     = !!(flags & PCI_MSI_FLAGS_64BIT);
    int              can_mask  = !!(flags & PCI_MSI_FLAGS_MASKBIT);

    /* Determine max vectors supported by device */
    int multi_cap = (flags & PCI_MSI_FLAGS_QMASK) >> 9;
    int max_nvec  = 1 << multi_cap;
    if (nvec > max_nvec) nvec = max_nvec;

    /* Multiple Message Enable is a power of two and must not exceed the request. */
    int allocated_nvec = 1;
    while ((allocated_nvec << 1) <= nvec) allocated_nvec <<= 1;

    int qsize = 0;
    while ((1 << qsize) < allocated_nvec) qsize++;

    /* Allocate vectors contiguously under the msi_lock. */
    msi_vector_init();
    uint64_t rflags = spin_lock_irqsave(&msi_lock);
    int      found  = -1;

    for (int i = MSI_VECTOR_MIN; i <= MSI_VECTOR_MAX - allocated_nvec + 1; i++) {
        if (i & (allocated_nvec - 1)) continue;
        int ok = 1;

        for (int j = 0; j < allocated_nvec; j++) {
            int idx = (i + j) - MSI_VECTOR_MIN;
            if (msi_vector_bmap[idx / 8] & (1 << (idx % 8))) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            for (int j = 0; j < allocated_nvec; j++) {
                int idx = (i + j) - MSI_VECTOR_MIN;
                msi_vector_bmap[idx / 8] |= (1 << (idx % 8));
                dev->msi.msi_vectors[j] = i + j;
            }
            found = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&msi_lock, rflags);

    if (found < 0) {
        plogk("pci: %04x:%02x:%02x.%01x: no contiguous MSI vectors available for %d vector(s)\n", dev->device->domain, dev->device->bus, dev->device->slot, dev->device->func, allocated_nvec);
        return -ENOSPC;
    }

    int first_vector = dev->msi.msi_vectors[0];

    /* Build MSI message */
    uint32_t addr_lo  = msi_message_address();
    uint32_t addr_hi  = 0;
    uint32_t msg_data = msi_message_data(first_vector);

    /* Disable MSI while programming */
    write_pci(flags_reg, flags & ~PCI_MSI_FLAGS_ENABLE);

    /* Program the MSI capability registers */
    pci_device_reg_t reg = {dev, 0};

    reg.offset = cap + PCI_MSI_ADDRESS_LO;
    write_pci(reg, addr_lo);

    if (is_64) {
        reg.offset = cap + PCI_MSI_ADDRESS_HI;
        write_pci(reg, addr_hi);

        reg.offset = cap + PCI_MSI_DATA_64;
        write_pci(reg, msg_data);

        if (can_mask) {
            reg.offset = cap + PCI_MSI_MASK_64;
            write_pci(reg, 0);
        }
    } else {
        reg.offset = cap + PCI_MSI_DATA_32;
        write_pci(reg, msg_data);

        if (can_mask) {
            reg.offset = cap + PCI_MSI_MASK_32;
            write_pci(reg, 0);
        }
    }

    /* Set QSIZE (Multiple Message Enable) and enable MSI */
    flags &= ~PCI_MSI_FLAGS_QSIZE;
    flags |= (qsize << 4) & PCI_MSI_FLAGS_QSIZE;
    flags |= PCI_MSI_FLAGS_ENABLE;
    write_pci(flags_reg, flags);

    /* Disable INTx */
    reg.offset   = PCI_CONF_COMMAND;
    uint16_t cmd = read_pci(reg) & 0xFFFF;
    cmd |= (1 << 10);
    write_pci(reg, cmd);

    dev->msi.msi_nvec = allocated_nvec;
    return allocated_nvec;
}

/* Disable MSI */
void pci_disable_msi(pci_device_cache_t *dev)
{
    if (!dev || !dev->msi.msi_cap || !dev->msi.msi_nvec) return;
    int cap = dev->msi.msi_cap;

    /* Disable MSI */
    pci_device_reg_t flags_reg = {dev, cap + PCI_MSI_FLAGS};
    uint16_t         flags     = read_pci(flags_reg) & 0xFFFF;
    write_pci(flags_reg, flags & ~PCI_MSI_FLAGS_ENABLE);

    /* Free allocated vectors */
    for (int i = 0; i < dev->msi.msi_nvec; i++) msi_vector_free(dev->msi.msi_vectors[i]);

    /* Re-enable INTx */
    pci_device_reg_t cmd_reg = {dev, PCI_CONF_COMMAND};
    uint16_t         cmd     = read_pci(cmd_reg) & 0xFFFF;
    cmd &= ~(1 << 10);
    write_pci(cmd_reg, cmd);
    dev->msi.msi_nvec = 0;
}

/* Map MSI-X table from PCI BAR */
static int msix_map_table(pci_device_cache_t *dev, int nvec)
{
    int              cap          = dev->msi.msix_cap;
    pci_device_reg_t reg          = {dev, cap + PCI_MSIX_TABLE};
    uint32_t         table_info   = read_pci(reg);
    int              bir          = table_info & PCI_MSIX_TABLE_BIR;
    uint32_t         table_offset = table_info & PCI_MSIX_TABLE_OFFSET;

    if (bir >= 6) {
        plogk("pci: %04x:%02x:%02x.%01x: MSI-X table BIR %d out of range.\n", dev->device->domain, dev->device->bus, dev->device->slot, dev->device->func, bir);
        return -EINVAL;
    }

    base_address_register_t bar = get_base_address_register(dev, bir);

    if (!bar.address || bar.type != mem_mapping) {
        plogk("pci: %04x:%02x:%02x.%01x: MSI-X BAR %d is not a memory BAR.\n", dev->device->domain, dev->device->bus, dev->device->slot, dev->device->func, bir);
        return -ENODEV;
    }

    uint64_t bar_size   = bar.size & ~BAR_64BIT_FLAG;
    uint64_t table_size = (uint64_t)nvec * PCI_MSIX_ENTRY_SIZE;

    if (table_offset >= bar_size || table_size > bar_size - table_offset) {
        plogk("pci: %04x:%02x:%02x.%01x: MSI-X table (offset 0x%x, %llu bytes) does not fit BAR %d (size 0x%llx)\n", dev->device->domain, dev->device->bus, dev->device->slot, dev->device->func,
              table_offset, table_size, bir, bar_size);
        return -EINVAL;
    }

    uint64_t table_phys = (uint64_t)(uintptr_t)virt_to_phys((uint64_t)(uintptr_t)bar.address) + table_offset;
    uint64_t map_start  = table_phys & ~(PAGE_4K_SIZE - 1);
    uint64_t map_end    = (table_phys + table_size + PAGE_4K_SIZE - 1) & ~(PAGE_4K_SIZE - 1);

    page_map_range_to(get_kernel_pagedir(), map_start, map_end - map_start, PTE_MMIO_FLAGS);
    dev->msi.msix_table = phys_to_virt(table_phys);

    return 0;
}

/* Enable MSI-X with nvec vectors. Returns the number of vectors allocated, or negative errno. */
int pci_enable_msix(pci_device_cache_t *dev, int nvec)
{
    if (!dev || !dev->msi.msix_cap || nvec < 1) return -EINVAL;
    if (dev->msi.msix_nvec) return -EBUSY;
    if (nvec > PCI_MAX_MSI_VECTORS) nvec = PCI_MAX_MSI_VECTORS;

    int cap = dev->msi.msix_cap;

    /* Read MSI-X control register to get table size */
    pci_device_reg_t flags_reg  = {dev, cap + PCI_MSIX_FLAGS};
    uint16_t         flags      = read_pci(flags_reg) & 0xFFFF;
    int              table_size = (flags & PCI_MSIX_FLAGS_QSIZE) + 1;

    if (nvec > table_size) nvec = table_size;

    /* Map the MSI-X table */
    if (msix_map_table(dev, nvec) < 0) return -ENODEV;
    if (!dev->msi.msix_table) return -ENODEV;

    /* Allocate vectors */
    for (int i = 0; i < nvec; i++) {
        dev->msi.msix_vectors[i] = msi_vector_alloc(1);
        if (dev->msi.msix_vectors[i] < 0) {
            plogk("pci: %04x:%02x:%02x.%01x: MSI-X vector allocation failed at index %d\n", dev->device->domain, dev->device->bus, dev->device->slot, dev->device->func, i);
            for (int j = 0; j < i; j++) msi_vector_free(dev->msi.msix_vectors[j]);
            dev->msi.msix_table = 0;
            return -ENOSPC;
        }
    }

    /* Mask all entries and enable MSI-X with MaskAll */
    write_pci(flags_reg, (flags & ~PCI_MSIX_FLAGS_ENABLE) | PCI_MSIX_FLAGS_MASKALL);

    /* Program each MSI-X table entry via MMIO */
    uint32_t addr_lo = msi_message_address();
    uint32_t addr_hi = 0;

    for (int i = 0; i < nvec; i++) {
        volatile uint32_t *entry    = (volatile uint32_t *)((uintptr_t)dev->msi.msix_table + ((uintptr_t)i * PCI_MSIX_ENTRY_SIZE));
        uint32_t           msg_data = msi_message_data(dev->msi.msix_vectors[i]);

        entry[PCI_MSIX_ENTRY_VECTOR_CTRL / 4] |= PCI_MSIX_ENTRY_CTRL_MASKBIT;
        dma_full_barrier();

        entry[PCI_MSIX_ENTRY_LOWER_ADDR / 4] = addr_lo;
        entry[PCI_MSIX_ENTRY_UPPER_ADDR / 4] = addr_hi;
        entry[PCI_MSIX_ENTRY_DATA / 4]       = msg_data;
        dma_full_barrier();

        entry[PCI_MSIX_ENTRY_VECTOR_CTRL / 4] &= ~PCI_MSIX_ENTRY_CTRL_MASKBIT;
    }

    /* Clear MaskAll and set Enable */
    dma_full_barrier();
    write_pci(flags_reg, (flags & ~PCI_MSIX_FLAGS_MASKALL) | PCI_MSIX_FLAGS_ENABLE);

    /* Disable INTx */
    pci_device_reg_t cmd_reg = {dev, PCI_CONF_COMMAND};
    uint16_t         cmd     = read_pci(cmd_reg) & 0xFFFF;
    cmd |= (1 << 10);
    write_pci(cmd_reg, cmd);
    dev->msi.msix_nvec = nvec;
    return nvec;
}

/* Disable MSI-X */
void pci_disable_msix(pci_device_cache_t *dev)
{
    if (!dev || !dev->msi.msix_cap || !dev->msi.msix_nvec) return;
    int cap = dev->msi.msix_cap;

    /* Disable MSI-X and set MaskAll */
    pci_device_reg_t flags_reg = {dev, cap + PCI_MSIX_FLAGS};
    uint16_t         flags     = read_pci(flags_reg) & 0xFFFF;
    write_pci(flags_reg, (flags & ~PCI_MSIX_FLAGS_ENABLE) | PCI_MSIX_FLAGS_MASKALL);

    /* Mask every entry before any vector can be reused. */
    for (int i = 0; i < dev->msi.msix_nvec; i++) {
        if (dev->msi.msix_table) {
            volatile uint32_t *entry = (volatile uint32_t *)((uintptr_t)dev->msi.msix_table + ((uintptr_t)i * PCI_MSIX_ENTRY_SIZE));
            entry[PCI_MSIX_ENTRY_VECTOR_CTRL / 4] |= PCI_MSIX_ENTRY_CTRL_MASKBIT;
        }
    }
    dma_full_barrier();
    for (int i = 0; i < dev->msi.msix_nvec; i++) msi_vector_free(dev->msi.msix_vectors[i]);

    /* Re-enable INTx */
    pci_device_reg_t cmd_reg = {dev, PCI_CONF_COMMAND};
    uint16_t         cmd     = read_pci(cmd_reg) & 0xFFFF;
    cmd &= ~(1 << 10);
    write_pci(cmd_reg, cmd);
    dev->msi.msix_nvec  = 0;
    dev->msi.msix_table = 0;
}

/* Get interrupt vector for MSI/MSI-X (index 0..nvec-1) */
int pci_irq_vector(pci_device_cache_t *dev, int index)
{
    if (!dev || index < 0) return -EINVAL;
    if (dev->msi.msix_nvec && index < dev->msi.msix_nvec) return dev->msi.msix_vectors[index];
    if (dev->msi.msi_nvec && index < dev->msi.msi_nvec) return dev->msi.msi_vectors[index];
    return -ENODEV;
}

/* Unified PCI interrupt request: MSI -> MSI-X -> INTx */
int pci_request_irq(pci_device_cache_t *dev, pci_irq_request_t *request, pci_irq_state_t *out)
{
    if (!dev || !request || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->vector = -1;

    /* MSI first */
    if (request->modes & PCI_IRQ_MSI) {
        pci_msi_init(dev);
        int vector = pci_enable_msi(dev);
        if (vector >= 0) {
            register_interrupt_handler((uint16_t)vector, request->idt_handler, 0, 0x8e);
            out->vector = vector;
            out->mode   = PCI_IRQ_MSI;
            return 0;
        }
    }

    /* MSI-X next */
    if (request->modes & PCI_IRQ_MSIX) {
        if (!(request->modes & PCI_IRQ_MSI)) pci_msi_init(dev);
        if (pci_enable_msix(dev, 1) == 1) {
            /* Device-specific MSI-X programming (e.g. virtio queue vectors) */
            if (request->msix_setup && request->msix_setup(dev, request->msix_context) < 0) {
                pci_disable_msix(dev);
                return -ENODEV;
            }
            int vector = pci_irq_vector(dev, 0);
            if (vector >= 0) {
                register_interrupt_handler((uint16_t)vector, request->idt_handler, 0, 0x8e);
                out->vector = vector;
                out->mode   = PCI_IRQ_MSIX;
                return 0;
            }
            pci_disable_msix(dev);
        }
    }

    /* Legacy INTx fallback */
    if (request->modes & PCI_IRQ_LEGACY) {
        uint8_t irq = (uint8_t)pci_get_irq(dev);
        if (irq == 0 || irq == 0xff) return -ENODEV;
        out->irq = irq;

        /* Register the IDT handler at legacy_base + irq */
        int vector = request->legacy_base + irq;
        register_interrupt_handler((uint16_t)vector, request->idt_handler, 0, 0x8e);
        if (request->legacy_ioapic) {
            ioapic_routing_t routing = {(uint8_t)vector, irq};
            ioapic_add(&routing);
        }
        out->vector        = vector;
        out->mode          = PCI_IRQ_LEGACY;
        out->legacy_ioapic = request->legacy_ioapic ? 1 : 0;
        return 0;
    }

    return -ENODEV;
}

/* Tear down the interrupt requested by pci_request_irq() */
void pci_free_irq(pci_device_cache_t *dev, pci_irq_state_t *state)
{
    if (!dev || !state) return;

    switch (state->mode) {
        case PCI_IRQ_MSI :
            unregister_interrupt_handler((uint16_t)state->vector);
            pci_disable_msi(dev);
            break;
        case PCI_IRQ_MSIX :
            unregister_interrupt_handler((uint16_t)state->vector);
            pci_disable_msix(dev);
            break;
        case PCI_IRQ_LEGACY :
            unregister_interrupt_handler((uint16_t)state->vector);
            if (state->legacy_ioapic) {
                ioapic_routing_t routing = {(uint8_t)state->vector, state->irq};
                ioapic_remove(&routing);
            }
            break;
        default :
            break;
    }
    state->vector = -1;
    state->mode   = 0;
}

/* Find devices by class code */
static pci_finding_response_iter_t pci_class_finding(pci_device_cache_t *start, pci_finding_request_t *req)
{
    pci_class_request_t         class_req  = req->req.class_req;
    pci_device_cache_t         *cache      = pci_found_class_cache(start, class_req);
    pci_device_reg_t            reg_vendor = {cache, PCI_CONF_VENDOR};
    pci_finding_response_iter_t response   = {0};

    response.device = 0;
    response.error  = PCI_FINDING_NOT_FOUND;

    /* Test existence of device */
    if (cache && read_pci(reg_vendor) != 0xffffffff) {
        response.device = cache;
        response.error  = PCI_FINDING_SUCCESS;
    }
    return response;
}

/* Accurately search based on device information */
static pci_finding_response_iter_t pci_device_finding(pci_device_cache_t *start, pci_finding_request_t *req)
{
    pci_device_request_t        device_req = req->req.device_req;
    pci_device_cache_t         *cache      = pci_found_device_cache(start, device_req);
    pci_device_reg_t            reg_vendor = {cache, PCI_CONF_VENDOR};
    pci_finding_response_iter_t response   = {0};

    response.device = 0;
    response.error  = PCI_FINDING_NOT_FOUND;

    /* Test existence of device */
    if (cache && read_pci(reg_vendor) != 0xffffffff) {
        response.device = cache;
        response.error  = PCI_FINDING_SUCCESS;
    }
    return response;
}

/* Find a PCI device matching the request. Note: req must persist (global). */
void pci_device_find(pci_finding_request_t *req)
{
    pci_finding_response_iter_t *response = malloc(sizeof(pci_finding_response_iter_t));
    if (!response) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("pci: failed to allocate finding response.\n");
        req->response = NULL;
        return;
    }

    req->response = response;

    /* Process the request */
    switch (req->type) {
        case PCI_FOUND_CLASS :
            *req->response = pci_class_finding(0, req);
            break;
        case PCI_FOUND_DEVICE :
            *req->response = pci_device_finding(0, req);
            break;
        default :
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("pci: Unknown finding type %d\n", req->type);
            req->response->device = 0;
            req->response->error  = PCI_FINDING_ERROR;
            break;
    }

    /*
     * Response contract: error holds the outcome. On PCI_FINDING_SUCCESS,
     * device points at the matched cache; on PCI_FINDING_NOT_FOUND the
     * caller may flush the device cache and retry the find.
     */
}

/* Returns the device name based on the class code */
const char *pci_classname(uint32_t classcode)
{
    for (size_t i = 0; pci_classnames[i].name != 0; i++) {
        if (pci_classnames[i].classcode == classcode) return pci_classnames[i].name;
        if (pci_classnames[i].classcode == (classcode & 0xffff00)) return pci_classnames[i].name;
    }
    return "Unknown device";
}

/* Returns a chached PCI devices table */
pci_devices_cache_t *pci_get_devices_cache(void)
{
    return &pci_cache;
}

/* Free the early-discovery cache before rebuilding it. */
static void pci_free_devices_cache(void)
{
    pci_device_cache_t *cache = pci_cache.head;
    pci_device_cache_t *free_ptr;

    while (cache) {
        free_ptr = cache;
        cache    = cache->next;
        free(free_ptr->device);
        free(free_ptr);
    }

    pci_cache.head          = 0;
    pci_cache.devices_count = 0;
}

/* A helper function to add device cache */
static void pci_add_device_cache(pci_device_cache_t *cache)
{
    pci_device_cache_t *cpy_cache = (pci_device_cache_t *)malloc(sizeof(pci_device_cache_t));
    if (!cpy_cache) {
        plogk("pci: failed to allocate device cache copy.\n");
        return;
    }
    *cpy_cache = *cache;

    pci_device_t *cpy_device = (pci_device_t *)malloc(sizeof(pci_device_t));
    if (!cpy_device) {
        plogk("pci: failed to allocate device copy.\n");
        free(cpy_cache);
        return;
    }
    *cpy_device       = *(cache->device);
    cpy_cache->device = cpy_device;
    cpy_cache->next   = pci_cache.head;
    pci_cache.head    = cpy_cache;
    pci_cache.devices_count++;
}

/* A helper function to read registers and add device cache */
static int pci_cache_process(pci_device_cache_t *cache)
{
    /* Check device existance */
    if (mcfg_info.enabled && cache->entry) {
        pci_device_reg_t ecam_area = {cache, 0};
        cache->ecam_ptr            = mcfg_ecam_addr(cache->entry, ecam_area);
    }
    pci_device_reg_t vendor_id = {cache, PCI_CONF_VENDOR};
    cache->vendor_id           = read_pci(vendor_id);

    /* Device not exist, return 0 */
    if (cache->vendor_id == 0xffffffff) return 0;
    cache->device_id = (cache->vendor_id >> 16) & 0xffff;
    cache->vendor_id &= 0xffff;
    pci_device_reg_t revision_reg = {cache, PCI_CONF_REVISION};
    cache->class_code             = read_pci(revision_reg) >> 8;
    pci_device_reg_t header       = {cache, PCI_CONF_HEADER_TYPE};
    cache->header_type            = read_pci(header) & 0xff;

    /* Initialize MSI/MSI-X state */
    memset(&cache->msi, 0, sizeof(cache->msi));
    pci_msi_init(cache);
    pci_add_device_cache(cache);

    return 1;
}

/* Scan the secondary bus behind a PCI-to-PCI bridge. */
static void pci_scan_bridge_children(pci_device_cache_t *cache, uint16_t end_bus)
{
    uint32_t class = cache->class_code & 0xffff00;
    if (class != 0x060400 && class != 0x060900) return;

    pci_device_reg_t secondary_bus_reg = {cache, 0x19};
    uint16_t         secondary_bus     = read_pci(secondary_bus_reg) & 0xff;
    if (!secondary_bus || secondary_bus > end_bus) return;

    pci_device_t   saved_device = *cache->device;
    volatile void *saved_ecam   = cache->ecam_ptr;
    pci_scan_bus(cache, secondary_bus, end_bus);
    *cache->device  = saved_device;
    cache->ecam_ptr = saved_ecam;
}

/* Process slots of PCI devices */
static void slot_process(pci_device_cache_t *cache, uint16_t end_bus)
{
    pci_device_t *device = cache->device;
    device->func         = 0;

    if (!pci_cache_process(cache)) return; // Device not exist
    pci_scan_bridge_children(cache, end_bus);

    /* Check if device is a multifunction device */
    if (!(cache->header_type & 0x80)) return; // Not a multifunction device

    /* Process func=1..7 */
    for (device->func = 1; device->func < 8; device->func++)
        if (pci_cache_process(cache)) pci_scan_bridge_children(cache, end_bus);
}

/* Scan one bus and recursively scan bridge secondary buses */
static void pci_scan_bus(pci_device_cache_t *cache, uint16_t bus, uint16_t end_bus)
{
    if (mcfg_info.enabled && !cache->entry) return; // Enabled MCFG but no entry found
    if (bus > end_bus || bus > 255 || pci_scanned_buses[bus]) return;

    pci_scanned_buses[bus] = 1;
    pci_device_t *device   = cache->device;
    device->bus            = bus;

    for (uint16_t slot = 0; slot < 32; slot++) {
        device->bus  = bus;
        device->slot = slot;
        slot_process(cache, end_bus);
    }
}

/* Build the PCI cache during early boot.  Cache entries have stable addresses after pci_init(): drivers and PCI sysfs deliberately retain their pointers. */
static void pci_flush_devices_cache(void)
{
    pci_free_devices_cache();
    memset(pci_scanned_buses, 0, sizeof(pci_scanned_buses));
    pci_device_t       curr_device = {0, 0, 0, 0};
    pci_device_cache_t curr_cache;
    memset(&curr_cache, 0, sizeof(curr_cache));
    curr_cache.device = &curr_device;

    if (!mcfg_info.enabled) {
        curr_device.domain = 0;
        pci_scan_bus(&curr_cache, 0, 255);
    } else {
        for (size_t i = 0; i < mcfg_info.count; i++) {
            mcfg_entry_t *entry = &mcfg_info.mcfg->entries[i];
            memset(pci_scanned_buses, 0, sizeof(pci_scanned_buses));
            curr_cache.entry   = entry;
            curr_device.domain = entry->segment;
            pci_scan_bus(&curr_cache, entry->start_bus, entry->end_bus);
        }
    }
}

/* Found PCI devices cache by vender ID and device ID */
pci_device_cache_t *pci_found_device_cache(pci_device_cache_t *start, pci_device_request_t device_req)
{
    uint32_t            vendor_id = device_req.vendor_id;
    uint32_t            device_id = device_req.device_id;
    pci_device_cache_t *cache     = start ? start : pci_cache.head;

    while (cache != 0) {
        if (cache->vendor_id == vendor_id && cache->device_id == device_id) return cache;
        cache = cache->next;
    }
    return 0;
}

/* Found PCI devices cache by class code */
pci_device_cache_t *pci_found_class_cache(pci_device_cache_t *start, pci_class_request_t class_req)
{
    uint32_t            class_code = class_req.class_code;
    pci_device_cache_t *cache      = start ? start : pci_cache.head;

    while (cache != 0) {
        if (cache->class_code == class_code || (cache->class_code & 0xffff00) == class_code) return cache;
        cache = cache->next;
    }
    return 0;
}

/* PCI device initialization */
void pci_init(void)
{
    pci_flush_devices_cache();
    pci_device_cache_t *cache  = pci_cache.head;
    pci_device_t       *device = 0;

    if (!mcfg_info.enabled) {
        plogk("pci: Using legacy PCI mode.\n");
    } else {
        plogk("pci: Using MCFG PCI mode.\n");
    }
    while (cache != 0) {
        device              = cache->device;
        const char *msi_str = "";

        if (cache->msi.msi_cap && cache->msi.msix_cap) {
            msi_str = " [MSI+MSI-X]";
        } else if (cache->msi.msi_cap) {
            msi_str = " [MSI]";
        } else if (cache->msi.msix_cap) {
            msi_str = " [MSI-X]";
        }

        plogk("pci: %04x:%02x:%02x.%01x: [0x%04x:0x%04x] class=0x%06x, %s%s\n", device->domain, device->bus, device->slot, device->func, cache->vendor_id, cache->device_id, cache->class_code,
              pci_classname(cache->class_code), msi_str);
        cache = cache->next;
    }
    plogk("pci: Found %zu devices.\n", pci_cache.devices_count);
}
