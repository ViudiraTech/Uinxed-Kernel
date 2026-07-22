/*
 *
 *      devtmpfs.c
 *      Device tmpfs population helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <blockdev.h>
#include <devtmpfs.h>
#include <drm/drm_init.h>
#include <errno.h>
#include <evdev.h>
#include <fbdev.h>
#include <ide.h>
#include <input_event.h>
#include <printk.h>
#include <ps2.h>
#include <sound/audio.h>
#include <tmpfs.h>
#include <tty.h>
#include <vfs.h>
#include <video.h>

typedef struct mbr_partition_entry {
        uint8_t  status;
        uint8_t  chs_first[3];
        uint8_t  type;
        uint8_t  chs_last[3];
        uint32_t first_lba;
        uint32_t sectors;
} __attribute__((packed)) mbr_partition_entry_t;

typedef struct mbr_sector {
        uint8_t               boot_code[446];
        mbr_partition_entry_t partitions[4];
        uint16_t              signature;
} __attribute__((packed)) mbr_sector_t;

static char devtmpfs_drive_letter(uint8_t drive)
{
    return (char)('a' + drive);
}

static int devtmpfs_scan_mbr(uint8_t drive, mbr_partition_entry_t parts[4])
{
    blockdev_device_t device;
    mbr_sector_t      mbr;
    int               status;

    status = blockdev_open_ide(drive, &device);
    if (status != EOK) return status;
    status = blockdev_read_bytes(&device, 0, &mbr, sizeof(mbr));
    if (status != EOK) return status;
    if (mbr.signature != 0xAA55) return -ENOENT;

    for (int i = 0; i < 4; i++) parts[i] = mbr.partitions[i];
    return EOK;
}

static void devtmpfs_create_block_node(uint8_t drive)
{
    char       dev_path[] = "/dev/hda";
    vfs_node_t node;
    int        status;

    dev_path[7] = devtmpfs_drive_letter(drive);
    status      = vfs_mkfile(dev_path);
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create %s: %d\n", dev_path, status);
        return;
    }

    node = vfs_open(dev_path);
    if (!node) {
        plogk("devtmpfs: Cannot open %s after creation.\n", dev_path);
        return;
    }

    node->type  = file_block;
    node->blksz = 512;
    node->dev   = drive;
    node->rdev  = drive;
    node->size  = (uint64_t)ide_devices[drive].size * 512;
    plogk("devtmpfs: Registered %s as block device.\n", dev_path);
}

static void devtmpfs_create_partition_node(uint8_t drive, uint8_t part_index, const mbr_partition_entry_t *part)
{
    char       dev_path[] = "/dev/hda1";
    vfs_node_t node;
    int        status;

    dev_path[7] = devtmpfs_drive_letter(drive);
    dev_path[8] = (char)('1' + part_index);
    status      = vfs_mkfile(dev_path);
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create %s: %d\n", dev_path, status);
        return;
    }

    node = vfs_open(dev_path);
    if (!node) {
        plogk("devtmpfs: Cannot open %s after creation.\n", dev_path);
        return;
    }

    node->type  = file_block;
    node->blksz = 512;
    node->dev   = drive;
    node->rdev  = ((uint64_t)drive << 8) | (part_index + 1);
    node->size  = (uint64_t)part->sectors * 512;
    plogk("devtmpfs: Registered %s for partition type 0x%02x, start %u, sectors %u\n", dev_path, part->type, part->first_lba, part->sectors);
}

/* ---- evdev ioctl wrapper for PS/2 keyboard ---- */

static evdev_client_t *ps2kbd_evdev_client;

static int ps2kbd_evdev_ioctl(void *ctx, size_t req, void *arg)
{
    evdev_t *evdev = ps2_keyboard_evdev;

    (void)ctx;

    if (!evdev || !evdev->exist)
        return -ENODEV;

    if (!ps2kbd_evdev_client) {
        ps2kbd_evdev_client = evdev_fop_open(evdev);
        if (!ps2kbd_evdev_client)
            return -ENOMEM;
    }

    return evdev_fop_ioctl(ps2kbd_evdev_client, (uint32_t)req, arg);
}

