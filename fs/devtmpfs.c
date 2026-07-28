/*
 *
 *      devtmpfs.c
 *      Device tmpfs population helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/block/ahci.h>
#include <drivers/block/atapi.h>
#include <drivers/block/blockdev.h>
#include <drivers/block/ide.h>
#include <drivers/block/nvme.h>
#include <drivers/block/partition.h>
#include <drivers/char/tty.h>
#include <drivers/gpu/drm_init.h>
#include <drivers/input/evdev.h>
#include <fs/devtmpfs.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <kernel/audio.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>
#include <video/fbdev.h>
#include <video/video.h>

/* ------------------------------------------------------------------ */
/* Character-device registration table                                 */
/* ------------------------------------------------------------------ */

#define DEVTMPFS_MAX_DEVICES     512
#define DEVTMPFS_MAX_BLOCK_NODES (PARTITION_MAX_COUNT + 1)

struct devtmpfs_block_registration {
        char   paths[DEVTMPFS_MAX_BLOCK_NODES][96];
        size_t count;
};

typedef struct devtmpfs_entry {
        char               path[256];
        uint64_t           dev;
        uint64_t           rdev;
        uint16_t           node_type;
        tmpfs_device_ops_t ops;
        vfs_node_t         node;
        bool               active;
} devtmpfs_entry_t;

static devtmpfs_entry_t devtmpfs_table[DEVTMPFS_MAX_DEVICES];
static spinlock_t       devtmpfs_lock = {.lock = 0, .rflags = 0};
static bool             devtmpfs_table_inited;

static void devtmpfs_table_init(void)
{
    if (devtmpfs_table_inited) return;
    memset(devtmpfs_table, 0, sizeof(devtmpfs_table));
    devtmpfs_table_inited = true;
}

int devtmpfs_register_char_device(const char *path, uint64_t dev, uint64_t rdev, uint16_t node_type, const tmpfs_device_ops_t *ops)
{
    vfs_node_t node;
    int        status;
    int        slot = -1;

    if (!path || !ops) return -EINVAL;

    devtmpfs_table_init();

    /* Create parent directories by tokenising the path. */
    {
        char *path_copy = strdup(path);
        char *save      = path_copy;
        char *slash;

        /* Skip leading '/' and walk components. */
        while ((slash = strchr(save + 1, '/'))) {
            *slash = '\0';
            status = vfs_mkdir(path_copy);
            *slash = '/';
            save   = slash;
        }
        free(path_copy);
    }

    /* Create the leaf node. */
    status = vfs_mkfile(path);
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create %s: %d\n", path, status);
        return status;
    }

    node = vfs_open(path);
    if (!node) {
        plogk("devtmpfs: Cannot open %s after creation.\n", path);
        return -ENOENT;
    }

    status = tmpfs_bind_device(node, node_type, ops);
    if (status != EOK) {
        plogk("devtmpfs: Cannot bind %s: %d\n", path, status);
        vfs_close(node);
        return status;
    }

    node->blksz = 1;
    node->dev   = dev;
    node->rdev  = rdev;

    /* Record in the table. */
    spin_lock(&devtmpfs_lock);
    for (int i = 0; i < DEVTMPFS_MAX_DEVICES; i++) {
        if (!devtmpfs_table[i].active) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        strncpy(devtmpfs_table[slot].path, path, sizeof(devtmpfs_table[slot].path) - 1);
        devtmpfs_table[slot].path[sizeof(devtmpfs_table[slot].path) - 1] = '\0';
        devtmpfs_table[slot].dev                                         = dev;
        devtmpfs_table[slot].rdev                                        = rdev;
        devtmpfs_table[slot].node_type                                   = node_type;
        devtmpfs_table[slot].ops                                         = *ops;
        devtmpfs_table[slot].node                                        = node;
        devtmpfs_table[slot].active                                      = true;
    }
    spin_unlock(&devtmpfs_lock);

    if (slot < 0) {
        vfs_namespace_unlink(node);
        vfs_close(node);
        return -ENOSPC;
    }

    plogk("devtmpfs: Registered %s as %s device (dev=%llu, rdev=%llu)\n", path, node_type & file_block ? "block" : "char", dev, rdev);
    /* The registry owns the reference returned by vfs_open(). */
    return 0;
}

