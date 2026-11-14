/*
 *
 *      atapi.c
 *      ATAPI (ATA Packet Interface) driver implementation
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <drivers/block/ata/pata/atapi.h>
#include <drivers/block/ata/pata/ide.h>
#include <kernel/debug/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <libs/util/byteorder.h>

#if CONFIG_ATA

/* ATAPI device instances */
atapi_device_t atapi_devices[4];

/* Boot-time DMA/PIO comparison buffers, one block each */
static uint16_t atapi_validate_pio_buf[2048];
static uint16_t atapi_validate_dma_buf[2048];

/* Serialises use of the shared comparison buffers across channels */
static spinlock_t atapi_validate_lock;

static void atapi_validate_dma(uint8_t dev_index);
static int  atapi_probe_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size);

/* Highest ATA/ATAPI major version advertised by the device (word 80 bit index) */
static uint8_t atapi_major_version(const uint16_t *id_words)
{
    if (id_words[80] == 0xffff) return 0;
    for (int v = 14; v >= 1; v--)
        if (id_words[80] & (1 << v)) return (uint8_t)v;
    return 0;
}

/* ATAPI status polling */
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

    static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
    if (ratelimit_allow(&ratelimit)) plogk("atapi: BSY timeout on channel %u\n", channel);
    return status;
}

/* Poll until DRQ is set; log_error marks the command-phase call */
static uint8_t atapi_wait_drq(uint8_t channel, uint8_t log_error)
{
    uint8_t status;
    int     timeout = 400000;

    while (timeout--) {
        status = ide_read(channel, ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            if (log_error) {
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("atapi: ERR on channel %u\n", channel);
            }
            return 2;
        }
        if (status & ATA_SR_DF) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("atapi: DF on channel %u\n", channel);
            return 1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
        nsleep(100);
    }

    static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
    if (ratelimit_allow(&ratelimit)) plogk("atapi: DRQ timeout on channel %u\n", channel);
    return 3;
}

/* ATAPI command type classification */
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

/* Parse ATAPI identify data (called after ATAPI signature detected) */
uint8_t atapi_identify(uint8_t channel, uint8_t drive, uint8_t dev_index)
{
    int k;
    if (dev_index >= 4) return 0xff;

    /*
     * The device has already been selected and IDENTIFY failed with ATAPI signature.
     * Now send IDENTIFY PACKET DEVICE to get the full ATAPI identity.
     */
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
        atapi_devices[dev_index].dma_enabled  = 0;
        atapi_devices[dev_index].dma_capable  = 0;
        atapi_devices[dev_index].dma_dir      = 0;

        /* Extract model string */
        for (k = 0; k < 40; k += 2) {
            atapi_devices[dev_index].model[k]     = id_buf[ATA_IDENT_MODEL + k + 1];
            atapi_devices[dev_index].model[k + 1] = id_buf[ATA_IDENT_MODEL + k];
        }

        atapi_devices[dev_index].model[40] = 0;

        for (k = 39; k > 0; k--) {
            if (atapi_devices[dev_index].model[k] == ' ') {
                atapi_devices[dev_index].model[k] = '\0';
            } else {
                break;
            }
        }

        if ((id_words[49] & 0x100) && channels[channel].dma_ok) atapi_devices[dev_index].dma_capable = 1;
        if ((id_words[62] & 0x8000) && atapi_major_version(id_words) >= 7) atapi_devices[dev_index].dma_dir = 1;
    }

    /* Get real capacity via READ CAPACITY; slow drives report becoming ready */
    {
        uint32_t lba     = 0;
        uint32_t bsz     = 0;
        int      cap_ret = atapi_probe_capacity(dev_index, &lba, &bsz);

        if (cap_ret == 0) {
            atapi_devices[dev_index].lba_size = lba;
            atapi_devices[dev_index].blk_size = bsz;
        }
    }

    if (atapi_devices[dev_index].dma_capable && atapi_devices[dev_index].lba_size != 0 && atapi_devices[dev_index].blk_size != 0
        && atapi_devices[dev_index].blk_size <= sizeof(atapi_validate_pio_buf)) {
        atapi_validate_dma(dev_index);
    }

    return 0;
}

