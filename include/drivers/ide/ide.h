/*
 *
 *      ide.h
 *      Standard ATA/ATAPI device driver header
 *
 *      2024/7/11 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_IDE_IDE_H_
#define INCLUDE_DRIVERS_IDE_IDE_H_

#include <libs/std/stdint.h>

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

#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_READ_PIO_EXT 0x24
#define ATA_CMD_READ_DMA     0xc8
#ifndef ATA_CMD_READ_DMA_EXT
#    define ATA_CMD_READ_DMA_EXT 0x25
#endif
#define ATA_CMD_WRITE_PIO     0x30
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_WRITE_DMA     0xca
#ifndef ATA_CMD_WRITE_DMA_EXT
#    define ATA_CMD_WRITE_DMA_EXT 0x35
#endif
#ifndef ATA_CMD_CACHE_FLUSH
#    define ATA_CMD_CACHE_FLUSH 0xe7
#endif
#ifndef ATA_CMD_CACHE_FLUSH_EXT
#    define ATA_CMD_CACHE_FLUSH_EXT 0xea
#endif
#define ATA_CMD_SET_FEATURES 0xef
#ifndef ATA_CMD_PACKET
#    define ATA_CMD_PACKET 0xa0
#endif
#ifndef ATA_CMD_IDENTIFY_PACKET
#    define ATA_CMD_IDENTIFY_PACKET 0xa1
#endif
#ifndef ATA_CMD_IDENTIFY
#    define ATA_CMD_IDENTIFY 0xec
#endif

#define ATA_IDENT_DEVICETYPE   0
#define ATA_IDENT_CYLINDERS    2
#define ATA_IDENT_HEADS        6
#define ATA_IDENT_SECTORS      12
#define ATA_IDENT_SERIAL       20
#define ATA_IDENT_MODEL        54
#define ATA_IDENT_CAPABILITIES 98
#define ATA_IDENT_FIELDVALID   106
#define ATA_IDENT_MAX_LBA      120
#define ATA_IDENT_COMMANDSETS  164
#define ATA_IDENT_MAX_LBA_EXT  200

#define IDE_ATA   0x00
#define IDE_ATAPI 0x01

#define IDE_POLL_RETRY  100000
#define IDE_IRQ_TIMEOUT 500000

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
        uint16_t base;
        uint16_t ctrl;
        uint16_t bmide;
        uint8_t  nIEN;
} ide_channel_registers_t;

typedef struct {
        uint8_t  reserved;
        uint8_t  channel;
        uint8_t  drive;
        uint16_t type;
        uint16_t signature;
        uint16_t capabilities;
        uint32_t command_sets;
        uint32_t size;
        uint8_t  model[41];
} ide_device_t;

extern ide_channel_registers_t channels[2];
extern ide_device_t            ide_devices[4];
extern volatile uint8_t        ide_irq_invoked;

void    init_ide(void);
int     ide_wait_irq(void);
uint8_t ide_read(uint8_t channel, uint8_t reg);
void    ide_write(uint8_t channel, uint8_t reg, uint8_t data);
void    ide_read_buffer(uint8_t channel, uint8_t reg, uint8_t *buffer, uint32_t quads);
uint8_t ide_polling(uint8_t channel, uint32_t advanced_check);
void    ide_soft_reset(uint8_t drive);
uint8_t ide_flush_cache(uint8_t drive);
uint8_t ide_ata_access(uint8_t direction, uint8_t drive, uint32_t lba, uint8_t numsects, uint16_t *edi);
void    ide_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint16_t *edi);
void    ide_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint16_t *edi);

#endif