int devtmpfs_unregister_char_device(const char *path)
{
    int slot = -1;

    if (!path) return -EINVAL;

    spin_lock(&devtmpfs_lock);
    for (int i = 0; i < DEVTMPFS_MAX_DEVICES; i++) {
        if (devtmpfs_table[i].active && streq(devtmpfs_table[i].path, path)) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spin_unlock(&devtmpfs_lock);
        return -ENOENT;
    }

    devtmpfs_table[slot].active = false;
    vfs_node_t node             = devtmpfs_table[slot].node;
    devtmpfs_table[slot].node   = NULL;
    spin_unlock(&devtmpfs_lock);

    if (node) {
        int status = vfs_namespace_unlink(node);
        vfs_close(node);
        if (status && status != -ENOENT) return status;
    }
    plogk("devtmpfs: Unregistered %s\n", path);
    return 0;
}

static size_t devtmpfs_block_read(void *context, void *buffer, size_t offset, size_t size)
{
    blockdev_device_t *device = context;
    return blockdev_read_bytes(device, offset, buffer, size) == EOK ? size : 0;
}

static size_t devtmpfs_block_write(void *context, const void *buffer, size_t offset, size_t size)
{
    blockdev_device_t *device = context;
    return blockdev_write_bytes(device, offset, buffer, size) == EOK ? size : 0;
}

static void devtmpfs_block_destroy(void *context)
{
    blockdev_release(context);
    free(context);
}

static int devtmpfs_register_one_block(const char *path, const blockdev_device_t *device, uint64_t dev, uint64_t rdev)
{
    blockdev_device_t *context;
    tmpfs_device_ops_t ops = {
        .read    = devtmpfs_block_read,
        .write   = devtmpfs_block_write,
        .destroy = devtmpfs_block_destroy,
    };
    vfs_node_t node;
    int        status;

    if (!path || !device || !device->sector_size || device->sector_count > UINT64_MAX / device->sector_size) return -EINVAL;
    context = malloc(sizeof(*context));
    if (!context) return -ENOMEM;
    *context = *device;
    blockdev_retain(context);
    ops.ctx = context;
    status  = devtmpfs_register_char_device(path, dev, rdev, file_block, &ops);
    if (status != EOK) {
        blockdev_release(context);
        free(context);
        return status;
    }
    node = vfs_open(path);
    if (!node) {
        (void)devtmpfs_unregister_char_device(path);
        return -ENOENT;
    }
    node->blksz = device->sector_size;
    node->size  = device->sector_count * device->sector_size;
    vfs_close(node);
    return EOK;
}

int devtmpfs_register_block_device(const char *path, const blockdev_device_t *device, uint64_t dev, uint64_t rdev, bool scan_partitions,
                                   devtmpfs_block_registration_t **registration)
{
    devtmpfs_block_registration_t *nodes;
    partition_table_t              table;
    int                            status;

    if (!registration) return -EINVAL;
    *registration = NULL;
    nodes         = calloc(1, sizeof(*nodes));
    if (!nodes) return -ENOMEM;
    status = devtmpfs_register_one_block(path, device, dev, rdev);
    if (status != EOK) {
        free(nodes);
        return status;
    }
    strncpy(nodes->paths[0], path, sizeof(nodes->paths[0]) - 1);
    nodes->count = 1;

    if (scan_partitions && partition_scan(device, &table) == EOK) {
        bool separator = path[strlen(path) - 1] >= '0' && path[strlen(path) - 1] <= '9';
        for (size_t i = 0; i < table.count && nodes->count < DEVTMPFS_MAX_BLOCK_NODES; i++) {
            blockdev_device_t       view;
            char                    part_path[96];
            const partition_info_t *part = &table.partitions[i];

            if (blockdev_open_partition(device, part->start_lba, part->sector_count, &view) != EOK) continue;
            if (part->read_only) view.read_only = true;
            snprintf(part_path, sizeof(part_path), "%s%s%u", path, separator ? "p" : "", part->number);
            if (devtmpfs_register_one_block(part_path, &view, dev, rdev + part->number) != EOK) continue;
            strncpy(nodes->paths[nodes->count], part_path, sizeof(nodes->paths[nodes->count]) - 1);
            nodes->count++;
        }
        partition_table_destroy(&table);
    }
    *registration = nodes;
    return EOK;
}

