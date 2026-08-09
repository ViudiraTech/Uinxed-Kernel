/*
 *
 *      devtmpfs.c
 *      Device tmpfs population helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <drivers/ata/pata/atapi.h>
#include <drivers/ata/pata/ide.h>
#include <drivers/ata/sata/ahci.h>
#include <drivers/block/blockdev.h>
#include <drivers/block/partition.h>
#include <drivers/core/device.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/fbdev/fbdev.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/input/evdev/evdev.h>
#include <drivers/nvme/nvme.h>
#include <drivers/tty/tty.h>
#include <fs/core/vfs.h>
#include <fs/virtual/devtmpfs.h>
#include <fs/virtual/tmpfs.h>
#include <kernel/sound/audio.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/* Character-device registration table                                 */
/* ------------------------------------------------------------------ */

#define DEVTMPFS_MAX_DEVICES     512
#define DEVTMPFS_MAX_BLOCK_NODES (PARTITION_MAX_COUNT + 1)

typedef struct devtmpfs_block_registration {
        char   paths[DEVTMPFS_MAX_BLOCK_NODES][96];
        size_t count;
} devtmpfs_block_registration_t;

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

typedef enum devtmpfs_memory_kind {
    DEVTMPFS_MEM_NULL,
    DEVTMPFS_MEM_ZERO,
    DEVTMPFS_MEM_FULL,
    DEVTMPFS_MEM_RANDOM,
} devtmpfs_memory_kind_t;

typedef struct {
        devtmpfs_memory_kind_t kind;
} devtmpfs_memory_device_t;

static spinlock_t devtmpfs_random_lock = {.lock = 0, .rflags = 0};
static uint64_t   devtmpfs_random_state[4];

static uint64_t devtmpfs_random_word(void)
{
    spin_lock(&devtmpfs_random_lock);
    if (!(devtmpfs_random_state[0] | devtmpfs_random_state[1] | devtmpfs_random_state[2] | devtmpfs_random_state[3])) {
        uint64_t seed = rdtsc() ^ (uintptr_t)&devtmpfs_random_state ^ 0x9e3779b97f4a7c15ULL;
        for (size_t i = 0; i < 4; i++) {
            seed += 0x9e3779b97f4a7c15ULL;
            uint64_t z               = seed;
            z                        = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z                        = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            devtmpfs_random_state[i] = z ^ (z >> 31);
        }
    }

    /*
     * xoshiro256**.  The kernel getrandom path can later feed additional
     * hardware/platform entropy into the same state without changing the
     * character-device ABI.
     */
    uint64_t result = ((devtmpfs_random_state[1] * 5) << 7 | (devtmpfs_random_state[1] * 5) >> 57) * 9;
    uint64_t t      = devtmpfs_random_state[1] << 17;
    devtmpfs_random_state[2] ^= devtmpfs_random_state[0];
    devtmpfs_random_state[3] ^= devtmpfs_random_state[1];
    devtmpfs_random_state[1] ^= devtmpfs_random_state[2];
    devtmpfs_random_state[0] ^= devtmpfs_random_state[3];
    devtmpfs_random_state[2] ^= t;
    devtmpfs_random_state[3] = (devtmpfs_random_state[3] << 45) | (devtmpfs_random_state[3] >> 19);
    spin_unlock(&devtmpfs_random_lock);
    return result;
}

static int64_t devtmpfs_memory_read(void *context, void *private_data, uint64_t flags, void *buffer, size_t offset, size_t size)
{
    devtmpfs_memory_device_t *device = context;
    (void)private_data;
    (void)flags;
    (void)offset;
    if (!device || (!buffer && size)) return -EINVAL;
    if (device->kind == DEVTMPFS_MEM_NULL) return 0;
    if (device->kind == DEVTMPFS_MEM_ZERO || device->kind == DEVTMPFS_MEM_FULL) {
        memset(buffer, 0, size);
        return (int64_t)size;
    }

    uint8_t *out = buffer;
    while (size) {
        uint64_t word  = devtmpfs_random_word();
        size_t   chunk = size < sizeof(word) ? size : sizeof(word);
        memcpy(out, &word, chunk);
        out += chunk;
        size -= chunk;
    }
    return (int64_t)(out - (uint8_t *)buffer);
}