/* ATAPI soft reset */
void atapi_soft_reset(uint8_t drive)
{
    uint8_t channel = atapi_devices[drive].channel;

    channels[channel].irq_pending = 0;
    channels[channel].nIEN        = 0x04;

    ide_write(channel, ATA_REG_CONTROL, 0x04);
    nsleep(5000);

    channels[channel].irq_pending = 0;
    channels[channel].nIEN        = 0x00;

    ide_write(channel, ATA_REG_CONTROL, 0x00);
    nsleep(5000);
    atapi_wait_busy(channel);
}

/* Core ATAPI packet send; the caller holds the channel lock */
static uint8_t atapi_send_packet_locked(uint8_t drive, const uint8_t *cdb, uint16_t byte_limit, uint8_t direction, void *buf, size_t *xfer_len, uint8_t log_error)
{
    uint32_t channel  = atapi_devices[drive].channel;
    uint32_t slavebit = atapi_devices[drive].drive;
    uint32_t bus      = channels[channel].base;
    uint8_t  use_dma  = (direction == ATAPI_PROT_DMA) && atapi_devices[drive].dma_enabled && channels[channel].dma_ok;
    uint8_t  err;

    /*
     * Wait for BSY to clear, then drain any stale DRQ data from previous
     * commands (QEMU ATAPI emulation sometimes leaves DRQ stuck).
     */
    {
        int tout = IDE_POLL_RETRY;
        while (tout--) {
            uint8_t st = ide_read(channel, ATA_REG_STATUS);
            if (!(st & ATA_SR_BSY)) break;
            nsleep(100);
        }
        if (tout <= 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("atapi: BSY stuck on ch%u drv%u before PACKET.\n", channel, slavebit);
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

    /* Select device and disable IRQ; this path polls. */
    channels[channel].irq_pending = 0;
    channels[channel].nIEN        = 0x02;
    ide_write(channel, ATA_REG_CONTROL, 0x02);
    ide_write(channel, ATA_REG_HDDEVSEL, 0xa0 | (slavebit << 4));

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
    if (use_dma && (byte_limit == 0 || byte_limit > ATA_DMA_MAX_BYTES || !buf || !xfer_len || *xfer_len < byte_limit)) return 2;

    /* Set features and byte count limit */
    if (use_dma) {
        ide_write(channel, ATA_REG_FEATURES, ATAPI_PKT_DMA | (atapi_devices[drive].dma_dir ? ATAPI_DMADIR : 0));
    } else {
        ide_write(channel, ATA_REG_FEATURES, 0);
    }
    ide_write(channel, ATA_REG_LBA1, byte_limit & 0xff);
    ide_write(channel, ATA_REG_LBA2, (byte_limit >> 8) & 0xff);

    /* Send PACKET command */
    ide_write(channel, ATA_REG_COMMAND, ATA_CMD_PACKET);

    /* Wait for device to accept CDB */
    err = atapi_wait_drq(channel, log_error);
    if (err) return err;

    /* Check interrupt reason before sending CDB */
    {
        uint8_t ireason = ide_read(channel, ATA_REG_SECCOUNT0);
        if (!(ireason & ATAPI_COD)) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("atapi: Expected COD=1, got ireason=0x%02x\n", ireason);
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
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("atapi: NODATA BSY timeout on channel %u\n", channel);
                return 3;
            }
            nsleep(100);
        }
        if (status & ATA_SR_ERR) return 2;
        return 0;
    }

    /* DMA data phase (device to host only); the engine waits for the device's DMARQ */
    if (use_dma) {
        uint16_t bytes = byte_limit;

        ide_bm_clear_status(channel);
        ide_bm_set_prd(channel, bytes);
        ide_bm_start(channel, 0);

        err = ide_bm_wait(channel);
        if (err != 0) {
            ide_bm_stop(channel);
            ide_bm_clear_status(channel);
            return err;
        }

        dma_read_barrier();
        memcpy(buf, channels[channel].dma_buf, bytes);
        *xfer_len = bytes;
    } else if (direction != ATAPI_PROT_NODATA && buf && xfer_len) {
        /* PIO data transfer */
        size_t    transferred = 0;
        uint16_t *word_buf    = (uint16_t *)buf;

        while (transferred < *xfer_len) {
            /* Poll for data ready instead of IRQ - interrupts may not be enabled yet when init_ide() runs. */
            err = atapi_wait_drq(channel, 0);
            if (err) return err;

            uint8_t status = ide_read(channel, ATA_REG_STATUS);
            if (status & ATA_SR_ERR) return 2;

            uint8_t  ireason = ide_read(channel, ATA_REG_SECCOUNT0);
            uint8_t  bc_lo   = ide_read(channel, ATA_REG_LBA1);
            uint8_t  bc_hi   = ide_read(channel, ATA_REG_LBA2);
            uint16_t bc      = (bc_hi << 8) | bc_lo;

            /* COD must be 0 (data phase) */
            if (ireason & ATAPI_COD) {
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("atapi: Unexpected COD=1 in data phase.\n");
                return 2;
            }

            /* IO=1 means device to host (read), IO=0 means host to device (write) */
            if (!(ireason & ATAPI_IO)) {
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("atapi: Unexpected IO direction.\n");
                return 2;
            }
            if (bc == 0) break;

            /*
             * Must read ALL data the device sends to maintain protocol sync.
             * Only store into buffer if there is room; discard overflow.
             */
            uint16_t words = bc / 2;
            for (uint16_t h = 0; h < words; h++) {
                uint16_t val = inw(bus);
                if (transferred + ((size_t)h * 2) < *xfer_len) word_buf[(transferred / 2) + h] = val;
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
        if (timeout < 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("atapi: Completion timeout after data phase on ch%u drv%u\n", channel, slavebit);
            return 3;
        }
    }

    /* The final packet may report CHECK CONDITION after DRQ has dropped */
    if (ide_read(channel, ATA_REG_STATUS) & ATA_SR_ERR) return 2;
    return 0;
}

/*
 * The firmware paired host timing with the drive's DMA mode; reuse it instead of
 * per-vendor chipset programming (Intel/VIA/SiS/AMD/NVIDIA/ATI/...).  A boot-time
 * DMA/PIO comparison validates it; full speed still needs those chipset drivers.
 */
static void atapi_validate_dma(uint8_t dev_index)
{
    uint8_t  channel            = atapi_devices[dev_index].channel;
    uint32_t blk_size           = atapi_devices[dev_index].blk_size;
    uint8_t  cdb[ATAPI_CDB_LEN] = {GPCMD_READ_12, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    size_t   pio_len            = blk_size;
    size_t   dma_len            = blk_size;
    uint8_t  dma_tried          = 0;
    uint8_t  err;

    if (!channels[channel].dma_ok) {
        atapi_devices[dev_index].dma_enabled = 0;
        return;
    }

    spin_lock(&atapi_validate_lock);
    spin_lock(&channels[channel].lock);

    /* Start from PIO until the DMA comparison proves the firmware pairing */
    atapi_devices[dev_index].dma_enabled = 0;

    err = atapi_send_packet_locked(dev_index, cdb, (uint16_t)blk_size, ATAPI_PROT_PIO, atapi_validate_pio_buf, &pio_len, 1);
    if (err == 0 && pio_len != blk_size) err = 4;
    if (err == 0) {
        dma_tried                            = 1;
        atapi_devices[dev_index].dma_enabled = 1;
        err                                  = atapi_send_packet_locked(dev_index, cdb, (uint16_t)blk_size, ATAPI_PROT_DMA, atapi_validate_dma_buf, &dma_len, 1);
        if (err == 0 && (dma_len != blk_size || memcmp(atapi_validate_pio_buf, atapi_validate_dma_buf, blk_size) != 0)) err = 4;
    }

    if (err != 0) {
        if (!dma_tried) {
            atapi_devices[dev_index].dma_enabled = 0;
            plogk("atapi: DMA validation skipped on drive %u (PIO read error %u), using PIO.\n", dev_index, err);
        } else {
            ide_dma_fallback(dev_index);
            plogk("atapi: DMA validation failed on drive %u, using PIO.\n", dev_index);
        }
    }

    spin_unlock(&channels[channel].lock);
    spin_unlock(&atapi_validate_lock);
}

/* Core ATAPI packet send function */
uint8_t atapi_send_packet(uint8_t drive, const uint8_t *cdb, uint16_t byte_limit, uint8_t direction, void *buf, size_t *xfer_len)
{
    uint8_t channel = atapi_devices[drive].channel;
    uint8_t err;

    spin_lock(&channels[channel].lock);
    err = atapi_send_packet_locked(drive, cdb, byte_limit, direction, buf, xfer_len, 1);

    spin_unlock(&channels[channel].lock);
    return err;
}

/* TEST UNIT READY */
uint8_t atapi_test_unit_ready(uint8_t drive)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    return atapi_send_packet(drive, cdb, 0, ATAPI_PROT_NODATA, NULL, NULL);
}

/* INQUIRY */
uint8_t atapi_inquiry(uint8_t drive, void *buf)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_INQUIRY, 0, 0, 0, 36, 0};
    size_t  len                = 36;
    return atapi_send_packet(drive, cdb, 36, ATAPI_PROT_PIO, buf, &len);
}

