/*
 *
 *      blockdev.h
 *      Block device abstraction layer
 *
 *      2026/5/18 By Rainy101112
 *      2026/7/23 By JiTianYu391 — VFS-style callback registration
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BLOCKDEV_H_
#define INCLUDE_BLOCKDEV_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define BLOCKDEV_SECTOR_SIZE 512

/* Ops dispatch macro — mirrors VFS callbackof() */
#define blk_ops(dev, _name_) (blk_ops_table[(dev)->ops_id]->_name_)

/* Forward declaration */
struct blockdev_device;

/* Block device operations callback table */
typedef struct blockdev_ops {
    int (*read_sectors)(const struct blockdev_device *dev, uint32_t lba,
                         uint32_t count, void *buf);
    int (*write_sectors)(const struct blockdev_device *dev, uint32_t lba,
                          uint32_t count, const void *buf);
} *blockdev_ops_t;

typedef struct blockdev_device {
    uint8_t  ops_id;       /* index into blk_ops_table */
    void    *backend_data; /* backend-specific context (NULL for IDE, namespace ptr for NVMe) */
    uint8_t  drive;        /* IDE drive number (kept for backward compat) */
    uint32_t sector_size;
    uint32_t base_lba;
    uint32_t sector_count;
} blockdev_device_t;

/* Global ops table — populated by driver init */
extern blockdev_ops_t *blk_ops_table;
#define BLOCKDEV_MAX_TYPES 16

/* Register a block device backend. Returns type id (>=1), or negative errno. */
int blockdev_register_type(blockdev_ops_t ops);

/* ---- Opening devices ---- */

/* Open an IDE disk (drive 0-3) */
int blockdev_open_ide(uint8_t drive, blockdev_device_t *device);
<<<<<<< HEAD

/* Open an NVMe namespace. `ns` is an opaque pointer to an nvme_namespace_t. */
int blockdev_open_nvme(void *ns, blockdev_device_t *device);

/* Create a partition view of a parent block device.
 * Copies the parent's ops_id and wraps base_lba/sector_count. */
int blockdev_open_partition(const blockdev_device_t *parent, uint32_t first_lba,
                             uint32_t sector_count, blockdev_device_t *device);

/* ---- I/O ---- */

/* Read `count` sectors starting at `lba` into `buffer` */
int blockdev_read_sectors(const blockdev_device_t *device, uint32_t lba,
                           uint32_t count, void *buffer);

/* Write `count` sectors starting at `lba` from `buffer` */
int blockdev_write_sectors(const blockdev_device_t *device, uint32_t lba,
                            uint32_t count, const void *buffer);

/* Byte-granularity read (handles partial sectors internally) */
int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset,
                         void *buffer, size_t size);

/* Byte-granularity write (read-modify-write for partial sectors) */
int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset,
                          const void *buffer, size_t size);
=======
int blockdev_open_atapi(uint8_t drive, blockdev_device_t *device);
int blockdev_open_partition(const blockdev_device_t *parent, uint32_t first_lba, uint32_t sector_count, blockdev_device_t *device);
int blockdev_read_sectors(const blockdev_device_t *device, uint32_t lba, uint32_t count, void *buffer);
int blockdev_write_sectors(const blockdev_device_t *device, uint32_t lba, uint32_t count, const void *buffer);
int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size);
int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size);
>>>>>>> 2ac679cce3884f3b51bb89387aadc35ab7be6445

#endif // INCLUDE_BLOCKDEV_H_
