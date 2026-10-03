/*
 *
 *      ata_cmds.h
 *      ATA/ATAPI command codes shared by the PATA and SATA drivers
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ATA_CMDS_H_
#define INCLUDE_ATA_CMDS_H_

/* SCSI/ATAPI packet commands */
#define ATAPI_CDB_LEN          16
#define ATAPI_MISC             0
#define ATAPI_READ             1
#define ATAPI_READ_CD          3
#define ATAPI_WRITE            2
#define GPCMD_GET_EVENT_STATUS 0x4a
#define GPCMD_INQUIRY          0x12
#define GPCMD_MODE_SENSE       0x1a
#define GPCMD_MODE_SENSE_10    0x5a
#define GPCMD_READ_10          0x28
#define GPCMD_READ_12          0xa8
#define GPCMD_READ_CAPACITY    0x25
#define GPCMD_READ_CD          0xbe
#define GPCMD_REQUEST_SENSE    0x03
#define GPCMD_START_STOP_UNIT  0x1b
#define GPCMD_TEST_UNIT_READY  0x00
#define GPCMD_WRITE_10         0x2a
#define GPCMD_WRITE_12         0xaa
#define SCSI_SENSE_BUFFER_SIZE 18

/* ATA commands and IDENTIFY field offsets */
#define ATA_CMD_CACHE_FLUSH     0xe7
#define ATA_CMD_CACHE_FLUSH_EXT 0xea
#define ATA_CMD_IDENTIFY        0xec
#define ATA_CMD_IDENTIFY_PACKET 0xa1
#define ATA_CMD_PACKET          0xa0
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_IDENT_CAPABILITIES  98
#define ATA_IDENT_COMMANDSETS   164
#define ATA_IDENT_CYLINDERS     2
#define ATA_IDENT_DEVICETYPE    0
#define ATA_IDENT_FIELDVALID    106
#define ATA_IDENT_HEADS         6
#define ATA_IDENT_MAX_LBA       120
#define ATA_IDENT_MAX_LBA_EXT   200
#define ATA_IDENT_MODEL         54
#define ATA_IDENT_SECTORS       12
#define ATA_IDENT_SERIAL        20

#endif // INCLUDE_ATA_CMDS_H_
