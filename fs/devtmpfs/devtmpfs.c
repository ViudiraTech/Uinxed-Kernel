/*
 *
 *      devtmpfs.c
 *      Device tmpfs population helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/block/core/gendisk.h>
#include <drivers/block/core/partition.h>
#include <drivers/char/chrdev.h>
#include <drivers/gpu/fbdev/fbdev.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/input/evdev/evdev.h>
#include <drivers/time/rtc.h>
#include <drivers/tty/pty/pty.h>
#include <drivers/tty/tty_driver.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/uaccess.h>

/* Character-device registration table */

typedef struct devtmpfs_entry {
        char               path[256];
        dev_t              dev;
        dev_t              rdev;
        uint16_t           node_type;
        tmpfs_device_ops_t ops;
        vfs_node_t         node;
        bool               active;
} devtmpfs_entry_t;

static devtmpfs_entry_t devtmpfs_table[CONFIG_DEVTMPFS_MAX_DEVICES];
static spinlock_t       devtmpfs_lock = {.lock = 0, .rflags = 0};
static bool             devtmpfs_table_inited;
static bool             devtmpfs_populated; // true once devtmpfs_init() finishes its one-time population

/* /proc block-device export helpers */

typedef void (*devtmpfs_block_walk_fn)(const char *name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque);

typedef struct {
        devtmpfs_block_walk_fn fn;
        void                  *opaque;
} devtmpfs_partition_walk_t;

typedef struct {
        uint32_t major;
        char     cls[8];
} devtmpfs_class_entry_t;

typedef struct {
        char                  *buf;
        size_t                 cap;
        size_t                 off;
        devtmpfs_class_entry_t seen[16];
        size_t                 seen_count;
} devtmpfs_devices_ctx_t;

typedef struct {
        char  *buf;
        size_t cap;
        size_t off;
} devtmpfs_block_file_ctx_t;

/* Initialize the device registration table once. */
static void devtmpfs_table_init(void)
{
    if (devtmpfs_table_inited) return;
    memset(devtmpfs_table, 0, sizeof(devtmpfs_table));
    devtmpfs_table_inited = true;
}

