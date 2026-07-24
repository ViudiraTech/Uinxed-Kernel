/*
 *
 *      atapi.c
 *      ATAPI (ATA Packet Interface) driver implementation
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <drivers/ide/atapi.h>
#include <drivers/ide/ide.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* ATAPI device instances */
atapi_device_t atapi_devices[4];

/* ─── ATAPI status polling ─── */
static uint8_t atapi_wait_busy(uint8_t channel)
{
    uint8_t status;
    int     timeout = 100000;

    for (int i = 0; i < 4; i++) ide_read(channel, ATA_REG_ALTSTATUS);

    while (timeout--) {
        status = ide_read(channel, ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) return status;
        nsleep(100);
    }
    plogk("atapi: BSY timeout on channel %u\n", channel);
    return status;
}

static uint8_t atapi_wait_drq(uint8_t channel)
{
    uint8_t status;
    int     timeout = 400000;

    while (timeout--) {
        status = ide_read(channel, ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            plogk("atapi: ERR on channel %u\n", channel);
            return 2;
        }
        if (status & ATA_SR_DF) {
            plogk("atapi: DF on channel %u\n", channel);
            return 1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
        nsleep(100);
    }
    plogk("atapi: DRQ timeout on channel %u\n", channel);
    return 3;
}

/* ─── ATAPI command type classification ─── */
int atapi_cmd_type(uint8_t opcode)
{
    switch (opcode) {
        case GPCMD_READ_10 :
        case GPCMD_READ_12 :
            return ATAPI_READ;
        case GPCMD_WRITE_10 :
        case GPCMD_WRITE_12 :
        case GPCMD_WRITE_AND_VERIFY_10 :
            return ATAPI_WRITE;
        case GPCMD_READ_CD :
        case GPCMD_READ_CD_MSF :
            return ATAPI_READ_CD;
        case GPCMD_FORMAT_UNIT :
        default :
            return ATAPI_MISC;
    }
}

/* ─── Parse ATAPI identify data (called after ATAPI signature detected) ─── */
uint8_t atapi_identify(uint8_t channel, uint8_t drive, uint8_t dev_index)
{
    int k;

    if (dev_index >= 4) return 0xff;

    /* The device has already been selected and IDENTIFY failed with ATAPI signature.
     * Now send IDENTIFY PACKET DEVICE to get the full ATAPI identity. */
    ide_write(channel, ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
    nsleep(10);

    /* Wait for command completion */
    {
        uint8_t status;
        int     timeout = IDE_POLL_RETRY;
        while (timeout--) {
            status = ide_read(channel, ATA_REG_STATUS);
            if (status & ATA_SR_ERR) {
                plogk("atapi: IDENTIFY PACKET failed on channel %u\n", channel);
                return 0xff;
            }
            if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) break;
            nsleep(100);
        }
        if (timeout <= 0) {
            plogk("atapi: IDENTIFY PACKET timeout on channel %u\n", channel);
            return 0xff;
        }
    }

    /* Read identification data (128 dwords = 256 words = 512 bytes) */
    {
        uint8_t   id_buf[512];
        uint16_t *id_words = (uint16_t *)id_buf;

        ide_read_buffer(channel, ATA_REG_DATA, id_buf, 128);

        atapi_devices[dev_index].reserved     = 1;
        atapi_devices[dev_index].channel      = channel;
        atapi_devices[dev_index].drive        = drive;
        atapi_devices[dev_index].type         = IDE_ATAPI;
        atapi_devices[dev_index].pkt_prot     = (id_words[0] >> 8) & 0x1f;
        atapi_devices[dev_index].capabilities = id_words[0] & 0xff;
        atapi_devices[dev_index].lba_size     = 0;
        atapi_devices[dev_index].blk_size     = 2048;

        /* Extract model string */
        for (k = 0; k < 40; k += 2) {
            atapi_devices[dev_index].model[k]     = id_buf[ATA_IDENT_MODEL + k + 1];
            atapi_devices[dev_index].model[k + 1] = id_buf[ATA_IDENT_MODEL + k];
        }
        atapi_devices[dev_index].model[40] = 0;
    }

    /* Get real capacity via READ CAPACITY */
    {
        uint32_t lba     = 0;
        uint32_t bsz     = 0;
        uint8_t  cap_ret = atapi_read_capacity(dev_index, &lba, &bsz);
        if (cap_ret == 0) {
            atapi_devices[dev_index].lba_size = lba;
            atapi_devices[dev_index].blk_size = bsz;
        } else {
            /* Fallback: some devices may need retry after TEST UNIT READY */
            atapi_test_unit_ready(dev_index);
            nsleep(100000);
            cap_ret = atapi_read_capacity(dev_index, &lba, &bsz);
            if (cap_ret == 0) {
                atapi_devices[dev_index].lba_size = lba;
                atapi_devices[dev_index].blk_size = bsz;
            }
        }
    }

    return 0;
}

/* ─── ATAPI soft reset ─── */
void atapi_soft_reset(uint8_t drive)
{
    uint8_t channel = atapi_devices[drive].channel;

    ide_irq_invoked        = 0;
    channels[channel].nIEN = 0x04;
    ide_write(channel, ATA_REG_CONTROL, 0x04);
    nsleep(5000);
    ide_irq_invoked        = 0;
    channels[channel].nIEN = 0x00;
    ide_write(channel, ATA_REG_CONTROL, 0x00);
    nsleep(5000);
    atapi_wait_busy(channel);
}

/* ─── Core ATAPI packet send function ─── */
uint8_t atapi_send_packet(uint8_t drive, const uint8_t *cdb, uint16_t byte_limit, uint8_t direction, void *buf, size_t *xfer_len)
{
    uint32_t channel  = atapi_devices[drive].channel;
    uint32_t slavebit = atapi_devices[drive].drive;
    uint32_t bus      = channels[channel].base;
    uint8_t  err;

    /* Wait for BSY to clear, then drain any stale DRQ data from previous
     * commands (QEMU ATAPI emulation sometimes leaves DRQ stuck). */
    {
        int tout = IDE_POLL_RETRY;
        while (tout--) {
            uint8_t st = ide_read(channel, ATA_REG_STATUS);
            if (!(st & ATA_SR_BSY)) break;
            nsleep(100);
        }
        if (tout <= 0) {
            plogk("atapi: BSY stuck on ch%u drv%u before PACKET.\n", channel, slavebit);
            return 3;
        }
    }
    {
        int tout = IDE_POLL_RETRY;
        while (tout--) {
            uint8_t st = ide_read(channel, ATA_REG_STATUS);
            if (st & ATA_SR_BSY) {
                nsleep(100);
                continue;
            }
            if (!(st & ATA_SR_DRQ)) break;
            inw(bus);
        }
    }

    /* Select device and disable IRQ — we use polling, not interrupts. */
    ide_irq_invoked        = 0;
    channels[channel].nIEN = 0x02;
    ide_write(channel, ATA_REG_CONTROL, 0x02);
    ide_write(channel, ATA_REG_HDDEVSEL, slavebit << 4);

    /* Delay for device selection to settle, then wait for BSY clear */
    for (int i = 0; i < 10; i++) ide_read(channel, ATA_REG_ALTSTATUS);
    {
        int tout = IDE_POLL_RETRY;
        while (tout--) {
            uint8_t st = ide_read(channel, ATA_REG_STATUS);
            if (!(st & ATA_SR_BSY)) break;
            nsleep(100);
        }
    }

    /* Set features and byte count limit */
    if (direction == ATAPI_PROT_DMA) {
        ide_write(channel, ATA_REG_FEATURES, ATAPI_PKT_DMA);
    } else {
        ide_write(channel, ATA_REG_FEATURES, 0);
    }
    ide_write(channel, ATA_REG_LBA1, byte_limit & 0xff);
    ide_write(channel, ATA_REG_LBA2, (byte_limit >> 8) & 0xff);

    /* Send PACKET command */
    ide_write(channel, ATA_REG_COMMAND, ATA_CMD_PACKET);

    /* Wait for device to accept CDB */
    err = atapi_wait_drq(channel);
    if (err) return err;

    /* Check interrupt reason before sending CDB */
    {
        uint8_t ireason = ide_read(channel, ATA_REG_SECCOUNT0);
        if (!(ireason & ATAPI_COD)) {
            plogk("atapi: Expected COD=1, got ireason=0x%02x\n", ireason);
            return 2;
        }
    }

    /* Send CDB (as 16-bit words, 12 bytes minimum for group-5 commands) */
    {
        uint16_t *cdb_words = (uint16_t *)cdb;
        for (int i = 0; i < 6; i++) outw(bus, cdb_words[i]);
    }

    /* Handle data phase based on direction */
    if (direction == ATAPI_PROT_NODATA) {
        int     timeout = IDE_POLL_RETRY;
        uint8_t status  = ide_read(channel, ATA_REG_STATUS);
        while (status & ATA_SR_BSY) {
            status = ide_read(channel, ATA_REG_STATUS);
            if (--timeout <= 0) {
                plogk("atapi: NODATA BSY timeout on channel %u\n", channel);
                return 3;
            }
            nsleep(100);
        }
        if (status & ATA_SR_ERR) return 2;
        return 0;
    }

    /* PIO data transfer */
    if (direction == ATAPI_PROT_PIO && buf && xfer_len) {
        size_t    transferred = 0;
        uint16_t *word_buf    = (uint16_t *)buf;

        while (transferred < *xfer_len) {
            /* Poll for data ready instead of IRQ — interrupts may not be
             * enabled yet when init_ide() runs. */
            err = atapi_wait_drq(channel);
            if (err) return err;

            uint8_t status = ide_read(channel, ATA_REG_STATUS);
            if (status & ATA_SR_ERR) return 2;

            uint8_t  ireason = ide_read(channel, ATA_REG_SECCOUNT0);
            uint8_t  bc_lo   = ide_read(channel, ATA_REG_LBA1);
            uint8_t  bc_hi   = ide_read(channel, ATA_REG_LBA2);
            uint16_t bc      = (bc_hi << 8) | bc_lo;

            /* COD must be 0 (data phase) */
            if (ireason & ATAPI_COD) {
                plogk("atapi: Unexpected COD=1 in data phase.\n");
                return 2;
            }

            /* IO=1 means device to host (read), IO=0 means host to device (write) */
            if (!(ireason & ATAPI_IO)) {
                plogk("atapi: Unexpected IO direction.\n");
                return 2;
            }

            if (bc == 0) break;

            /* Must read ALL data the device sends to maintain protocol sync.
             * Only store into buffer if there is room; discard overflow. */
            uint16_t words = bc / 2;
            for (uint16_t h = 0; h < words; h++) {
                uint16_t val = inw(bus);
                if (transferred < *xfer_len) word_buf[transferred / 2 + h] = val;
            }

            size_t remaining = *xfer_len - transferred;
            transferred += (bc > remaining) ? remaining : bc;
        }
        *xfer_len = transferred;
    }

    /* Wait for device to finish and clear DRQ/BSY */
    {
        int timeout = IDE_POLL_RETRY;
        while (timeout--) {
            uint8_t st = ide_read(channel, ATA_REG_STATUS);
            if (!(st & (ATA_SR_BSY | ATA_SR_DRQ))) break;
            nsleep(100);
        }
    }

    return 0;
}

/* ─── TEST UNIT READY ─── */
uint8_t atapi_test_unit_ready(uint8_t drive)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    return atapi_send_packet(drive, cdb, 0, ATAPI_PROT_NODATA, NULL, NULL);
}

