/*
 *
 *      ahci.c
 *      AHCI SATA controller driver
 *
 *      2026/7/23 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <drivers/ahci/ahci.h>
#include <drivers/pci.h>
#include <kernel/debug.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/hhdm.h>

/* PCI finding request for AHCI controller (class code 0x010601) */
static pci_finding_request_t ahci_pci_request = {
    .type = PCI_FOUND_CLASS,
    .req  = {
        .class_req = {
            .class_code = 0x010601,
        },
    },
};

/* Global device tables */
ahci_device_t ahci_devices[AHCI_MAX_DEVICES];
int           ahci_device_count = 0;

/* HBA memory base */
volatile uint8_t *hba_mmio = 0;

/* Per-port state */
#define CFL_DWORDS 5

ahci_port_state_t ahci_ports[AHCI_MAX_PORTS];
static int        ahci_port_count = 0;

/* ─── MMIO helpers ─── */

uint32_t ahci_read32(volatile uint8_t *base, uint32_t reg)
{
    return mmio_read32((void *)(base + reg));
}

void ahci_write32(volatile uint8_t *base, uint32_t reg, uint32_t val)
{
    mmio_write32((uint32_t *)(base + reg), val);
}

/* ─── Slot finder ─── */

static int ahci_find_slot(ahci_port_state_t *port)
{
    uint32_t slots = ((ahci_read32(hba_mmio, HOST_CAP) >> 8) & 0x1F) + 1;
    uint32_t ci    = ahci_read32(port->port_mmio, PORT_CI);
    uint32_t sact  = ahci_read32(port->port_mmio, PORT_SACT);
    for (uint32_t i = 0; i < slots; i++) {
        if (!((ci | sact) & (1u << i))) return (int)i;
    }
    return -1;
}

/* ─── Port start / stop ─── */

static void ahci_port_stop(ahci_port_state_t *port)
{
    int               tout;
    volatile uint8_t *p = port->port_mmio;

    ahci_write32(p, PORT_CMD, ahci_read32(p, PORT_CMD) & ~PORT_CMD_ST);
    tout = 500000;
    while (ahci_read32(p, PORT_CMD) & PORT_CMD_CR) {
        if (--tout <= 0) break;
    }

    ahci_write32(p, PORT_CMD, ahci_read32(p, PORT_CMD) & ~PORT_CMD_FRE);
    tout = 500000;
    while (ahci_read32(p, PORT_CMD) & PORT_CMD_FR) {
        if (--tout <= 0) break;
    }
}

static int ahci_port_start(ahci_port_state_t *port)
{
    volatile uint8_t *p = port->port_mmio;
    int               tout;

    ahci_port_stop(port);

    ahci_write32(p, PORT_LST_ADDR, (uint32_t)(port->clb_phys & 0xFFFFFFFFULL));
    ahci_write32(p, PORT_LST_ADDR_HI, (uint32_t)(port->clb_phys >> 32));
    ahci_write32(p, PORT_FIS_ADDR, (uint32_t)(port->fb_phys & 0xFFFFFFFFULL));
    ahci_write32(p, PORT_FIS_ADDR_HI, (uint32_t)(port->fb_phys >> 32));

    ahci_write32(p, PORT_SERR, 0xFFFFFFFF);

    ahci_write32(p, PORT_IRQ_STAT, 0xFFFFFFFF);
    ahci_write32(p, PORT_IRQ_MASK, 0xFFFFFFFF);

    ahci_write32(p, PORT_CMD, ahci_read32(p, PORT_CMD) | PORT_CMD_FRE);
    tout = 500000;
    while (!(ahci_read32(p, PORT_CMD) & PORT_CMD_FR)) {
        if (--tout <= 0) return -ETIMEDOUT;
    }

    ahci_write32(p, PORT_CMD, ahci_read32(p, PORT_CMD) | PORT_CMD_ST);
    tout = 500000;
    while (!(ahci_read32(p, PORT_CMD) & PORT_CMD_CR)) {
        if (--tout <= 0) return -ETIMEDOUT;
    }

    return 0;
}

/* ─── Issue a command ─── */
#define ATA_CMD_FIS_DWORDS 5

