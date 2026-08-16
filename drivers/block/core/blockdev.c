/*
 *
 *      blockdev.c
 *      Block device abstraction layer
 *
 *      2026/7/23 By Rainy101112 & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/ata/pata/atapi.h>
#include <drivers/block/ata/pata/ide.h>
#include <drivers/block/ata/sata/ahci.h>
#include <drivers/block/ata/sata/satapi.h>
#include <drivers/block/core/blockdev.h>
#include <drivers/block/core/partition.h>
#include <drivers/block/nvme/nvme.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

/* Global ops table */

static blockdev_ops_t _blk_ops_table[BLOCKDEV_MAX_TYPES];
blockdev_ops_t       *blk_ops_table = _blk_ops_table;
static int            blk_next_id   = 0;

/*
 * Overview
 * blockdev.c is the block-device abstraction shared by IDE, AHCI,
 * SATA-ATAPI and NVMe. It registers per-driver ops, publishes block
 * devices, and implements the generic read/write/ioctl entry points
 * on top of them.
 */

/* Default (empty) ops - returns -ENOSYS for everything */
static int blk_empty_read(const struct blockdev_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return -ENOSYS;
}

static int blk_empty_write(const struct blockdev_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return -ENOSYS;
}

static int blk_empty_flush(const struct blockdev_device *dev)
{
    (void)dev;
    /* A successful flush is a stable-storage guarantee. */
    return -EOPNOTSUPP;
}

static void blk_empty_reference(const struct blockdev_device *dev)
{
    (void)dev;
}

static struct blockdev_ops blk_empty_ops = {
    .read_sectors  = blk_empty_read,
    .write_sectors = blk_empty_write,
    .flush         = blk_empty_flush,
    .retain        = blk_empty_reference,
    .release       = blk_empty_reference,
};

/* Register a block backend ops table, returning its type id. */
int blockdev_register_type(blockdev_ops_t ops)
{
    if (!ops) return -EINVAL;
    int id = blk_next_id++;
    if (id >= BLOCKDEV_MAX_TYPES) {
        blk_next_id--;
        return -ENOSPC;
    }

    struct blockdev_ops *copy = malloc(sizeof(struct blockdev_ops));
    if (!copy) {
        blk_next_id--;
        return -ENOMEM;
    }
    memset(copy, 0, sizeof(*copy));

    size_t num_fields = sizeof(struct blockdev_ops) / sizeof(void *);
    for (size_t i = 0; i < num_fields; i++) {
        void *func         = ((void **)ops)[i];
        ((void **)copy)[i] = func ? func : ((void **)&blk_empty_ops)[i];
    }

    _blk_ops_table[id] = copy;
    return id;
}

/* IDE backend ops */

#if CONFIG_ATA

static int blk_ide_read_sectors(const blockdev_device_t *dev, uint64_t lba, uint32_t count, void *buffer)
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

static int blk_ide_write_sectors(const blockdev_device_t *dev, uint64_t lba, uint32_t count, const void *buffer)
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

static int blk_ide_flush(const blockdev_device_t *dev)
{
    if (!dev) return -EINVAL;
    return ide_flush_cache(dev->drive) == 0 ? EOK : -EIO;
}

static struct blockdev_ops blk_ide_ops = {
    .read_sectors  = blk_ide_read_sectors,
    .write_sectors = blk_ide_write_sectors,
    .flush         = blk_ide_flush,
};

static int blk_ide_type_id = -1;

#endif /* CONFIG_ATA */

/* NVMe backend ops (forwarders to nvme.c) */

#if CONFIG_NVME

static struct blockdev_ops blk_nvme_ops = {
    .read_sectors  = nvme_read_sectors,
    .write_sectors = nvme_write_sectors,
    .flush         = nvme_flush,
};

static int blk_nvme_type_id = -1;

#endif /* CONFIG_NVME */

/* AHCI backend ops */

#if CONFIG_ATA

static int blk_ahci_read_sectors(const blockdev_device_t *dev, uint64_t lba, uint32_t count, void *buffer)
{
    uint8_t *ptr = buffer;

    while (count) {
        uint8_t chunk = (count > 255) ? 255 : (uint8_t)count;
        int     ret   = ahci_read_sectors(dev->drive, chunk, dev->base_lba + lba, ptr);
        if (ret != 0) return -EIO;
        ptr += (size_t)chunk * dev->sector_size;
        lba += chunk;
        count -= chunk;
    }
    return EOK;
}