static int64_t devtmpfs_memory_write(void *context, void *private_data, uint64_t flags, const void *buffer, size_t offset, size_t size)
{
    devtmpfs_memory_device_t *device = context;
    (void)private_data;
    (void)flags;
    (void)buffer;
    (void)offset;
    if (!device) return -EINVAL;
    return device->kind == DEVTMPFS_MEM_FULL ? -ENOSPC : (int64_t)size;
}

static int64_t devtmpfs_memory_write_user(void *context, void *private_data, uint64_t flags, const void *buffer, size_t offset, size_t size,
                                          struct process *proc)
{
    /* Linux's null iterator advances without fetching source bytes. */
    (void)proc;
    return devtmpfs_memory_write(context, private_data, flags, buffer, offset, size);
}

static int64_t devtmpfs_kmsg_read(void *context, void *private_data, uint64_t flags, void *buffer, size_t offset, size_t size)
{
    (void)context;
    (void)private_data;
    (void)flags;
    (void)buffer;
    (void)offset;
    (void)size;
    /*
     * The kernel currently exposes printk output through its consoles.  A
     * nonblocking kmsg reader observes no queued record rather than EOF.
     */
    return -EAGAIN;
}

static int64_t devtmpfs_kmsg_write(void *context, void *private_data, uint64_t flags, const void *buffer, size_t offset, size_t size)
{
    (void)context;
    (void)private_data;
    (void)flags;
    (void)offset;
    if (!buffer && size) return -EINVAL;
    if (!size) return 0;

    char *message = malloc(size + 1);
    if (!message) return -ENOMEM;
    memcpy(message, buffer, size);
    message[size] = '\0';
    /*
     * /dev/kmsg writers may prefix a syslog priority as "<n>".  The console
     * backend has no severity lanes yet, but should not print that framing.
     */
    char *text = message;
    if (text[0] == '<') {
        char *end = strchr(text, '>');
        if (end && (size_t)(end - text) <= 4) text = end + 1;
    }
    printk("%s", text);
    free(message);
    return (int64_t)size;
}

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
    (void)snprintf(dev_path, sizeof(dev_path), "%s%s%u", dev_prefix, use_p_separator ? "p" : "", partition->number);

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
        .mmap  = video_fb_mmap,
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

static int devtmpfs_create_memory_nodes(void)
{
    static devtmpfs_memory_device_t devices[] = {
        {.kind = DEVTMPFS_MEM_NULL},   {.kind = DEVTMPFS_MEM_ZERO},   {.kind = DEVTMPFS_MEM_FULL},
        {.kind = DEVTMPFS_MEM_RANDOM}, {.kind = DEVTMPFS_MEM_RANDOM},
    };
    static const struct {
            const char *path;
            uint8_t     minor;
    } nodes[] = {
        {.path = "/dev/null",    .minor = 3},
        {.path = "/dev/zero",    .minor = 5},
        {.path = "/dev/full",    .minor = 7},
        {.path = "/dev/random",  .minor = 8},
        {.path = "/dev/urandom", .minor = 9},
    };

    int count = 0;
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        tmpfs_device_ops_t ops = {
            .file_read       = devtmpfs_memory_read,
            .file_write      = devtmpfs_memory_write,
            .file_write_user = devtmpfs_memory_write_user,
            .ctx             = &devices[i],
        };
        uint64_t devt = MKDEV(1, nodes[i].minor);
        if (devtmpfs_register_char_device(nodes[i].path, devt, devt, file_stream, &ops) != EOK) continue;
        vfs_node_t node = vfs_open(nodes[i].path);
        if (node) {
            node->mode = 0666;
            vfs_close(node);
        }
        count++;
    }
    return count;
}