void devtmpfs_unregister_block_device(devtmpfs_block_registration_t *registration)
{
    if (!registration) return;
    while (registration->count) {
        registration->count--;
        (void)devtmpfs_unregister_char_device(registration->paths[registration->count]);
    }
    free(registration);
}

static int devtmpfs_create_block_node(const char *dev_path, const blockdev_device_t *device, uint64_t dev, uint64_t rdev, bool is_partition)
{
    static const tmpfs_device_ops_t block_ops = {
        .read  = devtmpfs_block_read,
        .write = devtmpfs_block_write,
    };
    blockdev_device_t *context;
    tmpfs_device_ops_t ops;
    vfs_node_t         node;
    int                status;

    if (!device || !device->sector_size || device->sector_count > UINT64_MAX / device->sector_size) return -EINVAL;

    status = vfs_mkfile(dev_path);
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create %s: %d\n", dev_path, status);
        return status;
    }

    node = vfs_open(dev_path);
    if (!node) {
        plogk("devtmpfs: Cannot open %s after creation.\n", dev_path);
        return -ENOENT;
    }

    context = malloc(sizeof(*context));
    if (!context) {
        vfs_close(node);
        return -ENOMEM;
    }
    *context = *device;
    ops      = block_ops;
    ops.ctx  = context;
    status   = tmpfs_bind_device(node, file_block, &ops);
    if (status != EOK) {
        free(context);
        vfs_close(node);
        return status;
    }

    node->blksz = device->sector_size;
    node->dev   = dev;
    node->rdev  = rdev;
    node->size  = device->sector_count * device->sector_size;

    if (!is_partition) plogk("devtmpfs: Registered %s as block device.\n", dev_path);

    vfs_close(node);
    return EOK;
}

static void devtmpfs_create_partition_node(const char *dev_prefix, bool use_p_separator, const blockdev_device_t *parent, uint64_t dev,
                                           uint64_t rdev_base, const partition_info_t *partition)
{
    char dev_path[96];
    snprintf(dev_path, sizeof(dev_path), "%s%s%u", dev_prefix, use_p_separator ? "p" : "", partition->number);

    blockdev_device_t view;

    if (blockdev_open_partition(parent, partition->start_lba, partition->sector_count, &view) != EOK) return;
    if (partition->read_only) view.read_only = true;
    if (devtmpfs_create_block_node(dev_path, &view, dev, (rdev_base << 8) | partition->number, true) == EOK)
        plogk("devtmpfs: Registered %s as partition device (start %llu, sectors %llu%s%s)\n", dev_path, (unsigned long long)partition->start_lba,
              (unsigned long long)partition->sector_count, partition->name[0] ? ", name " : "", partition->name);
}

static int devtmpfs_create_partitions(const char *dev_prefix, bool use_p_separator, const blockdev_device_t *device, uint64_t dev,
                                      uint64_t rdev_base)
{
    partition_table_t table;
    int               status;

    status = partition_scan(device, &table);
    if (status == -ENOENT) return 0;
    if (status != EOK) {
        plogk("devtmpfs: Ignoring invalid partition table on %s: %d\n", dev_prefix, status);
        return 0;
    }
    for (size_t i = 0; i < table.count; i++)
        devtmpfs_create_partition_node(dev_prefix, use_p_separator, device, dev, rdev_base, &table.partitions[i]);
    int count = (int)table.count;
    partition_table_destroy(&table);
    return count;
}

