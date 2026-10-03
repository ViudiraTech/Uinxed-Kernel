/*
 *
 *      satapi.h
 *      AHCI SATAPI driver header
 *
 *      2026/7/23 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SATAPI_H_
#define INCLUDE_SATAPI_H_

#include <drivers/block/ata/ata_cmds.h>
#include <drivers/block/ata/sata/ahci.h>
#include <libs/std/stddef.h>

/* ATAPI protocol types for AHCI packet commands */
#define SATAPI_PROT_NODATA 0
#define SATAPI_PROT_PIO    1
#define SATAPI_PROT_DMA    2

/* AHCI SATAPI device structure */
typedef struct {
        uint8_t  reserved;
        uint8_t  port_idx;
        uint8_t  device_idx;
        uint32_t lba_size;
        uint32_t blk_size;
        char     model[41];
} ahci_satapi_device_t;

extern ahci_satapi_device_t ahci_satapi_devices[AHCI_MAX_DEVICES];
extern int                  ahci_satapi_device_count;

/* Initialize AHCI SATAPI devices */
void ahci_satapi_init(void);

/* Read `numsects` sectors from an AHCI SATAPI drive */
int ahci_satapi_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, void *buffer);

/* Execute a SCSI CDB packet on an AHCI SATAPI drive */
uint8_t ahci_satapi_send_packet(uint8_t drive, const uint8_t *cdb, uint16_t byte_limit, uint8_t direction, void *buf, size_t *xfer_len);

/* Send a TEST UNIT READY command */
uint8_t ahci_satapi_test_unit_ready(uint8_t drive);

/* Send READ CAPACITY, returning the LBA count and block size */
uint8_t ahci_satapi_read_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size);

/* Send INQUIRY, returning the vendor and product identification in model */
uint8_t ahci_satapi_inquiry(uint8_t drive, char *model, size_t model_size);

/* Classify a SCSI opcode as misc/read/write/read-cd */
int ahci_satapi_cmd_type(uint8_t opcode);

#endif // INCLUDE_SATAPI_H_
