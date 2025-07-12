/*
 *
 *      pci.c
 *      Peripheral Component Interconnect Standard Driver
 *
 *      2025/3/9 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "pci.h"
#include "acpi.h"
#include "common.h"
#include "debug.h"
#include "hhdm.h"
#include "printk.h"
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"

MCFGInfo mcfg_info;

pci_devices_cache pci_cache = {
    .head          = 0,
    .devices_count = 0,
};

static void slot_process_legacy(pci_device_cache *device);
static uint32_t pci_legacy_read(pci_device_reg reg);
static void pci_legacy_write(pci_device_reg reg, uint32_t value);

static void slot_process_mcfg(pci_device_cache *device);
static uint32_t pci_mcfg_read(pci_device_reg reg);
static void pci_mcfg_write(pci_device_reg reg, uint32_t value);

/* PCI operations (For MCFG and legacy mode) */
struct PCIOps {
        void (*slot_process)(pci_device_cache *device);
        uint32_t (*read)(pci_device_reg reg);
        void (*write)(pci_device_reg reg, uint32_t value);
} pci_ops = {
    .slot_process = slot_process_legacy,
    .read         = pci_legacy_read,
    .write        = pci_legacy_write,
};

/* PCI usable list */
pci_usable_list pci_usable = {
    .head  = 0,
    .count = 0,
};

struct {
        uint32_t classcode;
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

    {0x0d0000, "iRDA Compatible Controlle"                  },
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

    {0x100000, "Network and Computing Encrpytion/Decryption"},
    {0x101000, "Entertainment Encryption/Decryption"        },
    {0x108000, "Other Encryption Controller"                },

    {0x110000, "DPIO Modules"                               },
    {0x110100, "Performance Counters"                       },
    {0x111000, "Communication Synchronizer"                 },
    {0x112000, "Signal Processing Management"               },
    {0x118000, "Other Signal Processing Controller"         },
    {0x000000, 0                                            },
};

/* MCFG initialization */
void mcfg_init(void *mcfg)
{
    if (mcfg) {
        struct MCFG *inner = (struct MCFG *)mcfg;
        mcfg_info.count    = (inner->h.Length - sizeof(struct ACPISDTHeader) - 8) / sizeof(struct mcfg_entry);
        plogk("MCFG: MCFG found with %lu entries\n", mcfg_info.count);
        for (size_t i = 0; i < mcfg_info.count; i++) {
            /* Convert to the virtual address */
            inner->entries[i].base_addr = (uint64_t)phys_to_virt(inner->entries[i].base_addr);
            plogk("MCFG: mcfg->entries[%lu] base address: %p\n", i, inner->entries[i].base_addr);
            plogk("MCFG: mcfg->entries[%lu] segment: %hu\n", i, inner->entries[i].segment);
            plogk("MCFG: mcfg->entries[%lu] start bus: %hhu\n", i, inner->entries[i].start_bus);
            plogk("MCFG: mcfg->entries[%lu] end bus: %hhu\n", i, inner->entries[i].end_bus);
        }
        mcfg_info.mcfg    = inner;
        mcfg_info.enabled = 1;

        /* Set PCI operations */
        pci_ops = (struct PCIOps) {
            .read         = pci_mcfg_read,
            .write        = pci_mcfg_write,
            .slot_process = slot_process_mcfg,
        };
    } else {
        /* Never be executed, because it will be checked before this functions */
        panic("MCFG: mcfg=NULL\n");
    }
};

/* Search MCFG entry by bus */
mcfg_entry *mcfg_search_entry(uint16_t bus)
{
    for (size_t i = 0; i < mcfg_info.count; i++) {
        struct mcfg_entry *entry = &mcfg_info.mcfg->entries[i];
        if (bus >= entry->start_bus && bus <= entry->end_bus) return entry;
    }
    return 0;
}