static int devtmpfs_create_kmsg_node(void)
{
    tmpfs_device_ops_t ops = {
        .file_read  = devtmpfs_kmsg_read,
        .file_write = devtmpfs_kmsg_write,
    };
    uint64_t devt = MKDEV(1, 11);
    int      ret  = devtmpfs_register_char_device("/dev/kmsg", devt, devt, file_stream, &ops);
    if (ret == EOK) {
        vfs_node_t node = vfs_open("/dev/kmsg");
        if (node) {
            node->mode = 0600;
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

        (void)snprintf(dev_path, sizeof(dev_path), "/dev/snd/%s", audio_node->name);
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
        {.path = "/dev/tty1",    .major = TTY_MAJOR,     .minor = 1 },
        {.path = "/dev/tty2",    .major = TTY_MAJOR,     .minor = 2 },
        {.path = "/dev/tty3",    .major = TTY_MAJOR,     .minor = 3 },
        {.path = "/dev/tty4",    .major = TTY_MAJOR,     .minor = 4 },
        {.path = "/dev/tty5",    .major = TTY_MAJOR,     .minor = 5 },
        {.path = "/dev/tty6",    .major = TTY_MAJOR,     .minor = 6 },
        {.path = "/dev/tty7",    .major = TTY_MAJOR,     .minor = 7 },
        {.path = "/dev/ttyS0",   .major = TTY_MAJOR,     .minor = 64},
        {.path = "/dev/ttyS1",   .major = TTY_MAJOR,     .minor = 65},
        {.path = "/dev/ttyS2",   .major = TTY_MAJOR,     .minor = 66},
        {.path = "/dev/ttyS3",   .major = TTY_MAJOR,     .minor = 67},
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
        .read       = 0,
        .write      = 0,
        .poll       = 0,
        .ioctl      = 0,
        .open       = (tmpfs_dev_open_t)drm_dev_open,
        .release    = (tmpfs_dev_release_t)drm_dev_release,
        .mmap       = drm_dev_file_mmap,
        .file_read  = drm_dev_file_read,
        .file_write = drm_dev_file_write,
        .file_poll  = drm_dev_file_poll,
        .file_ioctl = drm_dev_file_ioctl,
        .ctx        = 0,
    };

    struct drm_device *drm_dev = drm_get_singleton();
    if (!drm_dev) return 0;

    int total = 0;
    if (devtmpfs_register_char_device("/dev/dri/card0", MKDEV(226, 0), MKDEV(226, 0), file_stream, &drm_device) == 0) total++;
    /*
     * libdrm probes the canonical render node as well. The current kernel
     * shares the DRM device implementation for this render-only minor.
     */
    if (devtmpfs_register_char_device("/dev/dri/renderD128", MKDEV(226, 128), MKDEV(226, 128), file_stream, &drm_device) == 0) total++;
    return total;
}

/* ------------------------------------------------------------------ */
/*  /proc block-device export helpers                                  */
/* ------------------------------------------------------------------ */

typedef void (*devtmpfs_block_walk_fn)(const char *name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque);

static void devtmpfs_walk_partitions(devtmpfs_block_walk_fn fn, void *opaque, uint32_t major, uint32_t minor_base, const char *disk_name,
                                     const blockdev_device_t *device, bool use_p)
{
    (void)use_p;
    partition_table_t table;
    if (partition_scan(device, &table) != EOK) return;
    bool separator = disk_name[strlen(disk_name) - 1] >= '0' && disk_name[strlen(disk_name) - 1] <= '9';
    for (size_t i = 0; i < table.count; i++) {
        const partition_info_t *part = &table.partitions[i];
        blockdev_device_t       view;
        if (blockdev_open_partition(device, part->start_lba, part->sector_count, &view) != EOK) continue;
        char name[96];
        (void)snprintf(name, sizeof(name), "%s%s%u", disk_name, separator ? "p" : "", part->number);
        fn(name, major, minor_base + part->number, view.sector_count * view.sector_size / 1024, opaque);
    }
    partition_table_destroy(&table);
}

static void devtmpfs_emit_block(devtmpfs_block_walk_fn fn, void *opaque, uint32_t major, uint32_t minor, const char *name,
                                const blockdev_device_t *device)
{
    fn(name, major, minor, device->sector_count * device->sector_size / 1024, opaque);
}

/*
 * Enumerate every published whole disk plus its partitions.  Minor numbers
 * follow the conventional per-backend layout (hd=3, sd=8, sr=11, nvme=259).
 */
static void devtmpfs_walk_block_devices(devtmpfs_block_walk_fn fn, void *opaque)
{
    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;
        blockdev_device_t device;
        if (blockdev_open_ide(drive, &device) != EOK) continue;
        char name[32];
        (void)snprintf(name, sizeof(name), "hd%c", 'a' + drive);
        devtmpfs_emit_block(fn, opaque, 3, drive, name, &device);
        devtmpfs_walk_partitions(fn, opaque, 3, drive, name, &device, false);
    }
    for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATA) continue;
        char disk_name[16];
        if (blockdev_format_disk_name(disk_name, sizeof(disk_name), d) != EOK) continue;
        blockdev_device_t device;
        if (blockdev_open_ahci(d, &device) != EOK) continue;
        devtmpfs_emit_block(fn, opaque, 8, d, disk_name, &device);
        devtmpfs_walk_partitions(fn, opaque, 8, d, disk_name, &device, false);
    }
    {
        uint8_t sr_idx = 0;
        for (uint8_t drive = 0; drive < 4; drive++) {
            if (!atapi_devices[drive].reserved || atapi_devices[drive].type != IDE_ATAPI) continue;
            blockdev_device_t device;
            if (blockdev_open_atapi(drive, &device) != EOK) continue;
            char name[32];
            (void)snprintf(name, sizeof(name), "sr%u", (unsigned)sr_idx);
            devtmpfs_emit_block(fn, opaque, 11, sr_idx, name, &device);
            sr_idx++;
        }
        for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
            if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATAPI) continue;
            blockdev_device_t device;
            if (blockdev_open_ahci_atapi(d, &device) != EOK) continue;
            char name[32];
            (void)snprintf(name, sizeof(name), "sr%u", (unsigned)sr_idx);
            devtmpfs_emit_block(fn, opaque, 11, sr_idx, name, &device);
            sr_idx++;
        }
    }
    for (int c = 0; c < nvme_controller_count(); c++) {
        nvme_controller_t *ctrl = nvme_get_controller(c);
        if (!ctrl || !ctrl->initialised) continue;
        for (uint32_t ns = 0; ns < ctrl->num_namespaces; ns++) {
            if (!ctrl->namespaces[ns].ready) continue;
            blockdev_device_t device;
            if (blockdev_open_nvme(&ctrl->namespaces[ns], &device) != EOK) continue;
            char name[64];
            (void)snprintf(name, sizeof(name), "nvme%dn%u", ctrl->id, ctrl->namespaces[ns].nsid);
            devtmpfs_emit_block(fn, opaque, 259, ctrl->namespaces[ns].nsid, name, &device);
            devtmpfs_walk_partitions(fn, opaque, 259, ctrl->namespaces[ns].nsid, name, &device, true);
        }
    }
}

