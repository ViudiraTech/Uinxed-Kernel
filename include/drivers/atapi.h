/*
 *
 *      atapi.h
 *      ATAPI (ATA Packet Interface) driver header
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ATAPI_H_
#define INCLUDE_ATAPI_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* ─── SCSI command opcodes (GPCMD_*) ─── */
#define GPCMD_TEST_UNIT_READY      0x00
#define GPCMD_REQUEST_SENSE        0x03
#define GPCMD_FORMAT_UNIT          0x04
#define GPCMD_INQUIRY              0x12
#define GPCMD_MODE_SELECT          0x15
#define GPCMD_MODE_SENSE           0x1a
#define GPCMD_START_STOP_UNIT      0x1b
#define GPCMD_PREVENT_ALLOW_MEDIUM 0x1e
#define GPCMD_READ_FORMAT_CAPS     0x23
#define GPCMD_READ_CAPACITY        0x25
#define GPCMD_READ_10              0x28
#define GPCMD_WRITE_10             0x2a
#define GPCMD_SEEK                 0x2b
#define GPCMD_READ_SUBCHANNEL      0x42
#define GPCMD_READ_TOC_PMA_ATIP    0x43
#define GPCMD_READ_HEADER          0x44
#define GPCMD_PLAY_AUDIO_10        0x45
#define GPCMD_GET_CONFIGURATION    0x46
#define GPCMD_PLAY_AUDIO_MSF       0x47
#define GPCMD_PAUSE_RESUME         0x4b
#define GPCMD_STOP_PLAY_SCAN       0x4e
#define GPCMD_MODE_SENSE_10        0x5a
#define GPCMD_MECHANISM_STATUS     0xbd
#define GPCMD_READ_CD              0xbe
#define GPCMD_READ_12              0xa8
#define GPCMD_WRITE_12             0xaa
#define GPCMD_READ_CD_MSF          0xb9
#define GPCMD_GET_EVENT_STATUS     0x4a
#define GPCMD_BLANK                0xa1
#define GPCMD_SEND_DVD_STRUCTURE   0xad
#define GPCMD_READ_DVD_STRUCTURE   0xad
#define GPCMD_SET_STREAMING        0xb6
#define GPCMD_READ_MASTER_CUE      0x59
#define GPCMD_REPORT_KEY           0xa4
#define GPCMD_SEND_KEY             0xa3

#define GPCMD_SET_READ_AHEAD      0xa7
#define GPCMD_FLUSH_CACHE         0x35
#define GPCMD_WRITE_AND_VERIFY_10 0x2e

#define SCSI_SENSE_BUFFER_SIZE 18

/* SCSI status codes */
#define SAM_STAT_GOOD            0x00
#define SAM_STAT_CHECK_CONDITION 0x02

/* ─── ATAPI CDB length ─── */
#define ATAPI_CDB_LEN 16

/* ─── ATAPI interrupt reason register bits (Sector Count) ─── */
#define ATAPI_IREASON_MASK 0x03
#define ATAPI_COD          0x01 /* 1 = Command, 0 = Data */
#define ATAPI_IO           0x02 /* 1 = To host (read), 0 = To device (write) */

/* ─── ATAPI Features register flags ─── */
#define ATAPI_PKT_DMA 0x01
#define ATAPI_DMADIR  0x04

/* ─── ATAPI command types for atapi_cmd_type() ─── */
#define ATAPI_MISC      0
#define ATAPI_READ      1
#define ATAPI_WRITE     2
#define ATAPI_READ_CD   3
#define ATAPI_PASS_THRU 4

/* ─── ATAPI protocol types ─── */
#define ATAPI_PROT_NODATA 0
#define ATAPI_PROT_PIO    1
#define ATAPI_PROT_DMA    2

/* ─── ATAPI identification offsets (within the 512-byte ID data) ─── */
#define ATAPI_ID_SECTOR_SIZE         0x00
#define ATAPI_ID_MAX_BYTE_COUNT      0x01
#define ATAPI_ID_CAPABILITIES        0x02
#define ATAPI_ID_INTERLEAVE_SIZE     0x02
#define ATAPI_ID_SUBCHANNEL_BUFFER   0x03
#define ATAPI_ID_VENDOR_SPECIFIC     0x04
#define ATAPI_ID_BSIZE               0x04
#define ATAPI_ID_TYPE_HID            0x04
#define ATAPI_ID_PROTOCOL_VERSION    0x05
#define ATAPI_ID_AUDIO_PAUSE_LEN     0x06
#define ATAPI_ID_SERIAL              0x07
#define ATAPI_ID_CACHE_SIZE          0x08
#define ATAPI_ID_BUFFER_SIZE         0x09
#define ATAPI_ID_MODEL               0x0a
#define ATAPI_ID_FIRMWARE            0x17
#define ATAPI_ID_MANAGEMENT_PROTOCOL 0x64

/* ─── ATAPI device structure ─── */
typedef struct {
        uint8_t  reserved;     /* 1 = device present */
        uint8_t  channel;      /* Primary(0) or Secondary(1) */
        uint8_t  drive;        /* Master(0) or Slave(1) */
        uint16_t type;         /* IDE_ATAPI */
        uint16_t pkt_prot;     /* Packet protocol level */
        uint16_t capabilities; /* Device capabilities */
        uint32_t lba_size;     /* Number of sectors (from READ CAPACITY) */
        uint32_t blk_size;     /* Block size (from READ CAPACITY, typically 2048) */
        uint8_t  model[41];    /* Device model string */
} atapi_device_t;

extern atapi_device_t atapi_devices[4];

/* ─── Function declarations ─── */

/* Initialize ATAPI identification for a specific drive */
uint8_t atapi_identify(uint8_t channel, uint8_t drive, uint8_t dev_index);

/* ATAPI soft reset */
void atapi_soft_reset(uint8_t drive);

/* Perform TEST UNIT READY */
uint8_t atapi_test_unit_ready(uint8_t drive);

/* Perform INQUIRY */
uint8_t atapi_inquiry(uint8_t drive, void *buf);

/* Perform READ CAPACITY */
uint8_t atapi_read_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size);

/* Perform REQUEST SENSE */
uint8_t atapi_request_sense(uint8_t drive, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq);

/* Perform START STOP UNIT (eject/load) */
uint8_t atapi_start_stop(uint8_t drive, uint8_t start, uint8_t loej);

/* Send ATAPI packet command */
uint8_t atapi_send_packet(uint8_t drive, const uint8_t *cdb, uint16_t byte_limit, uint8_t direction, void *buf, size_t *xfer_len);

/* Read sectors from ATAPI device (READ 12) */
uint8_t atapi_read(uint8_t drive, uint32_t lba, uint8_t num_sectors, uint16_t *buf);

/* Classify ATAPI command type from SCSI opcode */
int atapi_cmd_type(uint8_t opcode);

#endif /* INCLUDE_ATAPI_H_ */
