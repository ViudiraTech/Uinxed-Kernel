/*
 *
 *      blockdev.c
 *      Minimal block device helpers
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/blockdev.h>
#include <drivers/ide.h>
#include <kernel/errno.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

int blockdev_open_ide(uint8_t drive, blockdev_device_t *device)
{
    if (!device) return -EINVAL;
    if (drive > 3 || !ide_devices[drive].reserved) return -ENODEV;
    if (ide_devices[drive].type != IDE_ATA) return -ENOSYS;

    device->drive        = drive;
    device->sector_size  = BLOCKDEV_SECTOR_SIZE;
    device->base_lba     = 0;
    device->sector_count = ide_devices[drive].size;
    return EOK;
}

int blockdev_open_partition(const blockdev_device_t *parent, uint32_t first_lba, uint32_t sector_count, blockdev_device_t *device)
{
    if (!parent || !device) return -EINVAL;
    if (!sector_count) return -EINVAL;
    if ((uint64_t)first_lba + sector_count > parent->sector_count) return -EINVAL;

    device->drive        = parent->drive;
    device->sector_size  = parent->sector_size;
    device->base_lba     = parent->base_lba + first_lba;
    device->sector_count = sector_count;
    return EOK;
}

int blockdev_read_sectors(const blockdev_device_t *device, uint32_t lba, uint32_t count, void *buffer)
{
    uint8_t *ptr = buffer;

    if (!device || !buffer) return -EINVAL;
    if (!count) return EOK;
    if ((uint64_t)lba + count > device->sector_count) return -EINVAL;

    while (count) {
        uint8_t chunk = (count > 255) ? 255 : (uint8_t)count;
        ide_read_sectors(device->drive, chunk, device->base_lba + lba, (uint16_t *)ptr);
        ptr += (size_t)chunk * device->sector_size;
        lba += chunk;
        count -= chunk;
    }

    return EOK;
}

int blockdev_write_sectors(const blockdev_device_t *device, uint32_t lba, uint32_t count, const void *buffer)
{
    const uint8_t *ptr = buffer;

    if (!device || !buffer) return -EINVAL;
    if (!count) return EOK;
    if ((uint64_t)lba + count > device->sector_count) return -EINVAL;

    while (count) {
        uint8_t chunk = (count > 255) ? 255 : (uint8_t)count;
        ide_write_sectors(device->drive, chunk, device->base_lba + lba, (uint16_t *)ptr);
        ptr += (size_t)chunk * device->sector_size;
        lba += chunk;
        count -= chunk;
    }

    return EOK;
}

int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size)
{
    size_t   sector_offset;
    uint32_t start_sector;
    uint32_t sector_count;
    uint8_t *scratch;

    if (!device || !buffer) return -EINVAL;
    if (!size) return EOK;

    start_sector  = (uint32_t)(offset / device->sector_size);
    sector_offset = (size_t)(offset % device->sector_size);
    sector_count  = (uint32_t)((sector_offset + size + device->sector_size - 1) / device->sector_size);
    scratch       = malloc((size_t)sector_count * device->sector_size);
    if (!scratch) return -ENOMEM;

    if (blockdev_read_sectors(device, start_sector, sector_count, scratch) != EOK) {
        free(scratch);
        return -EIO;
    }

    memcpy(buffer, scratch + sector_offset, size);
    free(scratch);
    return EOK;
}

int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size)
{
    size_t   sector_offset;
    uint32_t start_sector;
    uint32_t sector_count;
    uint8_t *scratch;

    if (!device || !buffer) return -EINVAL;
    if (!size) return EOK;

    start_sector  = (uint32_t)(offset / device->sector_size);
    sector_offset = (size_t)(offset % device->sector_size);
    sector_count  = (uint32_t)((sector_offset + size + device->sector_size - 1) / device->sector_size);
    scratch       = malloc((size_t)sector_count * device->sector_size);
    if (!scratch) return -ENOMEM;

    if (blockdev_read_sectors(device, start_sector, sector_count, scratch) != EOK) {
        free(scratch);
        return -EIO;
    }

    memcpy(scratch + sector_offset, buffer, size);
    if (blockdev_write_sectors(device, start_sector, sector_count, scratch) != EOK) {
        free(scratch);
        return -EIO;
    }

    free(scratch);
    return EOK;
}
