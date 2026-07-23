/*
 *
 *      satapi.c
 *      AHCI SATAPI (ATAPI over AHCI) driver
 *
 *      2026/7/23 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/ahci/ahci.h>
#include <drivers/ahci/satapi.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/hhdm.h>

#define SATAPI_CDB_LEN 16
#define CFL_DWORDS     5

ahci_satapi_device_t ahci_satapi_devices[AHCI_MAX_DEVICES];
int                  ahci_satapi_device_count = 0;

/* ─── Slot finder (same logic as ahci.c) ─── */

static int satapi_find_slot(ahci_port_state_t *port)
{
    uint32_t slots = ((ahci_read32(hba_mmio, HOST_CAP) >> 8) & 0x1F) + 1;
    uint32_t ci    = ahci_read32(port->port_mmio, PORT_CI);
    uint32_t sact  = ahci_read32(port->port_mmio, PORT_SACT);
    for (uint32_t i = 0; i < slots; i++) {
        if (!((ci | sact) & (1u << i))) return (int)i;
    }
    return -1;
}

/* ─── Issue an ATAPI packet command via AHCI ─── */

static int satapi_issue_packet(ahci_port_state_t *port, int slot, const uint8_t *cdb, uint16_t cdb_len, uint8_t direction, int is_dma,
                               uint64_t buf_phys, uint32_t byte_count)
{
    volatile hba_cmd_header_t *hdr = &port->cmd_list[slot];
    volatile uint8_t          *p   = port->port_mmio;
    int                        tout;

    (void)direction;

    memset((void *)port->cmd_tbl->acmd, 0, 16);
    memcpy((void *)port->cmd_tbl->acmd, cdb, (cdb_len < 16) ? cdb_len : 16);

    fis_reg_h2d_t cfis;
    memset(&cfis, 0, sizeof(cfis));
    cfis.fis_type = FIS_TYPE_REG_H2D;
    cfis.c        = 1;
    cfis.command  = ATA_CMD_PACKET;
    cfis.featurel = is_dma ? 1 : 0;
    cfis.device   = 0;
    memcpy((void *)port->cmd_tbl->cfis, &cfis, 5 * sizeof(uint32_t));

    hdr->cfl = CFL_DWORDS;
    hdr->a   = 1;
    /* For ATAPI, w=0 means device-to-host (read), w=1 means host-to-device (write).
     * Currently all SATAPI commands are reads; for write support this must be
     * determined from the SCSI command opcode. */
    hdr->w     = 0;
    hdr->prdtl = byte_count ? 1 : 0;
    hdr->p     = 1;

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

/* ─── Public API ─── */

void ahci_satapi_init(void)
{
    ahci_satapi_device_count = 0;

    for (int i = 0; i < AHCI_MAX_DEVICES; i++) {
        if (!ahci_devices[i].reserved || ahci_devices[i].type != AHCI_DEV_SATAPI) continue;

        uint8_t port_idx = ahci_devices[i].port;

        ahci_satapi_device_t *sdev = &ahci_satapi_devices[ahci_satapi_device_count];
        memset(sdev, 0, sizeof(*sdev));
        sdev->reserved = 1;
        sdev->port_idx = port_idx;

        uint32_t lba_sz = 0, blk_sz = 0;
        uint8_t  cap_ret = ahci_satapi_read_capacity(ahci_satapi_device_count, &lba_sz, &blk_sz);
        if (cap_ret == 0) {
            sdev->lba_size = lba_sz;
            sdev->blk_size = blk_sz;
        }

        plogk("satapi: AHCI SATAPI device %u on AHCI port %u (%u blocks, %u bytes/block)\n", ahci_satapi_device_count, port_idx, sdev->lba_size,
              sdev->blk_size);

        ahci_satapi_device_count++;
    }
}

int ahci_satapi_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, void *buffer)
{
    if (drive >= ahci_satapi_device_count || !ahci_satapi_devices[drive].reserved) return -ENODEV;

    ahci_port_state_t *port     = &ahci_ports[ahci_satapi_devices[drive].port_idx];
    uint32_t           blk_size = ahci_satapi_devices[drive].blk_size;
    if (blk_size == 0) blk_size = 2048;

    uint32_t total_bytes = (uint32_t)numsects * blk_size;
    uint16_t byte_limit  = (uint16_t)(total_bytes > 0xFFFE ? 0xFFFE : total_bytes);

    uint8_t cdb[SATAPI_CDB_LEN];
    memset(cdb, 0, sizeof(cdb));
    cdb[0]  = GPCMD_READ_12;
    cdb[2]  = (lba >> 24) & 0xFF;
    cdb[3]  = (lba >> 16) & 0xFF;
    cdb[4]  = (lba >> 8) & 0xFF;
    cdb[5]  = lba & 0xFF;
    cdb[6]  = 0;
    cdb[7]  = 0;
    cdb[8]  = (numsects >> 24) & 0xFF;
    cdb[9]  = (numsects >> 16) & 0xFF;
    cdb[10] = (numsects >> 8) & 0xFF;
    cdb[11] = numsects & 0xFF;

    int slot = satapi_find_slot(port);
    if (slot < 0) return -EBUSY;

    int ret = satapi_issue_packet(port, slot, cdb, 12, SATAPI_PROT_PIO, 0, port->dma_buf_phys, byte_limit);
    if (ret != 0) return ret;

    memcpy(buffer, port->dma_buf, total_bytes);
    return 0;
}