static int ahci_issue_cmd(ahci_port_state_t *port, int slot, uint8_t *cfis, int write, uint64_t buf_phys, uint32_t byte_count)
{
    volatile hba_cmd_header_t *hdr = &port->cmd_list[slot];
    volatile uint8_t          *p   = port->port_mmio;
    int                        tout;

    hdr->cfl   = ATA_CMD_FIS_DWORDS;
    hdr->w     = write ? 1 : 0;
    hdr->prdtl = byte_count ? 1 : 0;
    hdr->p     = 1;

    memcpy((void *)port->cmd_tbl->cfis, cfis, 5 * sizeof(uint32_t));

    if (byte_count) {
        volatile hba_prdt_entry_t *prdt = &port->cmd_tbl->prdt_entry[0];
        prdt->dba                       = (uint32_t)(buf_phys & 0xFFFFFFFFULL);
        prdt->dbau                      = (uint32_t)(buf_phys >> 32);
        prdt->dbc                       = byte_count - 1;
        prdt->i                         = 1;
    }

    hdr->ctba  = (uint32_t)(port->ct_phys & 0xFFFFFFFFULL);
    hdr->ctbau = (uint32_t)(port->ct_phys >> 32);

    tout = 1000000;
    while (ahci_read32(p, PORT_TFDATA) & 0x88) {
        if (--tout <= 0) return -EBUSY;
    }

    ahci_write32(p, PORT_CI, (uint32_t)(1 << slot));

    tout = 1000000;
    while (1) {
        if (!(ahci_read32(p, PORT_CI) & (uint32_t)(1 << slot))) break;
        if (ahci_read32(p, PORT_IRQ_STAT) & PORT_IRQ_TF_ERR) {
            ahci_write32(p, PORT_IRQ_STAT, PORT_IRQ_TF_ERR);
            return -EIO;
        }
        if (--tout <= 0) return -ETIMEDOUT;
    }

    if (ahci_read32(p, PORT_TFDATA) & 0x01) return -EIO;
    return 0;
}

/* ─── Identify device (SATA only after signature check) ─── */

static int ahci_port_identify(ahci_port_state_t *port, ahci_device_t *dev)
{
    fis_reg_h2d_t cfis;
    int           slot, ret;

    memset(&cfis, 0, sizeof(cfis));
    cfis.fis_type = FIS_TYPE_REG_H2D;
    cfis.c        = 1;
    cfis.command  = ATA_CMD_IDENTIFY;
    cfis.device   = 0;

    slot = ahci_find_slot(port);
    if (slot < 0) return -EBUSY;

    memset(port->dma_buf, 0, 512);

    ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 0, port->dma_buf_phys, 512);
    if (ret != 0) return ret;

    uint16_t *buf = (uint16_t *)port->dma_buf;

    if (buf[0] == 0x0000 || buf[0] == 0xFFFF) return -ENODEV;

    dev->reserved    = 1;
    dev->type        = AHCI_DEV_SATA;
    dev->sector_size = 512;

    uint16_t *ident = (uint16_t *)port->dma_buf;
    uint32_t  cmds  = (uint32_t)ident[82] | ((uint32_t)ident[83] << 16);
    if (cmds & (1u << 26))
        dev->size = (uint32_t)ident[100] | ((uint32_t)ident[101] << 16);
    else
        dev->size = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);

    for (int k = 0; k < 40; k += 2) {
        dev->model[k]     = port->dma_buf[ATA_IDENT_MODEL + k + 1];
        dev->model[k + 1] = port->dma_buf[ATA_IDENT_MODEL + k];
    }
    dev->model[40] = 0;

    for (int k = 39; k > 0; k--) {
        if (dev->model[k] == ' ')
            dev->model[k] = '\0';
        else
            break;
    }

    return 0;
}

/* ─── SATA read/write ─── */

#define SATA_DMA_BUF_PAGES   8
#define SATA_DMA_MAX_SECTORS (SATA_DMA_BUF_PAGES * 4096 / 512)

int ahci_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, void *buffer)
{
    if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved) return -ENODEV;
    if (ahci_devices[drive].type != AHCI_DEV_SATA) return -ENOSYS;

    uint8_t            port_idx = ahci_devices[drive].port;
    ahci_port_state_t *port     = &ahci_ports[port_idx];

    uint8_t *out  = (uint8_t *)buffer;
    uint8_t  left = numsects;

    while (left) {
        uint8_t  chunk = (left > SATA_DMA_MAX_SECTORS) ? SATA_DMA_MAX_SECTORS : left;
        uint32_t bytes = (uint32_t)chunk * 512;

        fis_reg_h2d_t cfis;
        memset(&cfis, 0, sizeof(cfis));
        cfis.fis_type = FIS_TYPE_REG_H2D;
        cfis.c        = 1;
        cfis.command  = ATA_CMD_READ_DMA_EXT;
        cfis.device   = 1 << 6;
        cfis.lba0     = (uint8_t)(lba & 0xFF);
        cfis.lba1     = (uint8_t)((lba >> 8) & 0xFF);
        cfis.lba2     = (uint8_t)((lba >> 16) & 0xFF);
        cfis.lba3     = (uint8_t)((lba >> 24) & 0xFF);
        cfis.countl   = chunk;

        int slot = ahci_find_slot(port);
        if (slot < 0) return -EBUSY;

        int ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 0, port->dma_buf_phys, bytes);
        if (ret != 0) return ret;

        memcpy(out, port->dma_buf, bytes);
        out += bytes;
        lba += chunk;
        left -= chunk;
    }

    return 0;
}

