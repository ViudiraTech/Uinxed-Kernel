/*
 *
 *      blockdev.c
 *      Block device abstraction layer
 *
 *      2026/5/18 By Rainy101112
 *      2026/7/23 By JiTianYu391 — VFS-style callback registration
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/atapi.h>
#include <drivers/blockdev.h>
#include <drivers/ide.h>
#include <drivers/nvme.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

/* ---- Global ops table ---- */

static blockdev_ops_t  _blk_ops_table[BLOCKDEV_MAX_TYPES];
blockdev_ops_t        *blk_ops_table = _blk_ops_table;
static int             blk_next_id   = 0;

/* Default (empty) ops — returns -ENOSYS for everything */
static int blk_empty_read(const struct blockdev_device *dev, uint32_t lba,
                           uint32_t count, void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -ENOSYS;
}

static int blk_empty_write(const struct blockdev_device *dev, uint32_t lba,
                             uint32_t count, const void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -ENOSYS;
}

static struct blockdev_ops blk_empty_ops = {
    .read_sectors  = blk_empty_read,
    .write_sectors = blk_empty_write,
};

int blockdev_register_type(blockdev_ops_t ops)
{
    if (!ops) return -EINVAL;
    int id = blk_next_id++;
    if (id >= BLOCKDEV_MAX_TYPES) {
        blk_next_id--;
        return -ENOSPC;
    }

    /* Allocate and fill a copy of the ops, substituting empty_func for NULL */
    struct blockdev_ops *copy = malloc(sizeof(struct blockdev_ops));
    if (!copy) {
        blk_next_id--;
        return -ENOMEM;
    }
    memset(copy, 0, sizeof(*copy));

    size_t num_fields = sizeof(struct blockdev_ops) / sizeof(void *);
    for (size_t i = 0; i < num_fields; i++) {
        void *func            = ((void **)ops)[i];
        ((void **)copy)[i]    = func ? func : ((void **)&blk_empty_ops)[i];
    }

    _blk_ops_table[id] = copy;
    return id;
}

/* ---- IDE backend ops ---- */

static int blk_ide_read_sectors(const blockdev_device_t *dev, uint32_t lba,
                                 uint32_t count, void *buffer)
{
    uint8_t *ptr = buffer;

    while (count) {
        uint8_t chunk = (count > 255) ? 255 : (uint8_t)count;
        ide_read_sectors(dev->drive, chunk, dev->base_lba + lba, (uint16_t *)ptr);
        ptr += (size_t)chunk * dev->sector_size;
        lba += chunk;
        count -= chunk;
    }
    return EOK;
}

static int blk_ide_write_sectors(const blockdev_device_t *dev, uint32_t lba,
                                  uint32_t count, const void *buffer)
{
    const uint8_t *ptr = buffer;

    while (count) {
        uint8_t chunk = (count > 255) ? 255 : (uint8_t)count;
        ide_write_sectors(dev->drive, chunk, dev->base_lba + lba, (uint16_t *)ptr);
        ptr += (size_t)chunk * dev->sector_size;
        lba += chunk;
        count -= chunk;
    }
    return EOK;
}

static struct blockdev_ops blk_ide_ops = {
    .read_sectors  = blk_ide_read_sectors,
    .write_sectors = blk_ide_write_sectors,
};

static int blk_ide_type_id = -1;

/* ---- NVMe backend ops (forwarders to nvme.c) ---- */

extern int nvme_read_sectors(const struct blockdev_device *dev, uint32_t lba,
                              uint32_t count, void *buffer);
extern int nvme_write_sectors(const struct blockdev_device *dev, uint32_t lba,
                               uint32_t count, const void *buffer);

static struct blockdev_ops blk_nvme_ops = {
    .read_sectors  = nvme_read_sectors,
    .write_sectors = nvme_write_sectors,
};

static int blk_nvme_type_id = -1;

/* ---- Internal: lazy registration ---- */

static int blk_ide_type(void)
{
    if (blk_ide_type_id < 0) blk_ide_type_id = blockdev_register_type(&blk_ide_ops);
    return blk_ide_type_id;
}

