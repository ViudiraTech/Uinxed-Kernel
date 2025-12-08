/*
 *
 *      ide.h
 *      Standard ATA/ATAPI device driver header file
 *
 *      2024/7/11 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IDE_H_
#define INCLUDE_IDE_H_

#include <stdint.h>

#define ATA_CTRL_RESET       0xE0
#define ATA_CTRL_IDLE        0xE1
#define ATA_CTRL_MOTOR       0xE3
#define ATA_CTRL_STANDBY     0xE6
#define ATA_CTRL_IDLE_IMM    0xE7
#define ATA_CTRL_STANDBY_IMM 0xEB

#define ATA_SR_BSY              0x80
#define ATA_SR_DRDY             0x40
#define ATA_SR_DF               0x20
#define ATA_SR_DSC              0x10
#define ATA_SR_DRQ              0x08
#define ATA_SR_CORR             0x04
#define ATA_SR_IDX              0x02
#define ATA_SR_ERR              0x01
#define ATA_ER_BBK              0x80
#define ATA_ER_UNC              0x40
#define ATA_ER_MC               0x20
#define ATA_ER_IDNF             0x10
#define ATA_ER_MCR              0x08
#define ATA_ER_ABRT             0x04
#define ATA_ER_TK0NF            0x02
#define ATA_ER_AMNF             0x01
#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_READ_PIO_EXT    0x24
#define ATA_CMD_READ_DMA        0xc8
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_WRITE_PIO_EXT   0x34
#define ATA_CMD_WRITE_DMA       0xca
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_CACHE_FLUSH     0xe7
#define ATA_CMD_CACHE_FLUSH_EXT 0xea
#define ATA_CMD_PACKET          0xa0
#define ATA_CMD_IDENTIFY_PACKET 0xa1
#define ATA_CMD_IDENTIFY        0xec
#define ATAPI_CMD_READ          0xa8
#define ATAPI_CMD_EJECT         0x1b
#define ATA_IDENT_DEVICETYPE    0
#define ATA_IDENT_CYLINDERS     2
#define ATA_IDENT_HEADS         6
#define ATA_IDENT_SECTORS       12
#define ATA_IDENT_SERIAL        20
#define ATA_IDENT_MODEL         54
#define ATA_IDENT_CAPABILITIES  98
#define ATA_IDENT_FIELDVALID    106
#define ATA_IDENT_MAX_LBA       120
#define ATA_IDENT_COMMANDSETS   164
#define ATA_IDENT_MAX_LBA_EXT   200
#define IDE_ATA                 0x00
#define IDE_ATAPI               0x01

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
        uint16_t base;  // I/O base address
        uint16_t ctrl;  // Control base address
        uint16_t bmide; // Bus Master IDE
        uint8_t  nIEN;  // nIEN (no interrupt)
} ide_channel_registers_t;

typedef struct {
        uint8_t  reserved;     // Drive Status
        uint8_t  channel;      // Master-slave channel
        uint8_t  drive;        // Master-slave drive
        uint16_t type;         // Drive Type
        uint16_t signature;    // Drive Signature
        uint16_t capabilities; // Feature
        uint32_t command_sets; // Supported command sets
        uint32_t size;         // Size in sectors
        uint8_t  model[41];    // Drive Name
} ide_device_t;

/* Initialize IDE */
void init_ide(void);

/* Read a byte of data from the specified register of the IDE device */
uint8_t ide_read(uint8_t channel, uint8_t reg);

/* Write a byte of data to the specified register of the IDE device */
void ide_write(uint8_t channel, uint8_t reg, uint8_t data);

/* Read multiple words of data from the specified register of the IDE device into the buffer */
void ide_read_buffer(uint8_t channel, uint8_t reg, uint8_t *buffer, uint32_t quads);

/* Polling the status of IDE devices */
uint8_t ide_polling(uint8_t channel, uint32_t advanced_check);

/* Read and write ATA devices */
uint8_t ide_ata_access(uint8_t direction, uint8_t drive, uint32_t lba, uint8_t numsects, uint16_t *edi);

/* Reading data from ATAPI devices */
uint8_t ide_atapi_read(uint8_t drive, uint32_t lba, uint8_t numsects, uint16_t *edi);

/* Read multiple sectors from an IDE device */
void ide_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint16_t *edi);

/* Write multiple sectors to an IDE device */
void ide_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint16_t *edi);

#endif // INCLUDE_IDE_H_