/* READ CAPACITY with the channel lock held */
static uint8_t atapi_read_capacity_locked(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size, uint8_t log_error)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t cap_buf[8]         = {0};
    size_t  len                = 8;
    uint8_t err;

    err = atapi_send_packet_locked(drive, cdb, 8, ATAPI_PROT_PIO, cap_buf, &len, log_error);
    if (err) return err;

    /* READ CAPACITY returns the last addressable LBA; the sector count is one more. */
    *lba_size = load_be32(cap_buf) + 1;
    *blk_size = load_be32(cap_buf + 4);

    return 0;
}

/* READ CAPACITY */
uint8_t atapi_read_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size)
{
    uint8_t channel;
    uint8_t err;

    if (!lba_size || !blk_size) return 0xff;
    if (drive > 3 || !atapi_devices[drive].reserved) return 0xff;

    channel = atapi_devices[drive].channel;
    spin_lock(&channels[channel].lock);

    err = atapi_read_capacity_locked(drive, lba_size, blk_size, 1);
    spin_unlock(&channels[channel].lock);

    return err;
}

/* REQUEST SENSE with the channel lock held */
static uint8_t atapi_request_sense_locked(uint8_t drive, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq, uint8_t log_error)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_REQUEST_SENSE, 0, 0, 0, SCSI_SENSE_BUFFER_SIZE, 0};
    uint8_t sense_buf[SCSI_SENSE_BUFFER_SIZE];
    size_t  len = SCSI_SENSE_BUFFER_SIZE;
    uint8_t err;

    for (int i = 0; i < SCSI_SENSE_BUFFER_SIZE; i++) sense_buf[i] = 0;
    err = atapi_send_packet_locked(drive, cdb, SCSI_SENSE_BUFFER_SIZE, ATAPI_PROT_PIO, sense_buf, &len, log_error);
    if (err) return err;

    /* Sense data format: byte 0 = error code, byte 2 = sense key, byte 12 = ASC, byte 13 = ASCQ */
    if (sense_key) *sense_key = sense_buf[2] & 0x0f;
    if (asc) *asc = sense_buf[12];
    if (ascq) *ascq = sense_buf[13];

    return 0;
}

