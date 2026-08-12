/*
 *
 *      partition.h
 *      MBR and GPT partition table support
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PARTITION_H_
#define INCLUDE_PARTITION_H_

#include <drivers/block/core/blockdev.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define PARTITION_MAX_COUNT        255
#define PARTITION_NAME_SIZE        128
#define PARTITION_UUID_STRING_SIZE 37

typedef enum partition_table_type {
    PARTITION_TABLE_NONE = 0,
    PARTITION_TABLE_MBR,
    PARTITION_TABLE_GPT,
} partition_table_type_t;

typedef struct partition_info {
        uint32_t number;
        uint64_t start_lba;
        uint64_t sector_count;
        uint64_t attributes;
        uint8_t  mbr_type;
        uint8_t  type_guid[16];
        uint8_t  unique_guid[16];
        char     name[PARTITION_NAME_SIZE];
        bool     bootable;
        bool     read_only;
        bool     extended;
} partition_info_t;

typedef struct partition_table {
        partition_table_type_t type;
        partition_info_t      *partitions;
        size_t                 count;
        uint32_t               mbr_disk_signature;
        uint8_t                disk_guid[16];
        bool                   degraded;
        bool                   hybrid;
} partition_table_t;

/* Detect the partition-table format on a disk (MBR or GPT) and populate the table */
int partition_scan(const blockdev_device_t *device, partition_table_t *table);

/* Release the dynamic partition array owned by a partition table */
void partition_table_destroy(partition_table_t *table);

/* Look up a partition by its 1-based number (or GPT entry index) */
const partition_info_t *partition_find(const partition_table_t *table, uint32_t number);

/* Format a partition UUID: canonical GUID for GPT, "disk-signature-N" for MBR */
int partition_format_uuid(const partition_table_t *table, const partition_info_t *partition, char *buffer, size_t size);

#endif // INCLUDE_PARTITION_H_