static int blk_ahci_write_sectors(const blockdev_device_t *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    const uint8_t *ptr = buffer;

    while (count) {
        uint8_t chunk = (count > 255) ? 255 : (uint8_t)count;
        int     ret   = ahci_write_sectors(dev->drive, chunk, dev->base_lba + lba, ptr);
        if (ret != 0) return -EIO;
        ptr += (size_t)chunk * dev->sector_size;
        lba += chunk;
        count -= chunk;
    }
    return EOK;
}

static int blk_ahci_flush(const blockdev_device_t *dev)
{
    if (!dev) return -EINVAL;
    return ahci_flush_cache(dev->drive);
}

static struct blockdev_ops blk_ahci_ops = {
    .read_sectors  = blk_ahci_read_sectors,
    .write_sectors = blk_ahci_write_sectors,
    .flush         = blk_ahci_flush,
};

static int blk_ahci_type_id = -1;

static int blk_ahci_atapi_read_sectors(const blockdev_device_t *dev, uint64_t lba, uint32_t count, void *buffer)
{
    uint8_t *ptr = buffer;
    if (dev->base_lba > UINT32_MAX || lba > UINT32_MAX - dev->base_lba) return -EOVERFLOW;
    while (count) {
        uint8_t chunk = count > 255 ? 255 : (uint8_t)count;
        int     ret   = ahci_satapi_read_sectors(dev->drive, chunk, dev->base_lba + lba, ptr);
        if (ret) return ret;
        ptr += (size_t)chunk * dev->sector_size;
        lba += chunk;
        count -= chunk;
    }
    return EOK;
}

static struct blockdev_ops blk_ahci_atapi_ops = {
    .read_sectors = blk_ahci_atapi_read_sectors,
};

static int blk_ahci_atapi_type_id = -1;

#endif /* CONFIG_ATA */

/* Internal: lazy registration */

#if CONFIG_ATA
static int blk_ide_type(void)
{
    if (blk_ide_type_id < 0) blk_ide_type_id = blockdev_register_type(&blk_ide_ops);
    return blk_ide_type_id;
}
#endif

#if CONFIG_NVME
static int blk_nvme_type(void)
{
    if (blk_nvme_type_id < 0) blk_nvme_type_id = blockdev_register_type(&blk_nvme_ops);
    return blk_nvme_type_id;
}
#endif

#if CONFIG_ATA
static int blk_ahci_type(void)
{
    if (blk_ahci_type_id < 0) blk_ahci_type_id = blockdev_register_type(&blk_ahci_ops);
    return blk_ahci_type_id;
}

static int blk_ahci_atapi_type(void)
{
    if (blk_ahci_atapi_type_id < 0) blk_ahci_atapi_type_id = blockdev_register_type(&blk_ahci_atapi_ops);
    return blk_ahci_atapi_type_id;
}
#endif

/* Public API */

/* Open an IDE ATA drive and fill in a blockdev handle. */
int blockdev_open_ide(uint8_t drive, blockdev_device_t *device)
{
#if CONFIG_ATA
    if (!device) return -EINVAL;
    if (drive > 3 || !ide_devices[drive].reserved) return -ENODEV;
    if (ide_devices[drive].type != IDE_ATA) return -ENOSYS;

    device->ops_id       = (uint8_t)blk_ide_type();
    device->backend_data = NULL;
    device->drive        = drive;
    device->sector_size  = BLOCKDEV_SECTOR_SIZE;
    device->base_lba     = 0;
    device->sector_count = ide_devices[drive].size;
    device->read_only    = false;
    return EOK;
#else
    (void)drive;
    (void)device;
    return -ENODEV;
#endif
}

/* Open an NVMe namespace and fill in a blockdev handle. */
int blockdev_open_nvme(void *ns, blockdev_device_t *device)
{
#if CONFIG_NVME
    nvme_namespace_t *nvme_ns;

    if (!ns || !device) return -EINVAL;

    nvme_ns = (nvme_namespace_t *)ns;
    if (!nvme_ns->ready) return -ENXIO;

    device->ops_id       = (uint8_t)blk_nvme_type();
    device->backend_data = nvme_ns;
    device->drive        = 0;
    device->sector_size  = nvme_ns->sector_size;
    device->base_lba     = 0;
    device->sector_count = nvme_ns->total_sectors;
    device->read_only    = false;
    return EOK;
#else
    (void)ns;
    (void)device;
    return -ENODEV;
#endif
}