/* Get ECAM address of register */
void *mcfg_ecam_addr(mcfg_entry *entry, pci_device_reg reg)
{
    uintptr_t addr;
    pci_device *device = reg.parent->device;
    uint32_t bus       = device->bus & 0xff;
    uint32_t slot      = device->slot & 0x1f;
    uint32_t func      = device->func & 0x07;
    addr               = entry->base_addr   /* Base Address */
        + ((uint64_t)entry->segment << 32)  /* Segment */
        + (((bus - entry->start_bus) << 20) /* Bus */
           | (slot << 15)                   /* Slot */
           | (func << 12)                   /* Func */
           | (reg.offset & 0xffc));         /* Register */
    PointerCast cast;
    cast.val = addr;
    return cast.ptr;
}

/* Update ECAM addresses to cache */
pci_device_ecam mcfg_update_ecam(mcfg_entry *entry, pci_device_cache *cache)
{
    pci_device_ecam ecam;
    pci_device_reg cpy_reg = {.parent = cache};
    enum header_type type;
    cpy_reg.offset  = ECAM_AREA_ID;
    ecam.id_ecam    = mcfg_ecam_addr(entry, cpy_reg);
    cpy_reg.offset  = ECAM_AREA_OPS;
    ecam.ops_ecam   = mcfg_ecam_addr(entry, cpy_reg);
    cpy_reg.offset  = ECAM_AREA_FIELD;
    ecam.field_ecam = mcfg_ecam_addr(entry, cpy_reg);
    cpy_reg.offset  = ECAM_AREA_OPS2;
    ecam.ops2_ecam  = mcfg_ecam_addr(entry, cpy_reg);
    cache->ecam     = ecam;

    /* Others */
    uint32_t other_reg_count = 0;
    cpy_reg.offset           = PCI_CONF_HEADER_TYPE; // Read header type
    type                     = pci_mcfg_read(cpy_reg) & PCI_HEADER_TYPE_MASK;
    // switch (type) {
    //     case HEADER_TYPE_GENERAL :
    //     case HEADER_TYPE_BRIDGE :
    //         other_reg_count = 12;
    //         break;
    //     case HEADER_TYPE_CARDBUS :
    //         other_reg_count = 14;
    //         break;
    //     default :
    //         other_reg_count = 0;
    //         break;
    // }
    // Same as:
    // x(x-1) + 12 [x <= HEADER_TYPE_CARDBUS]
    // other_reg_count = type * (type - 1) + 12;
    // other_reg_count = (type <= HEADER_TYPE_CARDBUS) ? other_reg_count : 0;
    // Also same as:
    static uint32_t reg_cnts_table[4] = {12, 12, 14, 0};
    other_reg_count                   = reg_cnts_table[type < 3 ? type : 3];

    ecam.others = (volatile void **)malloc(other_reg_count * sizeof(volatile void *));
    for (uint32_t reg_idx = 0; reg_idx < other_reg_count; reg_idx++) {
        cpy_reg.offset       = reg_idx * 4 + ECAM_OTHERS;
        ecam.others[reg_idx] = mcfg_ecam_addr(entry, cpy_reg);
    }
    cache->ecam = ecam;
    return ecam;
}

/* Reading values ​​from PCI device registers in Legacy I/O */
static uint32_t pci_legacy_read(pci_device_reg reg)
{
    pci_device *device       = reg.parent->device;
    uint32_t register_offset = reg.offset;
    uint32_t bus             = device->bus & 0xff;
    uint32_t slot            = device->slot & 0x1f;
    uint32_t func            = device->func & 0x07;

    uint32_t id = 1 << 31 | (bus << 16) | (slot << 11) | (func << 8) | (register_offset & 0xfc);
    outl(PCI_COMMAND_PORT, id);
    uint32_t result = inl(PCI_DATA_PORT);
    return result >> (8 * (register_offset % 4));
}

/* Write values ​​to PCI device registers in Legacy I/O */
static void pci_legacy_write(pci_device_reg reg, uint32_t value)
{
    pci_device *device       = reg.parent->device;
    uint32_t register_offset = reg.offset;
    uint32_t bus             = device->bus & 0xff;
    uint32_t slot            = device->slot & 0x1f;
    uint32_t func            = device->func & 0x07;

    uint32_t id = 1 << 31 | (bus << 16) | (slot & 0x1f << 11) | (func << 8) | (register_offset & 0xfc);
    outl(PCI_COMMAND_PORT, id);
    outl(PCI_DATA_PORT, value);
}