/* Register a character or block device node under /dev. */
int devtmpfs_register_char_device(const char *path, dev_t dev, dev_t rdev, uint16_t node_type, const tmpfs_device_ops_t *ops)
{
    vfs_node_t node;
    int        status;
    int        slot = -1;

    if (!path || !ops) return -EINVAL;
    devtmpfs_table_init();

    /* Create parent directories by tokenising the path. */
    {
        char *path_copy = strdup(path);
        if (!path_copy) return -ENOMEM;
        char *save = path_copy;
        char *slash;

        /* Skip leading '/' and walk components. */
        while ((slash = strchr(save + 1, '/'))) {
            *slash = '\0';
            (void)vfs_mkdir(path_copy);
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
    for (int i = 0; i < CONFIG_DEVTMPFS_MAX_DEVICES; i++) {
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
        /* The caller still owns the context, so the node must not release it. */
        (void)tmpfs_unbind_device(node);
        vfs_namespace_unlink(node);
        vfs_close(node);
        return -ENOSPC;
    }
    if (!devtmpfs_populated) plogk("devtmpfs: Registered %s as %s device (dev=%u:%u, rdev=%u:%u)\n", path, node_type & file_block ? "block" : "char", MAJOR(dev), MINOR(dev), MAJOR(rdev), MINOR(rdev));

    /* The registry owns the reference returned by vfs_open(). */
    return 0;
}

/* Unregister a device node and remove it from the namespace. */
int devtmpfs_unregister_char_device(const char *path)
{
    int slot = -1;

    if (!path) return -EINVAL;

    spin_lock(&devtmpfs_lock);
    for (int i = 0; i < CONFIG_DEVTMPFS_MAX_DEVICES; i++) {
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
    return 0;
}

/* tmpfs device callback: read from a block device. */
static size_t devtmpfs_block_read(void *context, void *buffer, size_t offset, size_t size)
{
    blockdev_device_t *device = context;
    return blockdev_read_bytes(device, offset, buffer, size) == EOK ? size : 0;
}

/* tmpfs device callback: write to a block device. */
static size_t devtmpfs_block_write(void *context, const void *buffer, size_t offset, size_t size)
{
    blockdev_device_t *device = context;
    return blockdev_write_bytes(device, offset, buffer, size) == EOK ? size : 0;
}

/* Geometry queries for whole disks and partition nodes. */
static int devtmpfs_block_ioctl(void *context, size_t request, void *argument)
{
    blockdev_device_t *device = context;
    if (!device) return -ENODEV;
    if (!blockdev_geometry_valid(device)) return -EOVERFLOW;

    uint64_t bytes = device->sector_count * device->sector_size;
    switch (request) {
        case BLKROGET : {
            int value = device->read_only ? 1 : 0;
            return copy_to_user(argument, &value, sizeof(value)) ? -EFAULT : EOK;
        }
        case BLKSSZGET :
        case BLKPBSZGET : {
            int value = (int)device->sector_size;
            return copy_to_user(argument, &value, sizeof(value)) ? -EFAULT : EOK;
        }
        case BLKGETSIZE : {
            unsigned long sectors = (unsigned long)(bytes / BLOCKDEV_SECTOR_SIZE);
            return copy_to_user(argument, &sectors, sizeof(sectors)) ? -EFAULT : EOK;
        }
        case BLKGETSIZE64 :
            return copy_to_user(argument, &bytes, sizeof(bytes)) ? -EFAULT : EOK;
        case CDROM_GET_CAPABILITY :
            return device->optical ? 0 : -ENOTTY;
        default :
            return -ENOTTY;
    }
}

/* Release the retained device reference held by a block node. */
static void devtmpfs_block_destroy(void *context)
{
    blockdev_release(context);
    free(context);
}

/* Allocate the context and callbacks for one block device descriptor. */
static int devtmpfs_block_ops_init(tmpfs_device_ops_t *ops, const blockdev_device_t *device)
{
    blockdev_device_t *context = malloc(sizeof(*context));

    if (!context) return -ENOMEM;
    *context = *device;
    blockdev_retain(context);

    memset(ops, 0, sizeof(*ops));
    ops->read    = devtmpfs_block_read;
    ops->write   = devtmpfs_block_write;
    ops->ioctl   = devtmpfs_block_ioctl;
    ops->destroy = devtmpfs_block_destroy;
    ops->ctx     = context;
    return EOK;
}

/* Drop a callback set built by devtmpfs_block_ops_init(). */
static void devtmpfs_block_ops_destroy(tmpfs_device_ops_t *ops)
{
    ops->destroy(ops->ctx);
    ops->destroy = NULL;
    ops->ctx     = NULL;
}

/* Open a block device node, returning a retained device descriptor. */
int devtmpfs_open_block_device(const char *path, blockdev_device_t *device)
{
    vfs_node_t node;

    if (!path || !device) return -EINVAL;

    node = vfs_open(path);
    if (!node) return -ENOENT;
    if (!(node->type & file_block)) {
        vfs_close(node);
        return -ENOTBLK;
    }

    tmpfs_file_t *handle = node->handle;
    if (!handle || !(handle->node_type & file_block) || handle->device.read != devtmpfs_block_read || !handle->device.ctx) {
        vfs_close(node);
        return -ENODEV;
    }

    *device = *(blockdev_device_t *)handle->device.ctx;
    blockdev_retain(device);
    vfs_close(node);
    return EOK;
}

/* Register one block device node bound to a retained device descriptor. */
static int devtmpfs_register_one_block(const char *path, const blockdev_device_t *device, dev_t dev, dev_t rdev)
{
    tmpfs_device_ops_t ops;
    vfs_node_t         node;
    int                status;

    if (!path || !blockdev_geometry_valid(device)) return -EINVAL;
    status = devtmpfs_block_ops_init(&ops, device);
    if (status != EOK) return status;
    status = devtmpfs_register_char_device(path, dev, rdev, file_block, &ops);
    if (status != EOK) {
        devtmpfs_block_ops_destroy(&ops);
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

/* Register a whole disk plus its partitions, tracking the created paths. */
int devtmpfs_register_block_device(const char *path, const blockdev_device_t *device, dev_t dev, dev_t rdev, bool scan_partitions, bool use_p_separator, devtmpfs_block_registration_t **registration)
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
        bool separator = use_p_separator;

        if (table.degraded) plogk("gendisk: %s: Primary GPT header unusable, using the backup.\n", path);
        for (size_t i = 0; i < table.count && nodes->count < DEVTMPFS_MAX_BLOCK_NODES; i++) {
            blockdev_device_t       view;
            char                    part_path[96];
            const partition_info_t *part = &table.partitions[i];

            if (blockdev_open_partition(device, part->start_lba, part->sector_count, &view) != EOK) continue;
            if (part->read_only) view.read_only = true;
            (void)snprintf(part_path, sizeof(part_path), "%s%s%u", path, separator ? "p" : "", part->number);
            if (devtmpfs_register_one_block(part_path, &view, dev, rdev + part->number) != EOK) continue;
            strncpy(nodes->paths[nodes->count], part_path, sizeof(nodes->paths[nodes->count]) - 1);
            nodes->count++;
        }
        partition_table_destroy(&table);
    }
    *registration = nodes;
    return EOK;
}

/* Unregister every node tracked by a block device registration. */
void devtmpfs_unregister_block_device(devtmpfs_block_registration_t *registration)
{
    if (!registration) return;
    while (registration->count) {
        registration->count--;
        (void)devtmpfs_unregister_char_device(registration->paths[registration->count]);
    }
    free(registration);
}

/*
 * Resolve a whole disk or partition by device number.  On success @device takes
 * over the backend reference held by the disk snapshot, so the caller releases
 * it with blockdev_release(); @disk_dev receives the whole-disk device number
 * the published node reports.
 */
static int devtmpfs_block_lookup(uint32_t major, uint32_t minor, blockdev_device_t *device, dev_t *disk_dev)
{
    for (int index = 0;; index++) {
        gendisk_t disk;
        int       status = -ENODEV;

        if (!block_disk_snapshot(index, &disk)) break;
        if (disk.major != major) {
            blockdev_release(&disk.device);
            continue;
        }

        if (disk.minor_base == minor) {
            *device   = disk.device;
            *disk_dev = MKDEV(major, disk.minor_base);
            status    = EOK;
        } else if (disk.scan_partitions) {
            partition_table_t table;
            if (partition_scan(&disk.device, &table) == EOK) {
                for (size_t p = 0; p < table.count && status != EOK; p++) {
                    const partition_info_t *part = &table.partitions[p];
                    blockdev_device_t       view;

                    if (disk.minor_base + part->number != minor) continue;
                    if (blockdev_open_partition(&disk.device, part->start_lba, part->sector_count, &view) != EOK) continue;
                    if (part->read_only) view.read_only = true;
                    *device   = view;
                    *disk_dev = MKDEV(major, disk.minor_base);
                    status    = EOK;
                }
                partition_table_destroy(&table);
            }
        }

        if (status == EOK) return EOK;
        blockdev_release(&disk.device);
    }
    return -ENODEV;
}

/*
 * Find the callbacks published for a device number.  An entry that owns
 * per-node state is skipped: its context belongs to one node and cannot be
 * shared with a second one.
 */
static bool devtmpfs_lookup_published(dev_t rdev, uint16_t *node_type, tmpfs_device_ops_t *ops)
{
    bool found = false;

    spin_lock(&devtmpfs_lock);
    for (int i = 0; i < CONFIG_DEVTMPFS_MAX_DEVICES; i++) {
        if (!devtmpfs_table[i].active || devtmpfs_table[i].rdev != rdev) continue;
        if (devtmpfs_table[i].ops.destroy) continue; // the entry owns per-node state

        *node_type = devtmpfs_table[i].node_type;
        *ops       = devtmpfs_table[i].ops;
        found      = true;
        break;
    }
    spin_unlock(&devtmpfs_lock);
    return found;
}

/* Look up the driver bound to a device number, published or registered. */
static bool devtmpfs_lookup_device(dev_t rdev, uint16_t *node_type, tmpfs_device_ops_t *ops)
{
    cdev_t cdev;

    if (devtmpfs_lookup_published(rdev, node_type, ops)) return true;
    if (!chrdev_lookup(MAJOR(rdev), MINOR(rdev), &cdev)) return false;
    *node_type = cdev.node_type;
    *ops       = cdev.ops;
    return true;
}

/*
 * Open callback of a node whose driver is resolved per open.  A device node
 * created by mknod() holds no driver state of its own, so each open looks the
 * device number up again and binds what it finds there; a device that has gone
 * away fails the lookup instead of opening through a stale context.
 */
static int devtmpfs_deferred_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    tmpfs_device_ops_t ops;
    tmpfs_dev_open_t   open;
    uint16_t           node_type;

    if (!node) return -ENODEV;
    if (!devtmpfs_lookup_device(node->rdev, &node_type, &ops)) return -ENXIO;

    /*
     * Keep the resolver installed so the next open resolves again, and never
     * take ownership of a context this node did not create.
     */
    open        = ops.open;
    ops.open    = devtmpfs_deferred_open;
    ops.destroy = NULL;
    if (tmpfs_bind_device(node, node_type, &ops) != EOK) return -ENODEV;
    if (!open) {
        if (private_data) *private_data = NULL;
        return EOK;
    }
    return open(node, flags, private_data);
}

/* Create a device node for a device number, bound to its registered driver. */
int devtmpfs_mknod(const char *path, uint16_t mode, dev_t dev)
{
    uint32_t           major        = MAJOR(dev);
    uint32_t           minor        = MINOR(dev);
    bool               block        = (mode & 0170000) == 0060000;
    tmpfs_device_ops_t ops          = {.open = devtmpfs_deferred_open};
    uint16_t           node_type    = block ? file_block : file_stream;
    dev_t              node_dev     = dev;
    uint32_t           sector_size  = 0;
    uint64_t           sector_count = 0;
    blockdev_device_t  device;
    bool               have_block = false;
    vfs_node_t         parent;
    vfs_node_t         node;
    char               parent_path[CONFIG_VFS_PATH_MAX];
    int                status;

    if (!path) return -EINVAL;
    status = vfs_split_parent(path, parent_path, sizeof(parent_path), NULL);
    if (status != EOK) return status;

    /* Device callbacks need a node whose filesystem declares device-node support. */
    parent = vfs_open(parent_path);
    if (!parent) return -ENOENT;
    if (!vfs_node_supports_device_nodes(parent)) {
        vfs_close(parent);
        return -EPERM;
    }
    vfs_close(parent);

    if (block) {
        status = devtmpfs_block_lookup(major, minor, &device, &node_dev);
        if (status == EOK) {
            if (!blockdev_geometry_valid(&device)) {
                blockdev_release(&device);
            } else {
                sector_size  = device.sector_size;
                sector_count = device.sector_count;
                status       = devtmpfs_block_ops_init(&ops, &device);
                blockdev_release(&device); // the callbacks hold their own reference
                if (status != EOK) return status;
                have_block = true;
            }
        }
    }

    status = vfs_mkfile_mode(path, mode);
    if (status != EOK) goto fail;

    node = vfs_open(path);
    if (!node) {
        status = -ENOENT;
        goto fail;
    }

    node->dev  = node_dev;
    node->rdev = dev;
    status     = tmpfs_bind_device(node, node_type, &ops);
    if (status != EOK) {
        (void)vfs_namespace_unlink(node);
        vfs_close(node);
        goto fail;
    }
    if (have_block) {
        node->blksz = sector_size;
        node->size  = sector_count * sector_size;
    }
    vfs_close(node);
    return EOK;
fail:
    if (have_block) devtmpfs_block_ops_destroy(&ops);
    return status;
}

/* Register the /dev/fb0 framebuffer device node. */
static int devtmpfs_create_framebuffer_node(void)
{
    static const tmpfs_device_ops_t fb_device = {
        .read  = video_fb_read,
        .write = video_fb_write,
        .poll  = 0,
        .ioctl = video_fb_ioctl,
        .mmap  = video_fb_mmap,
        .ctx   = 0,
    };

    int ret = devtmpfs_register_char_device("/dev/fb0", MKDEV(FB_MAJOR, 0), MKDEV(FB_MAJOR, 0), file_fbdev | file_stream, &fb_device);
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

/* Register the /dev/ptmx pseudo-terminal master node. */
#if CONFIG_UNIX98_PTYS
static int devtmpfs_create_ptmx_node(void)
{
    if (devtmpfs_register_char_device("/dev/ptmx", MKDEV(PTMX_MAJOR, PTMX_MINOR), MKDEV(PTMX_MAJOR, PTMX_MINOR), file_ptmx | file_stream, &pty_ptmx_operations) != EOK) return 0;
    vfs_node_t node = vfs_open("/dev/ptmx");
    if (node) {
        node->mode = 0666;
        vfs_close(node);
    }
    return 1;
}
#endif

/* Register the /dev/rtc0 real-time clock device node. */
static int devtmpfs_create_rtc_node(void)
{
    static const tmpfs_device_ops_t rtc_device = {
        .file_read  = rtc_dev_read,
        .file_write = rtc_dev_write,
        .file_ioctl = rtc_dev_ioctl,
    };

    if (devtmpfs_register_char_device("/dev/rtc0", MKDEV(RTC_DEV_MAJOR, RTC0_MINOR), MKDEV(RTC_DEV_MAJOR, RTC0_MINOR), file_stream, &rtc_device) != EOK) return 0;
    vfs_node_t node = vfs_open("/dev/rtc0");
    if (node) {
        node->mode = 0644;
        vfs_close(node);
    }
    return 1;
}

/* Adapter from the gendisk partition iterator to the block walk callback. */
static void devtmpfs_partition_walk_cb(const gendisk_t *disk, const char *part_name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque)
{
    devtmpfs_partition_walk_t *ctx = opaque;
    (void)disk;
    ctx->fn(part_name, major, minor, blocks, ctx->opaque);
}

/*
 * Enumerate every published whole disk plus its partitions by walking the
 * gendisk registry.  Minor numbers follow the conventional per-backend
 * layout (hd=3, sd=8, sr=11, nvme=259).
 */
static void devtmpfs_walk_block_devices(devtmpfs_block_walk_fn fn, void *opaque)
{
    devtmpfs_partition_walk_t ctx = {.fn = fn, .opaque = opaque};

    for (int index = 0;; index++) {
        gendisk_t disk;
        if (!block_disk_snapshot(index, &disk)) break;
        fn(disk.name, disk.major, disk.minor_base, disk.device.sector_count * disk.device.sector_size / 1024, opaque);
        blockdev_release(&disk.device);
    }
    block_foreach_partition(devtmpfs_partition_walk_cb, &ctx);
}

/* Map a disk name prefix to the conventional device class. */
static const char *devtmpfs_block_class(const char *name)
{
    if (!strncmp(name, "sd", 2)) return "sd";
    if (!strncmp(name, "hd", 2)) return "ide";
    if (!strncmp(name, "sr", 2)) return "sr";
    if (!strncmp(name, "nvme", 4)) return "nvme";
    return "block";
}

/* Emit one line per unique block major for /proc/devices. */
static void devtmpfs_devices_block(const char *name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque)
{
    devtmpfs_devices_ctx_t *ctx = opaque;
    const char             *cls = devtmpfs_block_class(name);
    (void)minor;
    (void)blocks;
    for (size_t i = 0; i < ctx->seen_count; i++)
        if (ctx->seen[i].major == major) return;
    if (ctx->seen_count >= sizeof(ctx->seen) / sizeof(ctx->seen[0])) return;
    ctx->seen[ctx->seen_count].major = major;
    strncpy(ctx->seen[ctx->seen_count].cls, cls, sizeof(ctx->seen[0].cls) - 1);
    ctx->seen[ctx->seen_count].cls[sizeof(ctx->seen[0].cls) - 1] = '\0';
    ctx->seen_count++;
    int n = snprintf(ctx->buf + ctx->off, ctx->cap - ctx->off, "  %3u %s\n", major, cls);
    if (n > 0 && (size_t)n < ctx->cap - ctx->off) ctx->off += (size_t)n;
}

/* /proc/devices: one line per character/block device major. */
int devtmpfs_format_proc_devices(char *buf, size_t cap)
{
    devtmpfs_devices_ctx_t ctx;
    size_t                 off = 0;
    if (!buf || cap < 128) return 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = buf;
    ctx.cap = cap;

    off += (size_t)snprintf(buf + off, cap - off, "Character devices:\n");
    spin_lock(&devtmpfs_lock);
    for (int i = 0; i < CONFIG_DEVTMPFS_MAX_DEVICES; i++) {
        if (!devtmpfs_table[i].active || (devtmpfs_table[i].node_type & file_block)) continue;
        uint32_t    major = MAJOR(devtmpfs_table[i].dev);
        const char *leaf  = strrchr(devtmpfs_table[i].path, '/');
        leaf              = leaf ? leaf + 1 : devtmpfs_table[i].path;
        bool dup          = false;
        for (int j = 0; j < i; j++) {
            if (!devtmpfs_table[j].active || (devtmpfs_table[j].node_type & file_block)) continue;
            if (MAJOR(devtmpfs_table[j].dev) != major) continue;
            const char *leaf2 = strrchr(devtmpfs_table[j].path, '/');
            leaf2             = leaf2 ? leaf2 + 1 : devtmpfs_table[j].path;
            if (streq(leaf, leaf2)) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        int n = snprintf(buf + off, cap - off, "  %3u %s\n", major, leaf);
        if (n <= 0 || (size_t)n >= cap - off) break;
        off += (size_t)n;
    }
    spin_unlock(&devtmpfs_lock);

    off += (size_t)snprintf(buf + off, cap - off, "\nBlock devices:\n");
    ctx.off = off;
    devtmpfs_walk_block_devices(devtmpfs_devices_block, &ctx);
    return (int)ctx.off;
}

/* Emit one line per disk or partition for /proc/partitions. */
static void devtmpfs_partitions_block(const char *name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque)
{
    devtmpfs_block_file_ctx_t *ctx = opaque;
    int                        n   = snprintf(ctx->buf + ctx->off, ctx->cap - ctx->off, "  %u        %u %9llu %s\n", major, minor, blocks, name);
    if (n > 0 && (size_t)n < ctx->cap - ctx->off) ctx->off += (size_t)n;
}

/* /proc/partitions: whole disks and their MBR/GPT partition views. */
int devtmpfs_format_proc_partitions(char *buf, size_t cap)
{
    devtmpfs_block_file_ctx_t ctx;
    if (!buf || cap < 128) return 0;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = buf;
    ctx.cap = cap;
    ctx.off += (size_t)snprintf(buf + ctx.off, cap - ctx.off, "major minor  #blocks  name\n\n");
    devtmpfs_walk_block_devices(devtmpfs_partitions_block, &ctx);
    return (int)ctx.off;
}

/* Emit a zeroed per-disk counter line for /proc/diskstats. */
static void devtmpfs_diskstats_block(const char *name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque)
{
    devtmpfs_block_file_ctx_t *ctx = opaque;
    (void)blocks;
    int n = snprintf(ctx->buf + ctx->off, ctx->cap - ctx->off, "%4u %4u %s 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", major, minor, name);
    if (n > 0 && (size_t)n < ctx->cap - ctx->off) ctx->off += (size_t)n;
}

/* /proc/diskstats: per-disk and per-partition I/O counters. */
int devtmpfs_format_proc_diskstats(char *buf, size_t cap)
{
    devtmpfs_block_file_ctx_t ctx;
    if (!buf || cap < 128) return 0;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = buf;
    ctx.cap = cap;
    devtmpfs_walk_block_devices(devtmpfs_diskstats_block, &ctx);
    return (int)ctx.off;
}

/* Populate /dev with the registered device nodes. */
void devtmpfs_init(void)
{
    int status;
    int total_devices = 0;

    status = vfs_mkdir("/dev");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev: %d\n", status);
        return;
    }

    /*
     * CONFIG_DEVTMPFS_MOUNT semantics: establish /dev as a real devtmpfs
     * mount before publishing nodes.  OpenRC/eudev intentionally verify both
     * the filesystem type and the mount point, not merely the directory.
     */
#if CONFIG_DEVTMPFS_MOUNT
    vfs_node_t dev_root = vfs_open("/dev");
    if (!dev_root) {
        plogk("devtmpfs: Cannot open /dev mount point.\n");
        return;
    }
    if (!dev_root->is_mount) status = vfs_mount_fs("devtmpfs", "devtmpfs", dev_root);
    vfs_close(dev_root);
    if (status != EOK) {
        plogk("devtmpfs: Cannot mount /dev: %d\n", status);
        return;
    }
#endif

    status = vfs_mkdir("/dev/pts");
    if (status != EOK && status != -EEXIST) plogk("devtmpfs: Cannot create /dev/pts: %d\n", status);

    /*
     * /dev/shm is a private tmpfs mount with the sticky bit set, matching
     * the devtmpfs layout (openrc mounts tmpfs there at boot).
     */
    status = vfs_mkdir("/dev/shm");
    if (status != EOK && status != -EEXIST) plogk("devtmpfs: Cannot create /dev/shm: %d\n", status);
    vfs_node_t shm_root = vfs_open("/dev/shm");
    if (shm_root) {
        if (!shm_root->is_mount) {
            status = vfs_mount_fs("tmpfs", "tmpfs", shm_root);
            if (status != EOK) plogk("devtmpfs: Cannot mount tmpfs on /dev/shm: %d\n", status);
        }
        shm_root->mode        = 01777;
        shm_root->permissions = 01777;
        vfs_close(shm_root);
    }

    total_devices += evdev_publish_nodes();
    total_devices += chrdev_populate();
    total_devices += tty_devices_populate();
    total_devices += devtmpfs_create_framebuffer_node();
#if CONFIG_UNIX98_PTYS
    total_devices += devtmpfs_create_ptmx_node();
#endif
    total_devices += devtmpfs_create_rtc_node();
    devtmpfs_populated = true; // Boot-time population done; later devices stay silent.

    /* Conventional process-fd aliases expected by libc and service scripts. */
    status = vfs_symlink("/dev/fd", "/proc/self/fd");
    if (status != EOK && status != -EEXIST) plogk("devtmpfs: Cannot create /dev/fd: %d\n", status);
    status = vfs_symlink("/dev/stdin", "/proc/self/fd/0");
    if (status != EOK && status != -EEXIST) plogk("devtmpfs: Cannot create /dev/stdin: %d\n", status);
    status = vfs_symlink("/dev/stdout", "/proc/self/fd/1");
    if (status != EOK && status != -EEXIST) plogk("devtmpfs: Cannot create /dev/stdout: %d\n", status);
    status = vfs_symlink("/dev/stderr", "/proc/self/fd/2");
    if (status != EOK && status != -EEXIST) plogk("devtmpfs: Cannot create /dev/stderr: %d\n", status);

    plogk("devtmpfs: %d device(s) created in /dev\n", total_devices);
}