static int devtmpfs_create_framebuffer_node(void)
{
    static const tmpfs_device_ops_t fb_device = {
        .read  = video_fb_read,
        .write = video_fb_write,
        .poll  = 0,
        .ioctl = video_fb_ioctl,
        .ctx   = 0,
    };

    int ret = devtmpfs_register_char_device("/dev/fb0", 2, 0, file_fbdev | file_stream, &fb_device);
    if (ret == 0) {
        /* Set the fb node size after registration. */
        vfs_node_t node = vfs_open("/dev/fb0");
        if (node) {
            video_info_t info = video_get_info();
            node->blksz       = sizeof(uint32_t);
            node->size        = info.stride * info.height * sizeof(uint32_t);
            vfs_close(node);
        }
        return 1;
    }
    return 0;
}

static int devtmpfs_create_audio_nodes(void)
{
    int count = 0;

    if (!audio_device_node_count()) return 0;

    for (size_t i = 0; i < audio_device_node_count(); i++) {
        audio_device_node_t *audio_node = audio_get_device_node(i);
        char                 dev_path[64];

        if (!audio_node) continue;

        snprintf(dev_path, sizeof(dev_path), "/dev/snd/%s", audio_node->name);
        if (devtmpfs_register_char_device(dev_path, audio_node->card->id, i, file_audio | file_stream, &audio_node->tmpfs_ops) == 0) count++;
    }
    return count;
}

/* Linux standard TTY device major numbers */
#define TTY_MAJOR     4
#define TTY_AUX_MAJOR 5

typedef struct {
        const char  *path;
        unsigned int major;
        unsigned int minor;
} tty_dev_info_t;

static int devtmpfs_create_tty_nodes(void)
{
    static const tmpfs_device_ops_t tty_device = {
        .read       = tty_dev_read,
        .write      = tty_dev_write,
        .poll       = tty_dev_poll,
        .file_read  = tty_dev_file_read,
        .file_write = tty_dev_file_write,
        .file_poll  = tty_dev_file_poll,
        .file_ioctl = tty_dev_file_ioctl,
        .open       = tty_dev_file_open,
    };

    static const struct {
            const char  *path;
            unsigned int major;
            unsigned int minor;
    } tty_nodes[] = {
        {.path = "/dev/tty0",    .major = TTY_MAJOR,     .minor = 0 },
        {.path = "/dev/ttyS0",   .major = TTY_MAJOR,     .minor = 64},
        {.path = "/dev/console", .major = TTY_AUX_MAJOR, .minor = 1 },
    };

    int count = 0;

    for (size_t i = 0; i < sizeof(tty_nodes) / sizeof(tty_nodes[0]); i++) {
        if (devtmpfs_register_char_device(tty_nodes[i].path, MKDEV(tty_nodes[i].major, tty_nodes[i].minor),
                                          MKDEV(tty_nodes[i].major, tty_nodes[i].minor), file_stream, &tty_device)
            == 0)
            count++;
    }

    static const tmpfs_device_ops_t controlling_tty_device = {
        .open       = tty_ctty_file_open,
        .release    = tty_ctty_file_release,
        .file_read  = tty_ctty_file_read,
        .file_write = tty_ctty_file_write,
        .file_poll  = tty_ctty_file_poll,
        .file_ioctl = tty_ctty_file_ioctl,
    };
    if (devtmpfs_register_char_device("/dev/tty", MKDEV(TTY_AUX_MAJOR, 0), MKDEV(TTY_AUX_MAJOR, 0), file_stream, &controlling_tty_device) == 0)
        count++;
    vfs_node_t node = vfs_open("/dev/tty");
    if (node) {
        node->mode = 0666;
        vfs_close(node);
    }
    return count;
}

static int devtmpfs_create_drm_node(void)
{
    static const tmpfs_device_ops_t drm_device = {
        .read  = 0,
        .write = 0,
        .poll  = 0,
        .ioctl = 0,
        .ctx   = 0,
    };

    struct drm_device *drm_dev = drm_get_singleton();
    if (!drm_dev) return 0;

    return devtmpfs_register_char_device("/dev/dri/card0", 226, 0, file_stream, &drm_device) == 0 ? 1 : 0;
}