/* Write values ​​to PCI device registers from `pci_device_ecam` */
static void pci_mcfg_write(pci_device_reg reg, uint32_t value)
{
    enum ecam_area area  = reg.offset / 4 * 4; // floor_4(reg.offset): A field is 4 bytes
    uint32_t offset      = reg.offset - area;  // = reg.offset % 4
    pci_device_ecam ecam = reg.parent->ecam;
    volatile uint32_t *ptr;
    switch (area) {
        case ECAM_AREA_ID :
            ptr = ecam.id_ecam;
            break;
        case ECAM_AREA_OPS :
            ptr = ecam.ops_ecam;
            break;
        case ECAM_AREA_FIELD :
            ptr = ecam.field_ecam;
            break;
        case ECAM_AREA_OPS2 :
            ptr = ecam.ops2_ecam;
            break;
        default :
            ptr = ecam.others[(area - ECAM_OTHERS) / 4];
            break;
    }
    uint32_t shift = 8 * offset;
    if (shift == 0) { // Aligned written
        *ptr = value;
    } else { // No aligned written
        uint32_t mask = 0xffffffff >> (32 - shift);
        *ptr          = (*ptr & ~mask) | (value << shift);

        /* Step1. A:=(*ptr & ~mask) -> Reset the bits of target register
         *        Example: 0x12345678 & ~0xff000000 = 0x00345678
         * Step2. B:=(value << shift) -> Move value to the bits of target register
         *        Example: (0xa5 & 0xff) << 24 = 0xa5000000
         * Step3. A|B -> Merge
         *        Example: 0x00345678 | 0xa5000000 = 0xa5345678
         */
    }
}

/* Reading values ​​from PCI device registers and `pci_device_ecam` */
static uint32_t pci_mcfg_read(pci_device_reg reg)
{
    enum ecam_area area  = reg.offset / 4 * 4; // floor_4(reg.offset): A field is 4 bytes
    uint32_t offset      = reg.offset - area;  // = reg.offset % 4
    pci_device_ecam ecam = reg.parent->ecam;
    volatile uint32_t *ptr;
    switch (area) {
        case ECAM_AREA_ID :
            ptr = ecam.id_ecam;
            break;
        case ECAM_AREA_OPS :
            ptr = ecam.ops_ecam;
            break;
        case ECAM_AREA_FIELD :
            ptr = ecam.field_ecam;
            break;
        case ECAM_AREA_OPS2 :
            ptr = ecam.ops2_ecam;
            break;
        default :
            ptr = ecam.others[(area - ECAM_OTHERS) / 4];
            break;
    }
    return *ptr >> (8 * offset);
}

/* Reading values ​​from PCI device registers */
uint32_t read_pci(pci_device_reg reg)
{
    return pci_ops.read(reg);
}

/* Write values ​​to PCI device registers */
void write_pci(pci_device_reg reg, uint32_t value)
{
    return pci_ops.write(reg, value);
}

/* Read the value from the PCI device command status register */
uint32_t pci_read_command_status(pci_device_cache *device)
{
    pci_device_reg reg = {device, ECAM_AREA_OPS};
    return read_pci(reg);
}

/* Write a value to the PCI device command status register */
void pci_write_command_status(pci_device_cache *device, uint32_t value)
{
    pci_device_reg reg = (pci_device_reg) {device, ECAM_AREA_OPS};
    write_pci(reg, value);
}