/* Clear the unit attention raised by a channel reset; the caller holds the lock */
void atapi_reset_recover(uint8_t drive)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {GPCMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    uint8_t key                = 0;
    uint8_t asc                = 0;
    uint8_t ascq               = 0;

    (void)atapi_send_packet_locked(drive, cdb, 0, ATAPI_PROT_NODATA, NULL, NULL, 0);
    (void)atapi_request_sense_locked(drive, &key, &asc, &ascq, 0);
}

/* Probe capacity with sense-driven retries; the caller does not hold the lock */
static int atapi_probe_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size)
{
    uint32_t channel = atapi_devices[drive].channel;
    uint32_t lba     = 0;
    uint32_t blk     = 0;

    for (int attempt = 0; attempt < 20; attempt++) {
        uint8_t key       = 0;
        uint8_t asc       = 0;
        uint8_t ascq      = 0;
        uint8_t retryable = 0;

        spin_lock(&channels[channel].lock);
        if (atapi_read_capacity_locked(drive, &lba, &blk, 0) == 0) {
            spin_unlock(&channels[channel].lock);
            if (lba == 0 || blk == 0 || blk > 4096) return -ENOMEDIUM;
            *lba_size = lba;
            *blk_size = blk;
            return 0;
        }

        /*
         * A new disc reports unit attention or becoming ready; 0x3A also means empty,
         * so it gets only two quick retries; the next command after a change clears it.
         */
        if (atapi_request_sense_locked(drive, &key, &asc, &ascq, 0) == 0) {
            if ((key == 6 && asc == 0x28) || (key == 2 && (asc == 0x04 || asc == 0x11))) {
                retryable = 2;
            } else if (key == 2 && asc == 0x3A && attempt < 2) {
                retryable = 1;
            }
        }
        spin_unlock(&channels[channel].lock);

        if (!retryable) break;
        if (retryable == 2) msleep(50);
    }

    return -ENOMEDIUM;
}