/* Open a legacy PATA ATAPI drive and fill in a blockdev handle. */
int blockdev_open_atapi(uint8_t drive, blockdev_device_t *device)
{
#if CONFIG_ATA
    if (!device) return -EINVAL;
    if (drive > 3 || !atapi_devices[drive].reserved) return -ENODEV;
    if (atapi_devices[drive].type != IDE_ATAPI) return -ENOSYS;

    device->ops_id       = (uint8_t)blk_ide_type();
    device->backend_data = NULL;
    device->drive        = drive;
    device->sector_size  = atapi_devices[drive].blk_size;
    device->base_lba     = 0;
    device->sector_count = atapi_devices[drive].lba_size;
    device->read_only    = true;
    return EOK;
#else
    (void)drive;
    (void)device;
    return -ENODEV;
#endif
}

/* Open an AHCI SATA drive and fill in a blockdev handle. */
int blockdev_open_ahci(uint8_t drive, blockdev_device_t *device)
{
#if CONFIG_ATA
    if (!device) return -EINVAL;
    if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved) return -ENODEV;
    if (ahci_devices[drive].type != AHCI_DEV_SATA) return -ENOSYS;

    device->ops_id       = (uint8_t)blk_ahci_type();
    device->backend_data = NULL;
    device->drive        = drive;
    device->sector_size  = ahci_devices[drive].sector_size;
    device->base_lba     = 0;
    device->sector_count = ahci_devices[drive].size;
    device->read_only    = false;
    return EOK;
#else
    (void)drive;
    (void)device;
    return -ENODEV;
#endif
}

/* Open an AHCI SATAPI drive and fill in a blockdev handle. */
int blockdev_open_ahci_atapi(uint8_t drive, blockdev_device_t *device)
{
#if CONFIG_ATA
    if (!device) return -EINVAL;
    if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved) return -ENODEV;
    if (ahci_devices[drive].type != AHCI_DEV_SATAPI) return -ENOSYS;

    device->ops_id       = (uint8_t)blk_ahci_atapi_type();
    device->backend_data = NULL;
    device->drive        = drive;
    device->sector_size  = ahci_devices[drive].sector_size;
    device->base_lba     = 0;
    device->sector_count = ahci_devices[drive].size;
    device->read_only    = true;
    return EOK;
#else
    (void)drive;
    (void)device;
    return -ENODEV;
#endif
}

/* Open a drive by its flat numeric identifier (see BLKDEV_*_FLAG). */
int blockdev_open_drive(uint8_t drive, blockdev_device_t *device)
{
    if (!device) return -EINVAL;

#if CONFIG_NVME
    if (drive & BLKDEV_NVME_FLAG) {
        int                ctrl_idx = drive & BLKDEV_DRIVE_MASK;
        nvme_controller_t *ctrl     = nvme_get_controller(ctrl_idx);
        if (!ctrl || ctrl->num_namespaces < 1) return -ENODEV;
        return blockdev_open_nvme(&ctrl->namespaces[0], device);
    }
#endif
#if CONFIG_ATA
    if (drive & BLKDEV_AHCI_FLAG) {
        uint8_t idx = drive & BLKDEV_DRIVE_MASK;
        if (drive & BLKDEV_ATAPI_FLAG) return blockdev_open_ahci_atapi(idx, device);
        return blockdev_open_ahci(idx, device);
    }
    if (drive & BLKDEV_ATAPI_FLAG) return blockdev_open_atapi(drive & BLKDEV_DRIVE_MASK, device);
    return blockdev_open_ide(drive & BLKDEV_DRIVE_MASK, device);
#else
    return -ENODEV;
#endif
}

/* Parse a leading unsigned decimal integer, advancing the cursor. */
static int parse_uint(const char **cursor, uint32_t *value)
{
    const char *position = *cursor;
    uint32_t    result   = 0;

    if (*position < '0' || *position > '9') return -EINVAL;
    do {
        uint32_t digit = (uint32_t)(*position - '0');
        if (result > (UINT32_MAX - digit) / 10) return -EOVERFLOW;
        result = result * 10 + digit;
        position++;
    } while (*position >= '0' && *position <= '9');
    *cursor = position;
    *value  = result;
    return EOK;
}