void devtmpfs_init(void)
{
    int status;
    int total_devices = 0;

    status = vfs_mkdir("/dev");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev: %d\n", status);
        return;
    }

    /* Register whole disks without issuing media I/O during early boot. */
    /* IDE ATA drives -> /dev/hda, /dev/hdb, ... */
    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;

        char              dev_path[32];
        blockdev_device_t device;
        snprintf(dev_path, sizeof(dev_path), "/dev/hd%c", 'a' + drive);
        if (blockdev_open_ide(drive, &device) == EOK) {
            if (devtmpfs_create_block_node(dev_path, &device, drive, drive, false) == EOK) total_devices++;
            total_devices += devtmpfs_create_partitions(dev_path, false, &device, drive, drive);
        }
    }

    /* AHCI SATA drives -> /dev/sda, /dev/sdb, ... */
    for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATA) continue;

        char              dev_path[32];
        uint8_t           encoded = BLKDEV_AHCI_FLAG | d;
        blockdev_device_t device;
        char              disk_name[16];
        if (blockdev_format_disk_name(disk_name, sizeof(disk_name), d) != EOK) continue;
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", disk_name);
        if (blockdev_open_ahci(d, &device) == EOK) {
            if (devtmpfs_create_block_node(dev_path, &device, encoded, encoded, false) == EOK) total_devices++;
            total_devices += devtmpfs_create_partitions(dev_path, false, &device, encoded, encoded);
        }
    }

    /* ATAPI drives (IDE + AHCI) -> /dev/sr0, /dev/sr1, ... */
    {
        uint8_t sr_idx = 0;

        for (uint8_t drive = 0; drive < 4; drive++) {
            if (!atapi_devices[drive].reserved || atapi_devices[drive].type != IDE_ATAPI) continue;

            char              dev_path[32];
            blockdev_device_t device;
            snprintf(dev_path, sizeof(dev_path), "/dev/sr%u", (unsigned)sr_idx);
            if (blockdev_open_atapi(drive, &device) == EOK && devtmpfs_create_block_node(dev_path, &device, drive, drive, false) == EOK)
                total_devices++;
            sr_idx++;
        }

        for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
            if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATAPI) continue;

            char              dev_path[32];
            blockdev_device_t device;
            snprintf(dev_path, sizeof(dev_path), "/dev/sr%u", (unsigned)sr_idx);
            uint8_t encoded = BLKDEV_AHCI_FLAG | BLKDEV_ATAPI_FLAG | d;
            if (blockdev_open_ahci_atapi(d, &device) == EOK && devtmpfs_create_block_node(dev_path, &device, encoded, encoded, false) == EOK)
                total_devices++;
            sr_idx++;
        }
    }

    /* NVMe namespaces -> /dev/nvme0n1, /dev/nvme0n1p1, ... */
    for (int c = 0; c < nvme_controller_count(); c++) {
        nvme_controller_t *ctrl = nvme_get_controller(c);
        if (!ctrl || !ctrl->initialised) continue;

        for (uint32_t ns = 0; ns < ctrl->num_namespaces; ns++) {
            if (!ctrl->namespaces[ns].ready) continue;

            char              ns_path[64];
            blockdev_device_t device;
            snprintf(ns_path, sizeof(ns_path), "/dev/nvme%dn%u", ctrl->id, ctrl->namespaces[ns].nsid);
            if (blockdev_open_nvme(&ctrl->namespaces[ns], &device) == EOK) {
                if (devtmpfs_create_block_node(ns_path, &device, ctrl->id, ctrl->namespaces[ns].nsid, false) == EOK) total_devices++;
                total_devices += devtmpfs_create_partitions(ns_path, true, &device, ctrl->id, ctrl->namespaces[ns].nsid);
            }
        }
    }

    total_devices += evdev_publish_nodes();
    total_devices += devtmpfs_create_framebuffer_node();
    total_devices += devtmpfs_create_audio_nodes();
    total_devices += devtmpfs_create_tty_nodes();
    total_devices += devtmpfs_create_drm_node();

    plogk("devtmpfs: %d device(s) created in /dev\n", total_devices);
}