static const tmpfs_device_ops_t ps2kbd_device = {
    .read  = ps2kbd_read_events,
    .poll  = ps2kbd_poll_events,
    .ioctl = ps2kbd_evdev_ioctl,
    .write = 0,
    .ctx   = 0,
};

static void devtmpfs_create_input_event_node(void)
{
    vfs_node_t node;
    int        status;

    status = vfs_mkdir("/dev/input");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev/input: %d\n", status);
        return;
    }

    status = vfs_mkfile("/dev/input/event0");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev/input/event0: %d\n", status);
        return;
    }

    node = vfs_open("/dev/input/event0");
    if (!node) {
        plogk("devtmpfs: Cannot open /dev/input/event0 after creation.\n");
        return;
    }

    status = tmpfs_bind_device(node, file_keyboard | file_stream, &ps2kbd_device);
    if (status != EOK) {
        plogk("devtmpfs: Cannot bind /dev/input/event0: %d\n", status);
        vfs_close(node);
        return;
    }

    node->blksz = sizeof(input_event_t);
    node->dev   = 1;
    node->rdev  = 0;
    plogk("devtmpfs: Registered /dev/input/event0 as PS/2 keyboard event device.\n");
    vfs_close(node);
}

static void devtmpfs_create_framebuffer_node(void)
{
    static const tmpfs_device_ops_t fb_device = {
        .read  = video_fb_read,
        .write = video_fb_write,
        .poll  = 0,
        .ioctl = video_fb_ioctl,
        .ctx   = 0,
    };

    video_info_t info;
    vfs_node_t   node;
    int          status;

    status = vfs_mkfile("/dev/fb0");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev/fb0: %d\n", status);
        return;
    }

    node = vfs_open("/dev/fb0");
    if (!node) {
        plogk("devtmpfs: Cannot open /dev/fb0 after creation.\n");
        return;
    }

    status = tmpfs_bind_device(node, file_fbdev | file_stream, &fb_device);
    if (status != EOK) {
        plogk("devtmpfs: Cannot bind /dev/fb0: %d\n", status);
        vfs_close(node);
        return;
    }

    info        = video_get_info();
    node->blksz = sizeof(uint32_t);
    node->size  = info.stride * info.height * sizeof(uint32_t);
    node->dev   = 2;
    node->rdev  = 0;
    plogk("devtmpfs: Registered /dev/fb0 as framebuffer device.\n");
    vfs_close(node);
}

static void devtmpfs_create_audio_nodes(void)
{
    int status;

    if (!audio_device_node_count()) return;

    status = vfs_mkdir("/dev/snd");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev/snd: %d\n", status);
        return;
    }

    for (size_t i = 0; i < audio_device_node_count(); i++) {
        audio_device_node_t *audio_node = audio_get_device_node(i);
        char                 dev_path[64];
        vfs_node_t           node;

        if (!audio_node) continue;

        snprintf(dev_path, sizeof(dev_path), "/dev/snd/%s", audio_node->name);
        status = vfs_mkfile(dev_path);
        if (status != EOK && status != -EEXIST) {
            plogk("devtmpfs: Cannot create %s: %d\n", dev_path, status);
            continue;
        }

        node = vfs_open(dev_path);
        if (!node) {
            plogk("devtmpfs: Cannot open %s after creation.\n", dev_path);
            continue;
        }

        status = tmpfs_bind_device(node, file_audio | file_stream, &audio_node->tmpfs_ops);
        if (status != EOK) {
            plogk("devtmpfs: Cannot bind %s: %d\n", dev_path, status);
            vfs_close(node);
            continue;
        }

        node->blksz = 1;
        node->dev   = audio_node->card->id;
        node->rdev  = i;
        plogk("devtmpfs: Registered %s as audio device.\n", dev_path);
        vfs_close(node);
    }
}

/* Linux standard TTY device major numbers */
#define TTY_MAJOR     4
#define TTY_AUX_MAJOR 5

typedef struct {
        const char  *path;
        unsigned int major;
        unsigned int minor;
} tty_dev_info_t;