/* Parse /dev/sdX, hdX, srX, nvme or ide names into drive/partition ids. */
static int parse_device_name(const char *name, uint8_t *drive, uint32_t *partition, uint32_t *nvme_nsid)
{
    const char *cursor;
    uint32_t    value;
    int         status;

    if (!name || !drive || !partition) return -EINVAL;
    if (!strncmp(name, "/dev/", 5)) name += 5;
    *partition = 0;
    if (nvme_nsid) *nvme_nsid = 0;

    if (!strncmp(name, "sd", 2)) {
        uint32_t encoded_index = 0;
        cursor                 = name + 2;
        if (*cursor < 'a' || *cursor > 'z') return -EINVAL;
        while (*cursor >= 'a' && *cursor <= 'z') {
            uint32_t digit = (uint32_t)(*cursor - 'a' + 1);
            if (encoded_index > (UINT32_MAX - digit) / 26) return -EOVERFLOW;
            encoded_index = encoded_index * 26 + digit;
            cursor++;
        }
        encoded_index--;
        if (encoded_index > BLKDEV_DRIVE_MASK) return -EINVAL;
        *drive = BLKDEV_AHCI_FLAG | (uint8_t)encoded_index;
        if (!*cursor) return EOK;
        status = parse_uint(&cursor, &value);
        if (status != EOK || *cursor || !value) return -EINVAL;
        *partition = value;
        return EOK;
    }

    if (!strncmp(name, "hd", 2)) {
        int idx = name[2] - 'a';
        if (idx < 0 || idx > 3) return -EINVAL;
        *drive = (uint8_t)idx;
        cursor = name + 3;
        if (!*cursor) return EOK;
        status = parse_uint(&cursor, &value);
        if (status != EOK || *cursor || !value) return -EINVAL;
        *partition = value;
        return EOK;
    }

    if (!strncmp(name, "sr", 2)) {
        cursor = name + 2;
        status = parse_uint(&cursor, &value);
        if (status != EOK || *cursor) return -EINVAL;
        if (value >= 4 + AHCI_MAX_DEVICES) return -EINVAL;
        if (value < 4)
            *drive = BLKDEV_ATAPI_FLAG | (uint8_t)value;
        else
            *drive = BLKDEV_AHCI_FLAG | BLKDEV_ATAPI_FLAG | (uint8_t)(value - 4);
        return EOK;
    }

    if (!strncmp(name, "nvme", 4)) {
        uint32_t controller;
        uint32_t namespace_id;

        cursor = name + 4;
        status = parse_uint(&cursor, &controller);
        if (status != EOK || controller > BLKDEV_DRIVE_MASK || *cursor != 'n') return -EINVAL;
        cursor++;
        status = parse_uint(&cursor, &namespace_id);
        if (status != EOK || !namespace_id) return -EINVAL;
        if (*cursor == 'p') {
            cursor++;
            status = parse_uint(&cursor, &value);
            if (status != EOK || !value) return -EINVAL;
            *partition = value;
        }
        if (*cursor) return -EINVAL;
        *drive = BLKDEV_NVME_FLAG | (uint8_t)controller;
        if (nvme_nsid) *nvme_nsid = namespace_id;
        return EOK;
    }

    if (!strncmp(name, "ide", 3)) {
        cursor = name + 3;
        status = parse_uint(&cursor, &value);
        if (status != EOK || *cursor || value > 3) return -EINVAL;
        *drive = (uint8_t)value;
        return EOK;
    }

    return -EINVAL;
}

/* Parse a disk name into a drive id and optional partition number. */
int blockdev_parse_name(const char *name, uint8_t *drive, uint32_t *partition)
{
    return parse_device_name(name, drive, partition, NULL);
}

/* Format a zero-based disk index into a /dev/sdX style name. */
int blockdev_format_disk_name(char *buffer, size_t size, uint32_t index)
{
    char     suffix[8];
    size_t   length = 0;
    uint32_t value  = index;

    if (!buffer || size < 4) return -EINVAL;
    while (1) {
        if (length >= sizeof(suffix) - 1) return -EOVERFLOW;
        suffix[length++] = (char)('a' + value % 26);
        if (value < 26) break;
        value = value / 26 - 1;
    }
    if (size < length + 3) return -ENOSPC;
    buffer[0] = 's';
    buffer[1] = 'd';
    for (size_t i = 0; i < length; i++) buffer[2 + i] = suffix[length - i - 1];
    buffer[2 + length] = '\0';
    return EOK;
}