/* REQUEST SENSE */
uint8_t atapi_request_sense(uint8_t drive, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq)
{
    uint8_t channel;
    uint8_t err;

    if (drive > 3 || !atapi_devices[drive].reserved) return 0xff;

    channel = atapi_devices[drive].channel;
    spin_lock(&channels[channel].lock);

    err = atapi_request_sense_locked(drive, sense_key, asc, ascq, 1);
    spin_unlock(&channels[channel].lock);

    return err;
}

/* Refresh capacity after a possible disc change. */
int atapi_revalidate(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size)
{
    int     ret;
    uint8_t was_empty;

    if (drive > 3 || !atapi_devices[drive].reserved || atapi_devices[drive].type != IDE_ATAPI) return -ENODEV;
    if (!lba_size || !blk_size) return -EINVAL;

    was_empty = atapi_devices[drive].lba_size == 0;
    ret       = atapi_probe_capacity(drive, lba_size, blk_size);

    if (ret != 0) {
        atapi_devices[drive].lba_size = 0;
        ide_devices[drive].size       = 0;
        return ret;
    }

    atapi_devices[drive].lba_size = *lba_size;
    atapi_devices[drive].blk_size = *blk_size;
    ide_devices[drive].size       = *lba_size;

    /* An empty drive was never validated against the firmware's DMA pairing */
    if (was_empty && atapi_devices[drive].dma_capable && !atapi_devices[drive].dma_enabled && *blk_size <= sizeof(atapi_validate_pio_buf)) atapi_validate_dma(drive);
    ide_devices[drive].dma_enabled = atapi_devices[drive].dma_enabled;

    return EOK;
}