/* Get detailed information about the base address register */
base_address_register get_base_address_register(pci_device_cache *device, uint32_t bar)
{
    /* Here is some notice when you are using this function:
     * 1. The `bar` is the index of BAR, not the *register_offset* of BAR.
     * 2. When you are reading a mem_mapping BAR, you should notice the `size` of BAR.
     *    It's useful, when you are doing something *special*.
    */
    base_address_register result = {0};
    pci_device_reg reg           = {device, PCI_CONF_HEADER_TYPE};

    uint32_t headertype = read_pci(reg) & 0x7e;
    uint32_t max_bars;
    // switch (headertype) {
    //     case HEADER_TYPE_GENERAL :
    //         max_bars = 6;
    //         break;
    //     case HEADER_TYPE_BRIDGE :
    //         max_bars = 2;
    //         break;
    //     case HEADER_TYPE_CARDBUS :
    //     default :
    //         max_bars = 0;
    //         break;
    // }
    // Same as:
    // x(x-5) + 6 [x<=HEADER_TYPE_CARDBUS]
    // max_bars = headertype * (headertype - 5) + 6;
    // max_bars = headertype <= HEADER_TYPE_CARDBUS ? max_bars : 0;
    // Also same as:
    static uint32_t max_bars_table[3] = {6, 2, 0};
    max_bars                          = max_bars_table[headertype < 2 ? headertype : 2];
    if (bar >= max_bars) return result;

    reg.offset         = 0x10 + 4 * bar;
    uint64_t bar_value = read_pci(reg);
    result.type        = (bar_value & 1) ? input_output : mem_mapping;

    switch (result.type) {
        case mem_mapping : // Memory
            // Match the BAR type bits
            result.size = (bar_value >> 1) & 0b11;
            switch (result.size) {
                case BAR_S32 :      // 32
                case BAR_Reserved : // Reserved (Un-processed)
                    break;
                case BAR_S64 : // 64
                    // Read the next BAR for 64-bit BARs
                    if (bar + 1 < max_bars) {
                        reg.offset = 0x10 + 4 * (bar + 1);
                        bar_value |= (uint64_t)read_pci(reg) << 32; // Read the next BAR and merge
                    } else {
                        plogk("PCI: 64-bit BAR without more BARs.\n");
                    }
                    break;
                default :
                    plogk("PCI: Unknown BAR type.\n");
                    break;
            }
            // We transform the address to a virtual address,
            // it removes the *higher* bits of the address.
            result.address      = (uint32_t *)phys_to_virt(bar_value & ~0b1111);
            result.prefetchable = bar_value & 0b1000;
            break;
        case input_output : // I/O
            result.address      = (uint32_t *)phys_to_virt(bar_value & ~0b11);
            result.prefetchable = 0;
            break;
        default :
            plogk("PCI: Runtime Error at %s:%d.\n", __FILE__, __LINE__);
            break;
    }
    return result;
}

/* Get the I/O port base address of the PCI device */
uint32_t pci_get_port_base(pci_device_cache *device)
{
    uint32_t io_port = 0;
    for (int i = 0; i < 6; i++) {
        base_address_register bar = get_base_address_register(device, i);
        if (bar.type == input_output) io_port = (uintptr_t)bar.address;
    }
    return io_port;
}

/* Read the value of the nth base address register */
uint32_t read_bar_n(pci_device_cache *device, uint32_t bar_n)
{
    pci_device_reg reg = {device, 0x10 + 4 * bar_n};
    return read_pci(reg);
}

/* Get the interrupt number of the PCI device */
uint32_t pci_get_irq(pci_device_cache *device)
{
    pci_device_reg reg = {device, 0x3c};
    return read_pci(reg);
}

/* Configuring PCI Devices */
void pci_config(pci_device_cache *cache, uint32_t addr)
{
    pci_device *device = cache->device;
    uint32_t cmd       = 0;
    cmd                = 0x80000000 + addr + (device->func << 8) + (device->slot << 11) + (device->bus << 16);
    outl(PCI_COMMAND_PORT, cmd);
}

/* Find devices by class code */
static void pci_class_finding(pci_finding_request *req)
{
    pci_class_request class_req = req->req.class_req;
    pci_device_cache *cache     = pci_found_class_cache(class_req);
    pci_device_reg reg_vendor   = {cache, PCI_CONF_VENDOR};

    req->response->device = 0;
    req->response->error  = PCI_FINDING_NOT_FOUND;
    if (cache != 0) {
        /* Test existence of device */
        if (read_pci(reg_vendor) != 0xffffffff) {
            req->response->device = cache;
            req->response->error  = PCI_FINDING_SUCCESS;
        }
    }
}