/* ─── INQUIRY ─── */
uint8_t atapi_inquiry(uint8_t drive, void *buf)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_INQUIRY, 0, 0, 0, 36, 0};
    size_t  len                = 36;
    return atapi_send_packet(drive, cdb, 36, ATAPI_PROT_PIO, buf, &len);
}

/* ─── READ CAPACITY ─── */
uint8_t atapi_read_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t cap_buf[8]         = {0};
    size_t  len                = 8;
    uint8_t err;

    if (!lba_size || !blk_size) return 0xff;

    err = atapi_send_packet(drive, cdb, 8, ATAPI_PROT_PIO, cap_buf, &len);
    if (err) return err;

    /* Parse big-endian values from response */
    *lba_size = ((uint32_t)cap_buf[0] << 24) | ((uint32_t)cap_buf[1] << 16) | ((uint32_t)cap_buf[2] << 8) | (uint32_t)cap_buf[3];
    *blk_size = ((uint32_t)cap_buf[4] << 24) | ((uint32_t)cap_buf[5] << 16) | ((uint32_t)cap_buf[6] << 8) | (uint32_t)cap_buf[7];

    return 0;
}

/* ─── REQUEST SENSE ─── */
uint8_t atapi_request_sense(uint8_t drive, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_REQUEST_SENSE, 0, 0, 0, SCSI_SENSE_BUFFER_SIZE, 0};
    uint8_t sense_buf[SCSI_SENSE_BUFFER_SIZE];
    size_t  len = SCSI_SENSE_BUFFER_SIZE;
    uint8_t err;

    for (int i = 0; i < SCSI_SENSE_BUFFER_SIZE; i++) sense_buf[i] = 0;

    err = atapi_send_packet(drive, cdb, SCSI_SENSE_BUFFER_SIZE, ATAPI_PROT_PIO, sense_buf, &len);
    if (err) return err;

    /* Sense data format: byte 0 = error code, byte 2 = sense key, byte 12 = ASC, byte 13 = ASCQ */
    if (sense_key) *sense_key = sense_buf[2] & 0x0f;
    if (asc) *asc = sense_buf[12];
    if (ascq) *ascq = sense_buf[13];

    return 0;
}