int ahci_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, const void *buffer)
{
    if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved) return -ENODEV;
    if (ahci_devices[drive].type != AHCI_DEV_SATA) return -ENOSYS;

    uint8_t            port_idx = ahci_devices[drive].port;
    ahci_port_state_t *port     = &ahci_ports[port_idx];

    const uint8_t *in   = (const uint8_t *)buffer;
    uint8_t        left = numsects;

    while (left) {
        uint8_t  chunk = (left > SATA_DMA_MAX_SECTORS) ? SATA_DMA_MAX_SECTORS : left;
        uint32_t bytes = (uint32_t)chunk * 512;

        memcpy(port->dma_buf, in, bytes);

        fis_reg_h2d_t cfis;
        memset(&cfis, 0, sizeof(cfis));
        cfis.fis_type = FIS_TYPE_REG_H2D;
        cfis.c        = 1;
        cfis.command  = ATA_CMD_WRITE_DMA_EXT;
        cfis.device   = 1 << 6;
        cfis.lba0     = (uint8_t)(lba & 0xFF);
        cfis.lba1     = (uint8_t)((lba >> 8) & 0xFF);
        cfis.lba2     = (uint8_t)((lba >> 16) & 0xFF);
        cfis.lba3     = (uint8_t)((lba >> 24) & 0xFF);
        cfis.countl   = chunk;

        int slot = ahci_find_slot(port);
        if (slot < 0) return -EBUSY;

        int ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 1, port->dma_buf_phys, bytes);
        if (ret != 0) return ret;

        in += bytes;
        lba += chunk;
        left -= chunk;
    }

    return 0;
}

/* ─── AHCI initialization ─── */