/* Accurately search based on device information */
static void pci_device_finding(pci_finding_request *req)
{
    pci_device_request device_req = req->req.device_req;
    pci_device_cache *cache       = pci_found_device_cache(device_req);
    pci_device_reg reg_vendor     = {cache, PCI_CONF_VENDOR};

    req->response->device = 0;
    req->response->error  = PCI_FINDING_NOT_FOUND;
    if (cache != 0) {
        /* Test existence of device */
        if (read_pci(reg_vendor) != 0xffffffff) {
            req->response->device = cache;
            req->response->error  = PCI_FINDING_SUCCESS;
        }
    }
}

/* Add the found devices to the usable list */
static void add_to_usable_list(pci_finding_request *req)
{
    pci_usable_node *node = (pci_usable_node *)malloc(sizeof(pci_usable_node));
    node->request         = req;
    node->next            = pci_usable.head;
    pci_usable.head       = node;
    pci_usable.count++;
}

/* Finding PCI devices */
void pci_device_find(pci_finding_request *req) // Notice: the req should be a global variable
{
    pci_finding_response *response = malloc(sizeof(pci_finding_response));
    req->response                  = response;

    /* Add to usable list */
    add_to_usable_list(req);

    /* Process the request */
    switch (req->type) {
        case PCI_FOUND_CLASS :
            pci_class_finding(req);
            break;
        case PCI_FOUND_DEVICE :
            pci_device_finding(req);
            break;
        default :
            plogk("PCI: Unknown finding type %d\n", req->type);
            req->response         = malloc(sizeof(pci_finding_response));
            req->response->device = 0;
            req->response->error  = PCI_FINDING_ERROR;
            break;
    }

    /* By the end of the function, you should to do: 
     * 1. Check the response->error
     * 2. If error is PCI_FINDING_SUCCESS, 
     *    then response->device should be set to the founded device cache.
     * 3. If error is PCI_FINDING_NOT_FOUND, 
     *    then response->device should be set to 0, and you can try to execute `pci_flush_devices_cache()` and then retry this function.
     */
}