uint8_t ahci_satapi_send_packet(uint8_t drive, const uint8_t *cdb, uint16_t byte_limit, uint8_t direction, void *buf, size_t *xfer_len)
{
    if (drive >= ahci_satapi_device_count || !ahci_satapi_devices[drive].reserved) return 0xFF;

    ahci_port_state_t *port = &ahci_ports[ahci_satapi_devices[drive].port_idx];

    int slot = satapi_find_slot(port);
    if (slot < 0) return 0xFE;

    int ret;
    if (direction == SATAPI_PROT_NODATA) {
        ret = satapi_issue_packet(port, slot, cdb, 12, direction, 0, 0, 0);
    } else {
        uint32_t bytes = byte_limit;
        ret            = satapi_issue_packet(port, slot, cdb, 12, direction, 0, port->dma_buf_phys, bytes);
        if (ret == 0 && buf && xfer_len) {
            size_t copy_len = (*xfer_len < bytes) ? *xfer_len : bytes;
            memcpy(buf, port->dma_buf, copy_len);
            *xfer_len = copy_len;
        }
    }

    return (uint8_t)(ret != 0 ? 0xFF : 0);
}

uint8_t ahci_satapi_test_unit_ready(uint8_t drive)
{
    uint8_t cdb[SATAPI_CDB_LEN] = {GPCMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    return ahci_satapi_send_packet(drive, cdb, 0, SATAPI_PROT_NODATA, NULL, NULL);
}

uint8_t ahci_satapi_read_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size)
{
    uint8_t cdb[SATAPI_CDB_LEN] = {GPCMD_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t cap_buf[8]          = {0};
    size_t  len                 = 8;
    uint8_t err;

    if (!lba_size || !blk_size) return 0xFF;

    err = ahci_satapi_send_packet(drive, cdb, 8, SATAPI_PROT_PIO, cap_buf, &len);
    if (err) return err;

    *lba_size = ((uint32_t)cap_buf[0] << 24) | ((uint32_t)cap_buf[1] << 16) | ((uint32_t)cap_buf[2] << 8) | (uint32_t)cap_buf[3];
    *blk_size = ((uint32_t)cap_buf[4] << 24) | ((uint32_t)cap_buf[5] << 16) | ((uint32_t)cap_buf[6] << 8) | (uint32_t)cap_buf[7];

    return 0;
}

int ahci_satapi_cmd_type(uint8_t opcode)
{
    switch (opcode) {
        case GPCMD_READ_10 :
        case GPCMD_READ_12 :
            return ATAPI_READ;
        case GPCMD_WRITE_10 :
        case GPCMD_WRITE_12 :
            return ATAPI_WRITE;
        case GPCMD_READ_CD :
            return ATAPI_READ_CD;
        default :
            return ATAPI_MISC;
    }
}
