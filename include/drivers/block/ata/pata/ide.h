/*
 *
 *      ide.h
 *      Standard ATA/ATAPI device driver header
 *
 *      2024/7/11 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IDE_H_
#define INCLUDE_IDE_H_

#include <drivers/block/ata/ata_cmds.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DSC   0x10
#define ATA_SR_DRQ   0x08
#define ATA_SR_CORR  0x04
#define ATA_SR_IDX   0x02
#define ATA_SR_ERR   0x01
#define ATA_ER_BBK   0x80
#define ATA_ER_UNC   0x40
#define ATA_ER_MC    0x20
#define ATA_ER_IDNF  0x10
#define ATA_ER_MCR   0x08
#define ATA_ER_ABRT  0x04
#define ATA_ER_TK0NF 0x02
#define ATA_ER_AMNF  0x01

#define ATA_CMD_READ_PIO      0x20
#define ATA_CMD_READ_PIO_EXT  0x24
#define ATA_CMD_READ_DMA      0xc8
#define ATA_CMD_WRITE_PIO     0x30
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_WRITE_DMA     0xca

#define IDE_ATA   0x00
#define IDE_ATAPI 0x01

#define IDE_POLL_RETRY  100000
#define IDE_IRQ_TIMEOUT 500000

/* ide_print_error() result for a medium-not-present sense */
#define IDE_ERR_NO_MEDIUM 24

/* Bus-master IDE register offsets within a channel's BAR4 window */
#define ATA_BM_CMD    0x00
#define ATA_BM_STATUS 0x02
#define ATA_BM_PRDT   0x04

/* ATA_BM_CMD bits */
#define ATA_BM_CMD_START 0x01
#define ATA_BM_CMD_DIR   0x08

/* ATA_BM_STATUS bits */
#define ATA_BM_STATUS_ACTIVE 0x01
#define ATA_BM_STATUS_ERR    0x02
#define ATA_BM_STATUS_INT    0x04
#define ATA_BM_STATUS_CLEAR  0x06

/* PRD flags */
#define ATA_PRD_EOT 0x8000

/* DMA bounce buffer geometry */
#define ATA_DMA_BUF_PAGES   8
#define ATA_DMA_MAX_BYTES   (ATA_DMA_BUF_PAGES * 4096)
#define ATA_DMA_MAX_SECTORS 64

#define ATA_MASTER         0x00
#define ATA_SLAVE          0x01
#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07
#define ATA_REG_SECCOUNT1  0x08
#define ATA_REG_LBA3       0x09
#define ATA_REG_LBA4       0x0a
#define ATA_REG_LBA5       0x0b
#define ATA_REG_CONTROL    0x0c
#define ATA_REG_ALTSTATUS  0x0c
#define ATA_REG_DEVADDRESS 0x0d

#define ATA_PRIMARY   0x00
#define ATA_SECONDARY 0x01

#define ATA_READ  0x00
#define ATA_WRITE 0x01

typedef struct {
        uint32_t base;
        uint16_t count;
        uint16_t flags;
} __attribute__((packed)) ata_prd_t;

typedef struct {
        uint16_t base;
        uint16_t ctrl;
        uint16_t bmide;
        uint8_t  nIEN;

        /* Per-channel bus-master bounce buffer and PRD table */
        uint8_t    dma_ok;
        uint8_t   *dma_buf;
        uint64_t   dma_buf_phys;
        uint64_t   prd_phys;
        ata_prd_t *prd;

        /*
         * Serialises the shared channel registers across concurrent I/O on one
         * cable (two CPUs accessing two drives); both paths poll completion, so
         * the lock is held for the whole transfer.
         */
        spinlock_t lock;

        /*
         * Per-channel interrupt-completion flag (the IRQ handlers for IRQ14
         * and IRQ15 are split so each knows its channel).
         */
        volatile uint8_t irq_pending;
} ide_channel_registers_t;

typedef struct {
        uint8_t  reserved;
        uint8_t  channel;
        uint8_t  drive;
        uint16_t type;
        uint16_t signature;
        uint16_t capabilities;
        uint32_t command_sets;
        uint64_t size;
        uint8_t  model[41];
        uint8_t  dma_enabled;
} ide_device_t;

extern ide_channel_registers_t channels[2];
extern ide_device_t            ide_devices[4];

/* Detect and initialize the IDE controllers and their drives */
#if CONFIG_ATA
void init_ide(void);
#else
static inline void init_ide(void) {}
#endif

/* Wait for the IDE IRQ on a channel to fire */
int ide_wait_irq(uint8_t channel);

/* Read a byte from a register of an IDE channel */
uint8_t ide_read(uint8_t channel, uint8_t reg);

/* Write a byte to a register of an IDE channel */
void ide_write(uint8_t channel, uint8_t reg, uint8_t data);

/* Read multiple 32-bit words from an IDE register into the buffer */
void ide_read_buffer(uint8_t channel, uint8_t reg, uint8_t *buffer, uint32_t quads);

/* Poll the IDE status register until the device is ready (optionally error-checked) */
uint8_t ide_polling(uint8_t channel, uint32_t advanced_check);

/* Soft reset the ATA device */
void ide_soft_reset(uint8_t drive);

/* Drop a drive to PIO after a bus-master error; a stuck BSY resets the channel */
void ide_dma_fallback(uint8_t drive);

/* Flush the drive's write cache */
uint8_t ide_flush_cache(uint8_t drive);

/* Bus-master primitives; callers hold the channel lock */
void    ide_bm_clear_status(uint8_t channel);
void    ide_bm_set_prd(uint8_t channel, uint16_t bytes);
void    ide_bm_start(uint8_t channel, uint8_t write);
void    ide_bm_stop(uint8_t channel);
uint8_t ide_bm_wait(uint8_t channel);

/* Perform an ATA read or write (bus-master DMA when the BIOS mode validates, PIO otherwise) */
uint8_t ide_ata_access(uint8_t direction, uint8_t drive, uint64_t lba, uint8_t numsects, uint16_t *edi);

/* Read multiple sectors from an IDE device (ATA or ATAPI); returns 0 or -errno */
int ide_read_sectors(uint8_t drive, uint8_t numsects, uint64_t lba, uint16_t *edi);

/* Write multiple sectors to an IDE device; returns 0 or -errno */
int ide_write_sectors(uint8_t drive, uint8_t numsects, uint64_t lba, uint16_t *edi);

#endif // INCLUDE_IDE_H_