/* Update the usable list */
void pci_update_usable_list(void)
{
    pci_usable_node *node = pci_usable.head;
    while (node != 0) {
        volatile pci_finding_response *response = node->request->response;

        /* Reset the response */
        response->device = 0;
        response->error  = PCI_FINDING_NOT_FOUND;

        /* Update the device cache */
        switch (node->request->type) {
            case PCI_FOUND_CLASS :
                pci_class_finding(node->request);
                break;
            case PCI_FOUND_DEVICE :
                pci_device_finding(node->request);
                break;
            default :
                plogk("PCI: Unknown finding type %d\n", node->request->type);
                response->device = 0;
                response->error  = PCI_FINDING_ERROR;
                break;
        }
        node = node->next;
    }
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
pci_devices_cache *pci_get_devices_cache(void)
{
    return &pci_cache;
}

/* Free the PCI devices cache */
void pci_free_devices_cache()
{
    pci_device_cache *cache = pci_cache.head;
    pci_device_cache *free_ptr;
    while (cache != 0) {
        free_ptr = cache;
        cache    = cache->next;
        free(free_ptr->device);
        free((void *)free_ptr->ecam.others);
        free(free_ptr);
    }
    pci_cache.head          = 0;
    pci_cache.devices_count = 0;
    return;
}

/* A helper function to add device cache */
static void pci_add_device_cache(pci_device_cache *cache)
{
    pci_device_cache *cpy_cache = (pci_device_cache *)malloc(sizeof(pci_device_cache));
    *cpy_cache                  = *cache;
    pci_device *cpy_device      = (pci_device *)malloc(sizeof(pci_device));
    *cpy_device                 = *(cache->device);
    cpy_cache->device           = cpy_device;
    cpy_cache->next             = pci_cache.head;
    pci_cache.head              = cpy_cache;
    pci_cache.devices_count++;
    return;
}

/* A helper function to read registers and add device cache */
static int pci_cache_process(pci_device_cache *cache)
{
    pci_device_reg reg_vendor_id = {cache, PCI_CONF_VENDOR};
    pci_device_reg reg_device_id = {cache, PCI_CONF_DEVICE};
    pci_device_reg reg_value_c   = {cache, PCI_CONF_REVISION};
    pci_device_reg reg_header    = {cache, PCI_CONF_HEADER_TYPE};

    /* Check device existance */
    cache->vendor_id = read_pci(reg_vendor_id);

    /* Device not exist, return 0 */
    if (cache->vendor_id == 0xffffffff) return 0;
    cache->vendor_id &= 0xffff;
    cache->device_id   = read_pci(reg_device_id) & 0xffff;
    cache->value_c     = read_pci(reg_value_c);
    cache->class_code  = cache->value_c >> 8;
    cache->header_type = read_pci(reg_header) & 0xff;
    pci_add_device_cache(cache);

    /* Exist and added */
    return 1;
}

/* Process slots of PCI devices in Legacy I/O */
static void slot_process_legacy(pci_device_cache *cache)
{
    pci_device *device = cache->device;

    device->func = 0;
    if (!pci_cache_process(cache)) {
        return; // Device not exist
    }

    /* Check if device is a multifunction device */
    if (!(cache->header_type & 0x80)) {
        return; // Not a multifunction device
    }

    /* Process func=1..7 */
    for (device->func = 1; device->func < 8; device->func++) pci_cache_process(cache);
    return;
}

/* Process slots of PCI devices in MCFG mode */
static void slot_process_mcfg(pci_device_cache *cache)
{
    pci_device *device = cache->device;
    mcfg_entry *entry  = mcfg_search_entry(device->bus);
    if (entry == 0) return;
    pci_device_ecam ecam = mcfg_update_ecam(entry, cache);
    cache->ecam          = ecam;

    device->func = 0;

    /* Check device existance */
    if (!pci_cache_process(cache)) {
        free((void *)ecam.others);
        return; // Device not exist
    }

    /* Check if device is a multifunction device */
    if (!(cache->header_type & 0x80)) {
        return; // Not a multifunction device
    }

    /* Process func=1..7 */
    for (device->func = 1; device->func < 8; device->func++) {
        /* Update ecam cache */
        ecam        = mcfg_update_ecam(entry, cache);
        cache->ecam = ecam;
        if (!pci_cache_process(cache)) {
            free((void *)ecam.others);
            continue; // Device not exist
        }
    }
}

/* Process slots of PCI devices */
static void slot_process(pci_device_cache *device)
{
    pci_ops.slot_process(device);
}

/* Flush the PCI devices cache */
void pci_flush_devices_cache()
{
    pci_free_devices_cache();
    pci_device curr_device      = {0, 0, 0};
    pci_device_cache curr_cache = {
        &curr_device, 0, 0, 0, 0, 0, 0, {0, 0, 0, 0, 0},
    };

    for (curr_device.bus = 0; curr_device.bus < 256; curr_device.bus++) {
        for (curr_device.slot = 0; curr_device.slot < 32; curr_device.slot++) slot_process(&curr_cache);
    }
    pci_update_usable_list();
    return;
}

/* Found PCI devices cache by vender ID and device ID */
pci_device_cache *pci_found_device_cache(pci_device_request device_req)
{
    uint32_t vendor_id      = device_req.vendor_id;
    uint32_t device_id      = device_req.device_id;
    pci_device_cache *cache = pci_cache.head;
    while (cache != 0) {
        if (cache->vendor_id == vendor_id && cache->device_id == device_id) return cache;
        cache = cache->next;
    }
    return 0;
}

/* Found PCI devices cache by class code */
pci_device_cache *pci_found_class_cache(pci_class_request class_req)
{
    uint32_t class_code     = class_req.class_code;
    pci_device_cache *cache = pci_cache.head;
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
    pci_device_cache *cache = pci_cache.head;
    pci_device *device      = 0;
    while (cache != 0) {
        device = cache->device;
        plogk("PCI: %03d:%02d.%01d: [0x%04x:0x%04x] %s\n", device->bus, device->slot, device->func, cache->vendor_id,
              cache->device_id, pci_classname(cache->class_code));
        cache = cache->next;
    }
    plogk("PCI: Found %lu devices.\n", pci_cache.devices_count);
}
