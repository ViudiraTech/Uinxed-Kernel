/*
 *
 *      atapi.h
 *      ATAPI (ATA Packet Interface) driver header
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_IDE_ATAPI_H_
#define INCLUDE_DRIVERS_IDE_ATAPI_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

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
#define GPCMD_SET_STREAMING        0xb6
#define GPCMD_READ_MASTER_CUE      0x59
#define GPCMD_SET_READ_AHEAD       0xa7
#define GPCMD_FLUSH_CACHE          0x35
#define GPCMD_WRITE_AND_VERIFY_10  0x2e

#define SCSI_SENSE_BUFFER_SIZE   18
#define SAM_STAT_GOOD            0x00
#define SAM_STAT_CHECK_CONDITION 0x02
#define ATAPI_CDB_LEN            16

#define ATAPI_IREASON_MASK 0x03
#define ATAPI_COD          0x01
#define ATAPI_IO           0x02
#define ATAPI_PKT_DMA      0x01
#define ATAPI_DMADIR       0x04

#define ATAPI_MISC      0
#define ATAPI_READ      1
#define ATAPI_WRITE     2
#define ATAPI_READ_CD   3
#define ATAPI_PASS_THRU 4

#define ATAPI_PROT_NODATA 0
#define ATAPI_PROT_PIO    1
#define ATAPI_PROT_DMA    2

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

typedef struct {
        uint8_t  reserved;
        uint8_t  channel;
        uint8_t  drive;
        uint16_t type;
        uint16_t pkt_prot;
        uint16_t capabilities;
        uint32_t lba_size;
        uint32_t blk_size;
        uint8_t  model[41];
} atapi_device_t;

extern atapi_device_t atapi_devices[4];

uint8_t atapi_identify(uint8_t channel, uint8_t drive, uint8_t dev_index);
void    atapi_soft_reset(uint8_t drive);
uint8_t atapi_test_unit_ready(uint8_t drive);
uint8_t atapi_inquiry(uint8_t drive, void *buf);
uint8_t atapi_read_capacity(uint8_t drive, uint32_t *lba_size, uint32_t *blk_size);
uint8_t atapi_request_sense(uint8_t drive, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq);
uint8_t atapi_start_stop(uint8_t drive, uint8_t start, uint8_t loej);
uint8_t atapi_send_packet(uint8_t drive, const uint8_t *cdb, uint16_t byte_limit, uint8_t direction, void *buf, size_t *xfer_len);
uint8_t atapi_read(uint8_t drive, uint32_t lba, uint8_t num_sectors, uint16_t *buf);
int     atapi_cmd_type(uint8_t opcode);

#endif