void init_ahci(void)
{
    pci_device_find(&ahci_pci_request);
    if (ahci_pci_request.response->error != PCI_FINDING_SUCCESS) {
        plogk("ahci: No AHCI controller found.\n");
        return;
    }

    pci_device_cache_t *cache = ahci_pci_request.response->device;

    base_address_register_t bar = get_base_address_register(cache, 5);
    if (bar.type != mem_mapping) {
        plogk("ahci: BAR5 is not a memory BAR.\n");
        return;
    }

    hba_mmio = (volatile uint8_t *)bar.address;

    /* BIOS/OS handoff */
    uint32_t cap2 = ahci_read32(hba_mmio, HOST_CAP2);
    if (cap2 & (1u << 0)) {
        uint32_t bohc = ahci_read32(hba_mmio, HOST_BOHC);
        if (bohc & (1u << 4)) {
            if (!(bohc & (1u << 1))) {
                ahci_write32(hba_mmio, HOST_BOHC, bohc | (1u << 1));
                int boh_timeout = 1000000;
                while ((ahci_read32(hba_mmio, HOST_BOHC) & (1u << 4)) && !(ahci_read32(hba_mmio, HOST_BOHC) & (1u << 1))) {
                    if (--boh_timeout <= 0) break;
                }
            }
        }
    }

    /* HBA reset */
    ahci_write32(hba_mmio, HOST_CTL, ahci_read32(hba_mmio, HOST_CTL) & ~HOST_AHCI_EN);

    ahci_write32(hba_mmio, HOST_CTL, ahci_read32(hba_mmio, HOST_CTL) | HOST_RESET);
    int reset_timeout = 1000000;
    while (ahci_read32(hba_mmio, HOST_CTL) & HOST_RESET) {
        if (--reset_timeout <= 0) {
            plogk("ahci: HBA reset timeout.\n");
            return;
        }
    }

    ahci_write32(hba_mmio, HOST_CTL, ahci_read32(hba_mmio, HOST_CTL) | HOST_AHCI_EN);
    int ae_timeout = 1000000;
    while (!(ahci_read32(hba_mmio, HOST_CTL) & HOST_AHCI_EN)) {
        if (--ae_timeout <= 0) {
            plogk("ahci: AHCI enable timeout.\n");
            return;
        }
    }

    uint32_t pi        = ahci_read32(hba_mmio, HOST_PORTS_IMPL);
    uint32_t cap       = ahci_read32(hba_mmio, HOST_CAP);
    uint32_t max_ports = (cap & 0x1F) + 1;
    if (max_ports > AHCI_MAX_PORTS) max_ports = AHCI_MAX_PORTS;

    ahci_port_count   = 0;
    ahci_device_count = 0;

    for (uint32_t i = 0; i < max_ports; i++) {
        if (!(pi & (1u << i))) continue;

        ahci_port_state_t *port = &ahci_ports[ahci_port_count];
        memset(port, 0, sizeof(*port));
        port->port_mmio = hba_mmio + 0x100 + i * 0x80;
        port->port_no   = (uint8_t)i;

        /* Allocate per-port memory */
        port->clb_phys = alloc_frames(1);
        port->fb_phys  = alloc_frames(1);
        port->ct_phys  = alloc_frames(1);

        port->cmd_list = (hba_cmd_header_t *)phys_to_virt(port->clb_phys);
        port->fis      = (hba_fis_t *)phys_to_virt(port->fb_phys);
        port->cmd_tbl  = (hba_cmd_tbl_t *)phys_to_virt(port->ct_phys);

        memset(port->cmd_list, 0, 0x400);
        memset((void *)port->fis, 0, 0x100);
        memset((void *)port->cmd_tbl, 0, sizeof(hba_cmd_tbl_t));

        /* Per-port DMA buffer */
        port->dma_buf_phys = alloc_frames(SATA_DMA_BUF_PAGES);
        if (!port->dma_buf_phys) {
            plogk("ahci: Port %u DMA buffer phys alloc failed.\n", i);
            continue;
        }
        port->dma_buf = (uint8_t *)phys_to_virt(port->dma_buf_phys);
        if (!port->dma_buf) {
            plogk("ahci: Port %u DMA buffer virt alloc failed.\n", i);
            continue;
        }

        if (ahci_port_start(port) != 0) {
            plogk("ahci: Port %u start failed.\n", i);
            continue;
        }

        /* Spin up */
        ahci_write32(port->port_mmio, PORT_SCTL, (ahci_read32(port->port_mmio, PORT_SCTL) & ~0xFu) | 0x1);

        {
            uint32_t ssts;
            int      det_timeout = 1000000;
            while (1) {
                ssts = ahci_read32(port->port_mmio, PORT_SSTS);
                if ((ssts & 0xF) == HBA_PORT_DET_PRESENT) break;
                if ((ssts & 0xF) == 0) break;
                if (--det_timeout <= 0) break;
            }
            if ((ssts & 0xF) != HBA_PORT_DET_PRESENT) {
                plogk("ahci: Port %u device detect failed (DET=%u).\n", i, ssts & 0xF);
                ahci_port_stop(port);
                continue;
            }
        }

        uint32_t sig = ahci_read32(port->port_mmio, PORT_SIG);
        if (sig != SATA_SIG_ATA && sig != SATA_SIG_ATAPI) {
            plogk("ahci: Port %u no device (sig=0x%x).\n", i, sig);
            ahci_port_stop(port);
            continue;
        }

        if (sig == SATA_SIG_ATAPI) {
            /* SATAPI device - handled by satapi.c, just register in ahci device table */
            ahci_device_t *dev = &ahci_devices[ahci_device_count];
            memset(dev, 0, sizeof(*dev));
            dev->port        = (uint8_t)ahci_port_count;
            dev->reserved    = 1;
            dev->type        = AHCI_DEV_SATAPI;
            dev->sector_size = 2048;
            dev->size        = 0;
            plogk("ahci: Found SATAPI device on port %u\n", i);
            ahci_device_count++;
            ahci_port_count++;
            continue;
        }

        /* SATA device: do IDENTIFY */
        ahci_device_t *dev = &ahci_devices[ahci_device_count];
        memset(dev, 0, sizeof(*dev));
        dev->port = (uint8_t)ahci_port_count;

        if (ahci_port_identify(port, dev) != 0) {
            plogk("ahci: Port %u identify failed.\n", i);
            ahci_port_stop(port);
            continue;
        }

        ahci_device_count++;
        ahci_port_count++;

        plogk("ahci: Found SATA Drive %u (KiB) on port %u - %s\n", (dev->size * 512) / 1024, i, dev->model);
    }

    if (ahci_device_count == 0) {
        plogk("ahci: No AHCI devices found.\n");
    } else {
        plogk("ahci: %u device(s) initialized.\n", ahci_device_count);
    }
}
