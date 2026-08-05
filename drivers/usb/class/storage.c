/*
 *
 *      storage.c
 *      USB Mass Storage Bulk-Only Transport and SCSI disk driver
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/block/blockdev.h>
#include <drivers/usb/class/usb_storage.h>
#include <drivers/usb/core/usb.h>
#include <fs/core/vfs.h>
#include <fs/sysfs/block_sysfs.h>
#include <fs/virtual/devtmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

#define USB_MSC_SUBCLASS_SCSI 0x06
#define USB_MSC_PROTOCOL_BOT  0x50
#define USB_MSC_REQ_RESET     0xff
#define USB_MSC_REQ_MAX_LUN   0xfe
#define USB_MSC_MAX_LUNS      16
#define USB_MSC_MAX_DISKS     256
#define USB_MSC_IO_CHUNK      4096
#define USB_MSC_MAJOR         8

typedef struct usb_storage_device usb_storage_device_t;

typedef struct {
        usb_storage_device_t          *storage;
        blockdev_device_t              blockdev;
        struct block_sysfs_dev        *sysfs;
        devtmpfs_block_registration_t *devtmpfs;
        uint64_t                       sector_count;
        uint32_t                       sector_size;
        uint16_t                       disk_index;
        uint8_t                        lun;
        char                           name[16];
        bool                           read_only;
        bool                           registered;
} usb_storage_lun_t;

struct usb_storage_device {
        usb_interface_t  *interface;
        usb_endpoint_t   *bulk_in;
        usb_endpoint_t   *bulk_out;
        usb_storage_lun_t luns[USB_MSC_MAX_LUNS];
        uint32_t          next_tag;
        volatile uint32_t references;
        volatile bool     io_busy;
        volatile bool     connected;
        uint8_t           lun_count;
};

static int        usb_storage_type = -1;
static bool       usb_storage_disk_ids[USB_MSC_MAX_DISKS];
static spinlock_t usb_storage_disk_lock;

static uint64_t usb_scsi_be64(const uint8_t *data)
{
    return (uint64_t)usb_scsi_be32(data) << 32 | usb_scsi_be32(data + 4);
}

static void usb_storage_lock(usb_storage_device_t *storage)
{
    while (__atomic_test_and_set(&storage->io_busy, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}

static void usb_storage_unlock(usb_storage_device_t *storage)
{
    __atomic_clear(&storage->io_busy, __ATOMIC_RELEASE);
}

static int usb_storage_bulk(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual)
{
    size_t transferred = 0;
    int    status      = usb_bulk_msg(endpoint, buffer, length, &transferred, USB_IO_TIMEOUT_MS);
    if (actual) *actual = transferred;
    return status;
}

static int usb_storage_bulk_exact(usb_endpoint_t *endpoint, void *buffer, size_t length)
{
    size_t actual = 0;
    int    status = usb_storage_bulk(endpoint, buffer, length, &actual);
    if (status != EOK) return status;
    return actual == length ? EOK : -EREMOTEIO;
}

static int usb_storage_reset(usb_storage_device_t *storage)
{
    int status     = usb_control_msg(storage->interface->device, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_MSC_REQ_RESET, 0,
                                     storage->interface->descriptor.interface_number, NULL, 0, USB_CTRL_TIMEOUT_MS);
    int in_status  = usb_clear_halt(storage->bulk_in);
    int out_status = usb_clear_halt(storage->bulk_out);
    if (status != EOK) return status;
    if (in_status != EOK) return in_status;
    return out_status;
}

static int usb_storage_command_locked(usb_storage_device_t *storage, uint8_t lun, const void *command, uint8_t command_length, void *data,
                                      uint32_t data_length, bool input)
{
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;
    uint32_t      tag    = ++storage->next_tag;
    int           status = usb_msc_build_cbw(&cbw, tag, lun, command, command_length, data_length, input);
    if (status != EOK) return status;

    status = usb_storage_bulk_exact(storage->bulk_out, &cbw, sizeof(cbw));
    if (status != EOK) goto recover;
    uint32_t transferred = 0;
    for (uint32_t offset = 0; offset < data_length;) {
        uint32_t chunk = data_length - offset;
        if (chunk > USB_MSC_IO_CHUNK) chunk = USB_MSC_IO_CHUNK;
        size_t          actual        = 0;
        usb_endpoint_t *data_endpoint = input ? storage->bulk_in : storage->bulk_out;
        status                        = usb_storage_bulk(data_endpoint, (uint8_t *)data + offset, chunk, &actual);
        if (actual > chunk) {
            status = -EPROTO;
            goto recover;
        }
        transferred += (uint32_t)actual;
        offset += (uint32_t)actual;
        if (status == -EPIPE) {
            status = usb_clear_halt(data_endpoint);
            if (status != EOK) goto recover;
            break;
        }
        if (status != EOK) goto recover;
        if (actual != chunk) {
            if (!input) {
                status = -EREMOTEIO;
                goto recover;
            }
            break;
        }
    }
    status = usb_storage_bulk_exact(storage->bulk_in, &csw, sizeof(csw));
    if (status == -EPIPE) {
        status = usb_clear_halt(storage->bulk_in);
        if (status == EOK) status = usb_storage_bulk_exact(storage->bulk_in, &csw, sizeof(csw));
    }
    if (status != EOK) goto recover;
    if (csw.signature != USB_MSC_CSW_SIGNATURE || csw.tag != tag || csw.residue > data_length || csw.status > 2) {
        status = -EPROTO;
        goto recover;
    }
    uint32_t expected_residue = data_length - transferred;
    if (csw.status == 0) return csw.residue == expected_residue && csw.residue == 0 ? EOK : -EREMOTEIO;
    if (csw.status == 1) return -EIO;
    status = -EPROTO;

recover:
    (void)usb_storage_reset(storage);
    return status;
}

static int usb_storage_command(usb_storage_lun_t *lun, const void *command, uint8_t command_length, void *data, uint32_t data_length, bool input)
{
    usb_storage_device_t *storage = lun->storage;
    if (!__atomic_load_n(&storage->connected, __ATOMIC_ACQUIRE)) return -ENODEV;
    usb_storage_lock(storage);
    if (!storage->connected) {
        usb_storage_unlock(storage);
        return -ENODEV;
    }
    int status = usb_storage_command_locked(storage, lun->lun, command, command_length, data, data_length, input);
    usb_storage_unlock(storage);
    return status;
}

static int usb_storage_request_sense(usb_storage_lun_t *lun)
{
    uint8_t command[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t response[18];
    return usb_storage_command(lun, command, sizeof(command), response, sizeof(response), true);
}

static int usb_storage_wait_ready(usb_storage_lun_t *lun)
{
    uint8_t command[6] = {0};
    for (unsigned int attempt = 0; attempt < 10; attempt++) {
        int status = usb_storage_command(lun, command, sizeof(command), NULL, 0, true);
        if (status == EOK) return EOK;
        (void)usb_storage_request_sense(lun);
        msleep(100);
    }
    return -ENOMEDIUM;
}

static int usb_storage_capacity(usb_storage_lun_t *lun)
{
    uint8_t command10[10] = {0x25};
    uint8_t response10[8];
    int     status = usb_storage_command(lun, command10, sizeof(command10), response10, sizeof(response10), true);
    if (status != EOK) return status;
    uint32_t last_lba = usb_scsi_be32(response10);
    if (last_lba != UINT32_MAX) return usb_scsi_parse_capacity10(response10, &lun->sector_count, &lun->sector_size);

    uint8_t command16[16] = {0x9e, 0x10};
    uint8_t response16[32];
    command16[13] = sizeof(response16);
    status        = usb_storage_command(lun, command16, sizeof(command16), response16, sizeof(response16), true);
    if (status != EOK) return status;
    uint64_t last_lba16  = usb_scsi_be64(response16);
    uint32_t sector_size = usb_scsi_be32(response16 + 8);
    if (last_lba16 == UINT64_MAX || !sector_size || (sector_size & (sector_size - 1)) || sector_size < 512 || sector_size > 65536)
        return -EINVAL;
    lun->sector_count = last_lba16 + 1;
    lun->sector_size  = sector_size;
    return EOK;
}

static void usb_storage_mode_sense(usb_storage_lun_t *lun)
{
    uint8_t command[6] = {0x1a, 0, 0x3f, 0, 4, 0};
    uint8_t response[4];
    if (usb_storage_command(lun, command, sizeof(command), response, sizeof(response), true) == EOK) lun->read_only = (response[2] & 0x80) != 0;
}

static void usb_scsi_build_rw16(uint8_t command[16], bool write, uint64_t lba, uint32_t blocks)
{
    memset(command, 0, 16);
    command[0] = write ? 0x8a : 0x88;
    for (unsigned int i = 0; i < 8; i++) command[2 + i] = (uint8_t)(lba >> (56 - i * 8));
    command[10] = (uint8_t)(blocks >> 24);
    command[11] = (uint8_t)(blocks >> 16);
    command[12] = (uint8_t)(blocks >> 8);
    command[13] = (uint8_t)blocks;
}

static int usb_storage_rw(const blockdev_device_t *device, uint64_t lba, uint32_t count, void *buffer, bool write)
{
    usb_storage_lun_t *lun = device ? device->backend_data : NULL;
    if (!lun || !lun->storage || !lun->storage->connected) return -ENODEV;
    if (write && lun->read_only) return -EROFS;
    if (lba > UINT64_MAX - device->base_lba) return -EOVERFLOW;
    lba += device->base_lba;
    uint8_t *position = buffer;
    uint32_t maximum  = USB_MSC_IO_CHUNK / lun->sector_size;
    if (!maximum) return -EINVAL;

    while (count) {
        uint32_t blocks = count > maximum ? maximum : count;
        uint32_t bytes  = blocks * lun->sector_size;
        int      status;
        if (lba <= UINT32_MAX && blocks <= UINT16_MAX && lba + blocks - 1 <= UINT32_MAX) {
            uint8_t command[10];
            usb_scsi_build_rw10(command, write, (uint32_t)lba, (uint16_t)blocks, false);
            status = usb_storage_command(lun, command, sizeof(command), position, bytes, !write);
        } else {
            uint8_t command[16];
            usb_scsi_build_rw16(command, write, lba, blocks);
            status = usb_storage_command(lun, command, sizeof(command), position, bytes, !write);
        }
        if (status != EOK) return status;
        position += bytes;
        lba += blocks;
        count -= blocks;
    }
    return EOK;
}

static int usb_storage_read(const blockdev_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    return usb_storage_rw(device, lba, count, buffer, false);
}

static int usb_storage_write(const blockdev_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    return usb_storage_rw(device, lba, count, (void *)buffer, true);
}

static int usb_storage_flush(const blockdev_device_t *device)
{
    usb_storage_lun_t *lun         = device ? device->backend_data : NULL;
    uint8_t            command[10] = {0x35};
    if (!lun || !lun->storage) return -ENODEV;
    if (lun->read_only) return EOK;
    return usb_storage_command(lun, command, sizeof(command), NULL, 0, false);
}

static void usb_storage_retain(const blockdev_device_t *device)
{
    usb_storage_lun_t *lun = device ? device->backend_data : NULL;
    if (lun && lun->storage) __atomic_add_fetch(&lun->storage->references, 1, __ATOMIC_RELAXED);
}

static void usb_storage_release(const blockdev_device_t *device)
{
    usb_storage_lun_t *lun = device ? device->backend_data : NULL;
    if (lun && lun->storage && __atomic_sub_fetch(&lun->storage->references, 1, __ATOMIC_ACQ_REL) == 0) free(lun->storage);
}

static struct blockdev_ops usb_storage_ops = {
    .read_sectors  = usb_storage_read,
    .write_sectors = usb_storage_write,
    .flush         = usb_storage_flush,
    .retain        = usb_storage_retain,
    .release       = usb_storage_release,
};

static int usb_storage_allocate_name(char *name, size_t size, uint16_t *disk_index)
{
    spin_lock(&usb_storage_disk_lock);
    for (uint16_t index = 0; index < USB_MSC_MAX_DISKS; index++) {
        if (usb_storage_disk_ids[index] || blockdev_format_disk_name(name, size, index) != EOK) continue;
        char path[32];
        snprintf(path, sizeof(path), "/dev/%s", name);
        vfs_node_t existing = vfs_open(path);
        if (existing) {
            vfs_close(existing);
            continue;
        }
        usb_storage_disk_ids[index] = true;
        *disk_index                 = index;
        spin_unlock(&usb_storage_disk_lock);
        return EOK;
    }
    spin_unlock(&usb_storage_disk_lock);
    return -ENOSPC;
}

static void usb_storage_release_name(uint16_t index)
{
    if (index >= USB_MSC_MAX_DISKS) return;
    spin_lock(&usb_storage_disk_lock);
    usb_storage_disk_ids[index] = false;
    spin_unlock(&usb_storage_disk_lock);
}

static int usb_storage_register_lun(usb_storage_lun_t *lun)
{
    char path[32];
    int  status = usb_storage_allocate_name(lun->name, sizeof(lun->name), &lun->disk_index);
    if (status != EOK) return status;
    lun->blockdev.ops_id       = (uint8_t)usb_storage_type;
    lun->blockdev.backend_data = lun;
    lun->blockdev.drive        = lun->lun;
    lun->blockdev.sector_size  = lun->sector_size;
    lun->blockdev.base_lba     = 0;
    lun->blockdev.sector_count = lun->sector_count;
    lun->blockdev.read_only    = lun->read_only;
    snprintf(path, sizeof(path), "/dev/%s", lun->name);
    uint64_t devt = MKDEV(USB_MSC_MAJOR, (uint32_t)lun->disk_index * 16);
    status        = devtmpfs_register_block_device(path, &lun->blockdev, devt, devt, true, &lun->devtmpfs);
    if (status != EOK) goto fail_name;
    status = block_sysfs_register_device(lun->name, &lun->blockdev, true, &lun->sysfs);
    if (status != EOK) goto fail_devtmpfs;
    lun->registered = true;
    return EOK;

fail_devtmpfs:
    devtmpfs_unregister_block_device(lun->devtmpfs);
    lun->devtmpfs = NULL;
fail_name:
    usb_storage_release_name(lun->disk_index);
    return status;
}

static void usb_storage_unregister_lun(usb_storage_lun_t *lun)
{
    if (!lun->registered) return;
    block_sysfs_unregister_device(lun->sysfs);
    devtmpfs_unregister_block_device(lun->devtmpfs);
    usb_storage_release_name(lun->disk_index);
    lun->sysfs      = NULL;
    lun->devtmpfs   = NULL;
    lun->registered = false;
}

int usb_storage_probe(usb_interface_t *interface)
{
#if CONFIG_USB_STORAGE
    if (!interface || interface->driver_data || interface->descriptor.interface_class != USB_CLASS_MASS_STORAGE
        || interface->descriptor.interface_subclass != USB_MSC_SUBCLASS_SCSI || interface->descriptor.interface_protocol != USB_MSC_PROTOCOL_BOT)
        return -ENODEV;
    usb_endpoint_t *bulk_in  = usb_find_endpoint(interface, USB_ENDPOINT_XFER_BULK, true);
    usb_endpoint_t *bulk_out = usb_find_endpoint(interface, USB_ENDPOINT_XFER_BULK, false);
    if (!bulk_in || !bulk_out) return -ENODEV;
    if (usb_storage_type < 0) {
        usb_storage_type = blockdev_register_type(&usb_storage_ops);
        if (usb_storage_type < 0) return usb_storage_type;
    }
    usb_storage_device_t *storage = calloc(1, sizeof(*storage));
    if (!storage) return -ENOMEM;
    storage->interface  = interface;
    storage->bulk_in    = bulk_in;
    storage->bulk_out   = bulk_out;
    storage->references = 1;
    storage->connected  = true;

    uint8_t max_lun = 0;
    if (usb_control_msg(interface->device, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_MSC_REQ_MAX_LUN, 0,
                        interface->descriptor.interface_number, &max_lun, sizeof(max_lun), USB_CTRL_TIMEOUT_MS)
        != EOK)
        max_lun = 0;
    if (max_lun >= USB_MSC_MAX_LUNS) max_lun = USB_MSC_MAX_LUNS - 1;
    for (uint8_t index = 0; index <= max_lun; index++) {
        usb_storage_lun_t *lun = &storage->luns[index];
        lun->storage           = storage;
        lun->lun               = index;
        if (usb_storage_wait_ready(lun) != EOK || usb_storage_capacity(lun) != EOK) continue;
        usb_storage_mode_sense(lun);
        if (usb_storage_register_lun(lun) != EOK) continue;
        storage->lun_count++;
        plogk("usb-storage: %s: %llu sectors, %u-byte logical blocks%s\n", lun->name, (unsigned long long)lun->sector_count, lun->sector_size,
              lun->read_only ? ", read-only" : "");
    }
    if (!storage->lun_count) {
        storage->connected = false;
        free(storage);
        return -ENOMEDIUM;
    }
    interface->driver_data = storage;
    return EOK;
#else
    (void)interface;
    return -ENOSYS;
#endif
}

void usb_storage_disconnect(usb_interface_t *interface)
{
#if CONFIG_USB_STORAGE
    usb_storage_device_t *storage = interface ? interface->driver_data : NULL;
    if (!storage) return;
    __atomic_store_n(&storage->connected, false, __ATOMIC_RELEASE);
    usb_storage_lock(storage);
    usb_storage_unlock(storage);
    interface->driver_data = NULL;
    for (size_t i = 0; i < USB_MSC_MAX_LUNS; i++) usb_storage_unregister_lun(&storage->luns[i]);
    if (__atomic_sub_fetch(&storage->references, 1, __ATOMIC_ACQ_REL) == 0) free(storage);
#else
    (void)interface;
#endif
}