/* Parse a disk name that must not refer to a partition. */
int blockdev_parse_drive(const char *name, uint8_t *drive)
{
    uint32_t partition;
    int      status = parse_device_name(name, drive, &partition, NULL);

    if (status != EOK) return status;
    return partition == 0 ? EOK : -EINVAL;
}

/* Open a block device by name, resolving partitions when requested. */
int blockdev_open_name(const char *name, blockdev_device_t *device)
{
    partition_table_t       table;
    const partition_info_t *partition_info;
    blockdev_device_t       parent;
    uint8_t                 drive;
    uint32_t                partition;
    uint32_t                namespace_id;
    int                     status;

    if (!device) return -EINVAL;
    status = parse_device_name(name, &drive, &partition, &namespace_id);
    if (status != EOK) return status;

    if (drive & BLKDEV_ATAPI_FLAG) {
#if CONFIG_ATA
        const char *cursor = name;
        uint32_t    optical_index;
        uint32_t    current = 0;

        if (!strncmp(cursor, "/dev/", 5)) cursor += 5;
        cursor += 2;
        status = parse_uint(&cursor, &optical_index);
        if (status != EOK) return status;
        status = -ENODEV;
        for (uint8_t i = 0; i < 4; i++) {
            if (!atapi_devices[i].reserved || atapi_devices[i].type != IDE_ATAPI) continue;
            if (current++ == optical_index) {
                status = blockdev_open_atapi(i, &parent);
                break;
            }
        }
        if (status != EOK) {
            for (uint8_t i = 0; i < AHCI_MAX_DEVICES; i++) {
                if (!ahci_devices[i].reserved || ahci_devices[i].type != AHCI_DEV_SATAPI) continue;
                if (current++ == optical_index) {
                    status = blockdev_open_ahci_atapi(i, &parent);
                    break;
                }
            }
        }
#else
        status = -ENODEV;
#endif
    } else if (drive & BLKDEV_NVME_FLAG) {
#if CONFIG_NVME
        nvme_controller_t *controller = nvme_get_controller(drive & BLKDEV_DRIVE_MASK);
        nvme_namespace_t *namespace   = NULL;
        if (!controller) return -ENODEV;
        for (uint32_t i = 0; i < controller->num_namespaces; i++)
            if (controller->namespaces[i].ready && controller->namespaces[i].nsid == namespace_id) namespace = &controller->namespaces[i];
        if (!namespace) return -ENODEV;
        status = blockdev_open_nvme(namespace, &parent);
#else
        status = -ENODEV;
#endif
    } else {
        status = blockdev_open_drive(drive, &parent);
    }
    if (status != EOK || !partition) {
        if (status == EOK) *device = parent;
        return status;
    }

    status = partition_scan(&parent, &table);
    if (status != EOK) return status;
    partition_info = partition_find(&table, partition);
    if (!partition_info) {
        partition_table_destroy(&table);
        return -ENOENT;
    }
    status = blockdev_open_partition(&parent, partition_info->start_lba, partition_info->sector_count, device);
    if (status == EOK && partition_info->read_only) device->read_only = true;
    partition_table_destroy(&table);
    return status;
}

/* Derive a partition view over a parent device. */
int blockdev_open_partition(const blockdev_device_t *parent, uint64_t first_lba, uint64_t sector_count, blockdev_device_t *device)
{
    if (!parent || !device) return -EINVAL;
    if (!sector_count) return -EINVAL;
    if (first_lba >= parent->sector_count || sector_count > parent->sector_count - first_lba) return -EINVAL;
    if (first_lba > UINT64_MAX - parent->base_lba) return -EOVERFLOW;

    *device              = *parent;
    device->base_lba     = parent->base_lba + first_lba;
    device->sector_count = sector_count;
    return EOK;
}

/* Read `count` sectors starting at `lba` into `buffer` */
int blockdev_read_sectors(const blockdev_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    if (!device) return -EINVAL;
    if (!count) return EOK;
    if (!buffer) return -EINVAL;
    if (lba >= device->sector_count || count > device->sector_count - lba) return -EINVAL;

    return blk_ops(device, read_sectors)(device, lba, count, buffer);
}