static const char *devtmpfs_block_class(const char *name)
{
    if (!strncmp(name, "sd", 2)) return "sd";
    if (!strncmp(name, "hd", 2)) return "ide";
    if (!strncmp(name, "sr", 2)) return "sr";
    if (!strncmp(name, "nvme", 4)) return "nvme";
    return "block";
}

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

static void devtmpfs_devices_block(const char *name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque)
{
    devtmpfs_devices_ctx_t *ctx = opaque;
    const char             *cls = devtmpfs_block_class(name);
    (void)minor;
    (void)blocks;
    for (size_t i = 0; i < ctx->seen_count; i++) {
        if (ctx->seen[i].major == major) return;
    }
    if (ctx->seen_count >= sizeof(ctx->seen) / sizeof(ctx->seen[0])) return;
    ctx->seen[ctx->seen_count].major = major;
    strncpy(ctx->seen[ctx->seen_count].cls, cls, sizeof(ctx->seen[0].cls) - 1);
    ctx->seen[ctx->seen_count].cls[sizeof(ctx->seen[0].cls) - 1] = '\0';
    ctx->seen_count++;
    int n = snprintf(ctx->buf + ctx->off, ctx->cap - ctx->off, "  %3u %s\n", major, cls);
    if (n > 0 && (size_t)n < ctx->cap - ctx->off) ctx->off += (size_t)n;
}

