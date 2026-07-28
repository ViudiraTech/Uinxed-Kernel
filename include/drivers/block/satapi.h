/*
 *
 *      satapi.h
 *      AHCI SATAPI driver header
 *
 *      2026/7/23 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_AHCI_SATAPI_H_
#define INCLUDE_DRIVERS_AHCI_SATAPI_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* SCSI command opcodes */
#define GPCMD_TEST_UNIT_READY  0x00
#define GPCMD_REQUEST_SENSE    0x03
#define GPCMD_INQUIRY          0x12
#define GPCMD_START_STOP_UNIT  0x1b
#define GPCMD_READ_CAPACITY    0x25
#define GPCMD_READ_10          0x28
#define GPCMD_READ_12          0xa8
#define GPCMD_WRITE_10         0x2a
#define GPCMD_WRITE_12         0xaa
#define GPCMD_READ_CD          0xbe
#define GPCMD_MODE_SENSE       0x1a
#define GPCMD_MODE_SENSE_10    0x5a
#define GPCMD_GET_EVENT_STATUS 0x4a

#define SCSI_SENSE_BUFFER_SIZE 18
#define ATAPI_CDB_LEN          16

/* ATAPI command types */
#define ATAPI_MISC    0
#define ATAPI_READ    1
#define ATAPI_WRITE   2
#define ATAPI_READ_CD 3

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

/* AHCI SATAPI API */
void    ahci_satapi_init(void);
int     ahci_satapi_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, void *buffer);
uint8_t ahci_satapi_send_packet(uint8_t drive, const uint8_t *cdb, uint16_t byte_limit, uint8_t direction, void *buf, size_t *xfer_len);
uint8_t ahci_satapi_test_unit_ready(uint8_t drive);
uint8_t ahci_satapi_read_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size);
int     ahci_satapi_cmd_type(uint8_t opcode);

#endif