/* START STOP UNIT (eject/load) */
uint8_t atapi_start_stop(uint8_t drive, uint8_t start, uint8_t loej)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {
        GPCMD_START_STOP_UNIT, 0, 0, 0, ((loej ? 1 : 0) << 1) | (start ? 1 : 0), 0,
    };
    return atapi_send_packet(drive, cdb, 0, ATAPI_PROT_NODATA, NULL, NULL);
}

/* Build a READ(12) CDB for count blocks starting at lba */
static void atapi_read12_cdb(uint8_t *cdb, uint32_t lba, uint8_t count)
{
    memset(cdb, 0, ATAPI_CDB_LEN);
    cdb[0] = GPCMD_READ_12;
    cdb[2] = (lba >> 24) & 0xff;
    cdb[3] = (lba >> 16) & 0xff;
    cdb[4] = (lba >> 8) & 0xff;
    cdb[5] = lba & 0xff;
    cdb[8] = (count >> 8) & 0xff;
    cdb[9] = count & 0xff;
}

/* Read sectors from ATAPI device (READ 12) */
uint8_t atapi_read(uint8_t drive, uint32_t lba, uint8_t num_sectors, uint16_t *buf)
{
    uint8_t  channel  = atapi_devices[drive].channel;
    uint32_t blk_size = atapi_devices[drive].blk_size;
    uint32_t done     = 0;
    uint8_t  err      = 0;

    if (blk_size == 0) blk_size = 2048;
    if (blk_size > 0xfffe) return 2;

    spin_lock(&channels[channel].lock);

    if (atapi_devices[drive].dma_enabled && channels[channel].dma_ok && blk_size <= ATA_DMA_MAX_BYTES) {
        uint32_t per_packet = ATA_DMA_MAX_BYTES / blk_size;

        while (done < num_sectors) {
            uint8_t  chunk     = num_sectors - done;
            uint32_t chunk_lba = lba + done;
            uint16_t byte_limit;
            size_t   xfer_len;
            uint8_t  cdb[ATAPI_CDB_LEN];

            if (chunk > per_packet) chunk = (uint8_t)per_packet;
            byte_limit = (uint16_t)(chunk * blk_size);
            xfer_len   = (size_t)chunk * blk_size;
            atapi_read12_cdb(cdb, chunk_lba, chunk);

            err = atapi_send_packet_locked(drive, cdb, byte_limit, ATAPI_PROT_DMA, (uint8_t *)buf + ((size_t)done * blk_size), &xfer_len, 1);
            if (err != 0) break;
            done += chunk;
        }

        if (err != 0) {
            ide_dma_fallback(drive);
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("atapi: DMA error %u on drive %u, falling back to PIO.\n", err, drive);
            err = 0;
        }
    }

    if (done < num_sectors) {
        uint32_t per_packet = 0xfffe / blk_size;

        /* The 16-bit byte count limit caps each PACKET, so long reads are split */
        while (done < num_sectors) {
            uint8_t  chunk     = num_sectors - done;
            uint32_t chunk_lba = lba + done;
            uint16_t byte_limit;
            size_t   xfer_len;
            uint8_t  cdb[ATAPI_CDB_LEN];

            if (chunk > per_packet) chunk = (uint8_t)per_packet;
            byte_limit = (uint16_t)(chunk * blk_size);
            xfer_len   = (size_t)chunk * blk_size;
            atapi_read12_cdb(cdb, chunk_lba, chunk);

            err = atapi_send_packet_locked(drive, cdb, byte_limit, ATAPI_PROT_PIO, (uint8_t *)buf + ((size_t)done * blk_size), &xfer_len, 1);
            if (err == 0 && xfer_len != (size_t)chunk * blk_size) err = 5;
            if (err != 0) {
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("atapi: Read error %u on drive %u at block %u\n", err, drive, chunk_lba);
                break;
            }
            done += chunk;
        }
    }

    spin_unlock(&channels[channel].lock);
    return err;
}

#endif