static int blk_nvme_type(void)
{
    if (blk_nvme_type_id < 0) blk_nvme_type_id = blockdev_register_type(&blk_nvme_ops);
    return blk_nvme_type_id;
}

/* ---- Public API ---- */

int blockdev_open_ide(uint8_t drive, blockdev_device_t *device)
{
    if (!device) return -EINVAL;
    if (drive > 3 || !ide_devices[drive].reserved) return -ENODEV;
    if (ide_devices[drive].type != IDE_ATA) return -ENOSYS;

    device->ops_id       = (uint8_t)blk_ide_type();
    device->backend_data = NULL;
    device->drive        = drive;
    device->sector_size  = BLOCKDEV_SECTOR_SIZE;
    device->base_lba     = 0;
    device->sector_count = ide_devices[drive].size;
    return EOK;
}

int blockdev_open_nvme(void *ns, blockdev_device_t *device)
{
    nvme_namespace_t *nvme_ns;

    if (!ns || !device) return -EINVAL;

    nvme_ns = (nvme_namespace_t *)ns;
    if (!nvme_ns->ready) return -ENXIO;

    device->ops_id       = (uint8_t)blk_nvme_type();
    device->backend_data = nvme_ns;
    device->drive        = 0;
    device->sector_size  = nvme_ns->sector_size;
    device->base_lba     = 0;
    device->sector_count = (uint32_t)nvme_ns->total_sectors;
    return EOK;
}

int blockdev_open_partition(const blockdev_device_t *parent, uint32_t first_lba,
                             uint32_t sector_count, blockdev_device_t *device)
int blockdev_open_atapi(uint8_t drive, blockdev_device_t *device)
{
    if (!device) return -EINVAL;
    if (drive > 3 || !atapi_devices[drive].reserved) return -ENODEV;
    if (atapi_devices[drive].type != IDE_ATAPI) return -ENOSYS;

    device->drive        = drive;
    device->sector_size  = atapi_devices[drive].blk_size;
    device->base_lba     = 0;
    device->sector_count = atapi_devices[drive].lba_size;
    return EOK;
}

int blockdev_open_partition(const blockdev_device_t *parent, uint32_t first_lba, uint32_t sector_count, blockdev_device_t *device)
{
    if (!parent || !device) return -EINVAL;
    if (!sector_count) return -EINVAL;
    if ((uint64_t)first_lba + sector_count > parent->sector_count) return -EINVAL;

    /* Copy parent fields including ops_id */
    *device              = *parent;
    device->base_lba     = parent->base_lba + first_lba;
    device->sector_count = sector_count;
    return EOK;
}

int blockdev_read_sectors(const blockdev_device_t *device, uint32_t lba,
                           uint32_t count, void *buffer)
{
    if (!device || !buffer) return -EINVAL;
    if (!count) return EOK;
    if ((uint64_t)lba + count > device->sector_count) return -EINVAL;

    return blk_ops(device, read_sectors)(device, lba, count, buffer);
}

int blockdev_write_sectors(const blockdev_device_t *device, uint32_t lba,
                            uint32_t count, const void *buffer)
{
    if (!device || !buffer) return -EINVAL;
    if (!count) return EOK;
    if ((uint64_t)lba + count > device->sector_count) return -EINVAL;

    return blk_ops(device, write_sectors)(device, lba, count, buffer);
}

int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset,
                         void *buffer, size_t size)
{
    size_t   sector_offset;
    uint32_t start_sector;
    uint32_t sector_count;
    uint8_t *scratch;

    if (!device || !buffer) return -EINVAL;
    if (!size) return EOK;
    if (size > 128 * 1024 * 1024) return -EINVAL;

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

int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset,
                          const void *buffer, size_t size)
{
    size_t   sector_offset;
    uint32_t start_sector;
    uint32_t sector_count;
    uint8_t *scratch;

    if (!device || !buffer) return -EINVAL;
    if (!size) return EOK;
    if (size > 128 * 1024 * 1024) return -EINVAL;

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