static void devtmpfs_create_tty_nodes(void)
{
    static const tmpfs_device_ops_t tty_device = {
        .read  = 0, /* TTY read from userspace not yet supported */
        .write = tty_dev_write,
        .poll  = tty_dev_poll,
        .ioctl = 0,
        .ctx   = 0,
    };

    static const tty_dev_info_t tty_nodes[] = {
        {.path = "/dev/tty0",    .major = TTY_MAJOR,     .minor = 0},
        {.path = "/dev/tty",     .major = TTY_AUX_MAJOR, .minor = 0},
        {.path = "/dev/console", .major = TTY_AUX_MAJOR, .minor = 1},
    };

    for (size_t i = 0; i < sizeof(tty_nodes) / sizeof(tty_nodes[0]); i++) {
        const tty_dev_info_t *info = &tty_nodes[i];
        vfs_node_t            node;
        int                   status;

        status = vfs_mkfile(info->path);
        if (status != EOK && status != -EEXIST) {
            plogk("devtmpfs: Cannot create %s: %d\n", info->path, status);
            continue;
        }

        node = vfs_open(info->path);
        if (!node) {
            plogk("devtmpfs: Cannot open %s after creation.\n", info->path);
            continue;
        }

        status = tmpfs_bind_device(node, file_stream, &tty_device);
        if (status != EOK) {
            plogk("devtmpfs: Cannot bind %s: %d\n", info->path, status);
            vfs_close(node);
            continue;
        }

        node->blksz = 1;
        node->dev   = info->major;
        node->rdev  = info->minor;
        plogk("devtmpfs: Registered %s as TTY device (major=%u, minor=%u)\n", info->path, info->major, info->minor);
        vfs_close(node);
    }
}

static void devtmpfs_create_drm_node(void)
{
    static const tmpfs_device_ops_t drm_device = {
        .read  = 0,
        .write = 0,
        .poll  = 0,
        .ioctl = 0,
        .ctx   = 0,
    };

    vfs_node_t node;
    int        status;

    struct drm_device *drm_dev = drm_get_singleton();
    if (!drm_dev) {
        return;
    }

    status = vfs_mkdir("/dev/dri");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev/dri: %d\n", status);
        return;
    }

    status = vfs_mkfile("/dev/dri/card0");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev/dri/card0: %d\n", status);
        return;
    }

    node = vfs_open("/dev/dri/card0");
    if (!node) {
        plogk("devtmpfs: Cannot open /dev/dri/card0 after creation.\n");
        return;
    }

    status = tmpfs_bind_device(node, file_stream, &drm_device);
    if (status != EOK) {
        plogk("devtmpfs: Cannot bind /dev/dri/card0: %d\n", status);
        vfs_close(node);
        return;
    }

    node->blksz = 1;
    node->dev   = 226; /* DRM major number per Linux devices.txt */
    node->rdev  = 0;
    plogk("devtmpfs: Registered /dev/dri/card0 as DRM device (major=226).\n");
    vfs_close(node);
}

void devtmpfs_init(void)
{
    int        status;
    vfs_node_t dev = 0;

    status = vfs_mkdir("/dev");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev: %d\n", status);
        return;
    }

    dev = vfs_open("/dev");
    if (!dev) {
        plogk("devtmpfs: Cannot open /dev after creation.\n");
        return;
    }

    status = vfs_mount(0, dev);
    vfs_close(dev);
    if (status != EOK) {
        plogk("devtmpfs: Cannot mount tmpfs at /dev: %d\n", status);
        return;
    }

    for (uint8_t drive = 0; drive < 4; drive++) {
        mbr_partition_entry_t parts[4] = {0};

        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;

        devtmpfs_create_block_node(drive);

        if (devtmpfs_scan_mbr(drive, parts) == EOK) {
            for (uint8_t part = 0; part < 4; part++) {
                if (!parts[part].type || !parts[part].sectors) continue;
                devtmpfs_create_partition_node(drive, part, &parts[part]);
            }
        }
    }

    devtmpfs_create_input_event_node();
    devtmpfs_create_framebuffer_node();
    devtmpfs_create_audio_nodes();
    devtmpfs_create_tty_nodes();
    devtmpfs_create_drm_node();
}