/* Write `count` sectors starting at `lba` from `buffer` */
int blockdev_write_sectors(const blockdev_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    if (!device) return -EINVAL;
    if (!count) return EOK;
    if (device->read_only) return -EROFS;
    if (!buffer) return -EINVAL;
    if (lba >= device->sector_count || count > device->sector_count - lba) return -EINVAL;

    return blk_ops(device, write_sectors)(device, lba, count, buffer);
}

/* Commit volatile device write caches, if the backend provides one */
int blockdev_flush(const blockdev_device_t *device)
{
    if (!device) return -EINVAL;
    return blk_ops(device, flush)(device);
}

/* Hold a backend reference for a copied blockdev descriptor */
void blockdev_retain(const blockdev_device_t *device)
{
    if (device) blk_ops(device, retain)(device);
}

/* Drop a backend reference for a copied blockdev descriptor */
void blockdev_release(const blockdev_device_t *device)
{
    if (device) blk_ops(device, release)(device);
}

/* Byte-granularity read (handles partial sectors internally) */
int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size)
{
    size_t   sector_offset;
    uint64_t start_sector;
    uint32_t sector_count;
    uint8_t *scratch;
    uint64_t device_bytes;

    if (!device) return -EINVAL;
    if (!size) return EOK;
    if (!buffer || !device->sector_size || device->sector_count > UINT64_MAX / device->sector_size) return -EINVAL;
    if (size > (size_t)128 * 1024 * 1024) return -EINVAL;
    device_bytes = device->sector_count * device->sector_size;
    if (offset > device_bytes || size > device_bytes - offset) return -EINVAL;

    start_sector  = offset / device->sector_size;
    sector_offset = (size_t)(offset % device->sector_size);
    sector_count  = (uint32_t)((sector_offset + size + device->sector_size - 1) / device->sector_size);
    scratch       = malloc((size_t)sector_count * device->sector_size);
    if (!scratch) return -ENOMEM;

    int status = blockdev_read_sectors(device, start_sector, sector_count, scratch);
    if (status != EOK) {
        plogk("blockdev: Read failed at LBA %llu count %u (offset %llu size %lu): %d\n", (unsigned long long)start_sector, (unsigned)sector_count, (unsigned long long)offset, (unsigned long)size,
              status);
        free(scratch);
        return -EIO;
    }

    memcpy(buffer, scratch + sector_offset, size);
    free(scratch);
    return EOK;
}

/* Byte-granularity write (read-modify-write for partial sectors) */
int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size)
{
    size_t   sector_offset;
    uint64_t start_sector;
    uint32_t sector_count;
    uint8_t *scratch;
    uint64_t device_bytes;

    if (!device) return -EINVAL;
    if (!size) return EOK;
    if (device->read_only) return -EROFS;
    if (!buffer || !device->sector_size || device->sector_count > UINT64_MAX / device->sector_size) return -EINVAL;
    if (size > (size_t)128 * 1024 * 1024) return -EINVAL;
    device_bytes = device->sector_count * device->sector_size;
    if (offset > device_bytes || size > device_bytes - offset) return -EINVAL;

    start_sector  = offset / device->sector_size;
    sector_offset = (size_t)(offset % device->sector_size);
    sector_count  = (uint32_t)((sector_offset + size + device->sector_size - 1) / device->sector_size);
    scratch       = malloc((size_t)sector_count * device->sector_size);
    if (!scratch) return -ENOMEM;

    int status = blockdev_read_sectors(device, start_sector, sector_count, scratch);
    if (status != EOK) {
        plogk("blockdev: Read-modify-write read failed at LBA %llu count %u (offset %llu size %lu): %d\n", (unsigned long long)start_sector, (unsigned)sector_count, (unsigned long long)offset,
              (unsigned long)size, status);
        free(scratch);
        return -EIO;
    }

    memcpy(scratch + sector_offset, buffer, size);
    if (blockdev_write_sectors(device, start_sector, sector_count, scratch) != EOK) {
        plogk("blockdev: Write failed at LBA %llu count %u (offset %llu size %lu)\n", (unsigned long long)start_sector, (unsigned)sector_count, (unsigned long long)offset, (unsigned long)size);
        free(scratch);
        return -EIO;
    }

    free(scratch);
    return EOK;
}
