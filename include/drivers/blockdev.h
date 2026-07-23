/*
 *
 *      blockdev.h
 *      Minimal block device helpers
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BLOCKDEV_H_
#define INCLUDE_BLOCKDEV_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define BLOCKDEV_SECTOR_SIZE 512

typedef struct blockdev_device {
        uint8_t  drive;
        uint32_t sector_size;
        uint32_t base_lba;
        uint32_t sector_count;
} blockdev_device_t;

int blockdev_open_ide(uint8_t drive, blockdev_device_t *device);
int blockdev_open_atapi(uint8_t drive, blockdev_device_t *device);
int blockdev_open_partition(const blockdev_device_t *parent, uint32_t first_lba, uint32_t sector_count, blockdev_device_t *device);
int blockdev_read_sectors(const blockdev_device_t *device, uint32_t lba, uint32_t count, void *buffer);
int blockdev_write_sectors(const blockdev_device_t *device, uint32_t lba, uint32_t count, const void *buffer);
int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size);
int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size);

#endif // INCLUDE_BLOCKDEV_H_