/* Linux /proc/devices: one line per character/block device major. */
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
    for (int i = 0; i < DEVTMPFS_MAX_DEVICES; i++) {
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

typedef struct {
        char  *buf;
        size_t cap;
        size_t off;
} devtmpfs_block_file_ctx_t;

static void devtmpfs_partitions_block(const char *name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque)
{
    devtmpfs_block_file_ctx_t *ctx = opaque;
    int n = snprintf(ctx->buf + ctx->off, ctx->cap - ctx->off, "  %u        %u %9llu %s\n", major, minor, (unsigned long long)blocks, name);
    if (n > 0 && (size_t)n < ctx->cap - ctx->off) ctx->off += (size_t)n;
}

/* Linux /proc/partitions: whole disks and their MBR/GPT partition views. */
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

static void devtmpfs_diskstats_block(const char *name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque)
{
    devtmpfs_block_file_ctx_t *ctx = opaque;
    (void)blocks;
    int n = snprintf(ctx->buf + ctx->off, ctx->cap - ctx->off, "%4u %4u %s 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", major, minor, name);
    if (n > 0 && (size_t)n < ctx->cap - ctx->off) ctx->off += (size_t)n;
}

/* Linux /proc/diskstats: per-disk and per-partition I/O counters. */
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

    status = vfs_mkdir("/dev/pts");
    if (status != EOK && status != -EEXIST) plogk("devtmpfs: Cannot create /dev/pts: %d\n", status);

    /*
     * /dev/shm is a private tmpfs mount with the sticky bit set, matching
     * the Linux devtmpfs layout (openrc mounts tmpfs there at boot).
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

    /* Register whole disks without issuing media I/O during early boot. */
    /* IDE ATA drives -> /dev/hda, /dev/hdb, ... */
    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;

        char              dev_path[32];
        blockdev_device_t device;
        (void)snprintf(dev_path, sizeof(dev_path), "/dev/hd%c", 'a' + drive);
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
        (void)snprintf(dev_path, sizeof(dev_path), "/dev/%s", disk_name);
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
            (void)snprintf(dev_path, sizeof(dev_path), "/dev/sr%u", (unsigned)sr_idx);
            if (blockdev_open_atapi(drive, &device) == EOK && devtmpfs_create_block_node(dev_path, &device, drive, drive, false) == EOK)
                total_devices++;
            sr_idx++;
        }

        for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
            if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATAPI) continue;

            char              dev_path[32];
            blockdev_device_t device;
            (void)snprintf(dev_path, sizeof(dev_path), "/dev/sr%u", (unsigned)sr_idx);
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
            (void)snprintf(ns_path, sizeof(ns_path), "/dev/nvme%dn%u", ctrl->id, ctrl->namespaces[ns].nsid);
            if (blockdev_open_nvme(&ctrl->namespaces[ns], &device) == EOK) {
                if (devtmpfs_create_block_node(ns_path, &device, ctrl->id, ctrl->namespaces[ns].nsid, false) == EOK) total_devices++;
                total_devices += devtmpfs_create_partitions(ns_path, true, &device, ctrl->id, ctrl->namespaces[ns].nsid);
            }
        }
    }

    total_devices += evdev_publish_nodes();
    total_devices += devtmpfs_create_memory_nodes();
    total_devices += devtmpfs_create_kmsg_node();
    total_devices += devtmpfs_create_framebuffer_node();
    total_devices += devtmpfs_create_audio_nodes();
    total_devices += devtmpfs_create_tty_nodes();
    total_devices += devtmpfs_create_drm_node();

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