/* ─── START STOP UNIT (eject/load) ─── */
uint8_t atapi_start_stop(uint8_t drive, uint8_t start, uint8_t loej)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {
        GPCMD_START_STOP_UNIT, 0, 0, 0, ((loej ? 1 : 0) << 1) | (start ? 1 : 0), 0,
    };
    return atapi_send_packet(drive, cdb, 0, ATAPI_PROT_NODATA, NULL, NULL);
}

/* ─── Read sectors from ATAPI device (READ 12) ─── */
uint8_t atapi_read(uint8_t drive, uint32_t lba, uint8_t num_sectors, uint16_t *buf)
{
    uint32_t blk_size = atapi_devices[drive].blk_size;
    if (blk_size == 0) blk_size = 2048;

    uint32_t total_bytes = (uint32_t)num_sectors * blk_size;
    uint16_t byte_limit  = (uint16_t)(total_bytes > 0xfffe ? 0xfffe : total_bytes);
    size_t   xfer_len    = (size_t)total_bytes;

    /* Build READ(12) CDB */
    uint8_t cdb[ATAPI_CDB_LEN] = {
        GPCMD_READ_12,
        0,
        (lba >> 24) & 0xff,
        (lba >> 16) & 0xff,
        (lba >> 8) & 0xff,
        lba & 0xff,
        0,
        0,
        (num_sectors >> 24) & 0xff,
        (num_sectors >> 16) & 0xff,
        (num_sectors >> 8) & 0xff,
        num_sectors & 0xff,
    };

    return atapi_send_packet(drive, cdb, byte_limit, ATAPI_PROT_PIO, buf, &xfer_len);
}
