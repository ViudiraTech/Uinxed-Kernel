/*
 *
 *      ppdev.c
 *      /dev/parportN character device (Linux drivers/parport/ppdev.c analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/base/device.h>
#include <drivers/char/chrdev.h>
#include <drivers/parport/parport.h>
#include <kernel/errno.h>
#include <kernel/termios.h>
#include <libs/std/stdint.h>
#include <mem/heap.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>

#define PP_MAJOR 99
#define PP_IOCTL 'p'

#define PPSETMODE   _IOW(PP_IOCTL, 0x80, int)
#define PPRSTATUS   _IOR(PP_IOCTL, 0x81, unsigned char)
#define PPCONTROL   _IOR(PP_IOCTL, 0x83, unsigned char)
#define PPWCONTROL  _IOW(PP_IOCTL, 0x84, unsigned char)
#define PPFWDATA    _IOW(PP_IOCTL, 0x85, unsigned char)
#define PPWDATA     _IOW(PP_IOCTL, 0x86, unsigned char)
#define PPRDATA     _IOR(PP_IOCTL, 0x87, unsigned char)
#define PPGETMODE   _IOR(PP_IOCTL, 0x95, int)
#define PPCLAIM     _IO(PP_IOCTL, 0x8b)
#define PPRELEASE   _IO(PP_IOCTL, 0x8c)
#define PPYIELD     _IO(PP_IOCTL, 0x8d)
#define PPEXCL      _IO(PP_IOCTL, 0x8f)
#define PPDATADIR   _IOW(PP_IOCTL, 0x90, int)
#define PPNEGOT     _IOW(PP_IOCTL, 0x91, int)
#define PPSETFLAGS  _IOW(PP_IOCTL, 0x9a, int)
#define PPGETFLAGS  _IOR(PP_IOCTL, 0x9b, int)
#define PPFCONTROL  _IOW(PP_IOCTL, 0x9e, struct ppdev_frob_struct)
#define PP_MODE_SPP 0

struct ppdev_frob_struct {
        unsigned char mask;
        unsigned char val;
};

typedef struct ppdev_file {
        parport_t *port;
        bool       claimed;
        bool       exclusive;
} ppdev_file_t;

static int64_t ppdev_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    ppdev_file_t *file = private_data;
    (void)ctx;
    (void)flags;
    (void)offset;
    if (!file || !file->port) return -EIO;
    if (!addr || !size) return 0;
    uint8_t status   = parport_read_status(file->port);
    *(uint8_t *)addr = status;
    return 1;
}

static int64_t ppdev_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    ppdev_file_t  *file = private_data;
    const uint8_t *in   = addr;
    (void)ctx;
    (void)flags;
    (void)offset;
    if (!file || !file->port) return -EIO;
    if (!addr || !size) return 0;
    for (size_t i = 0; i < size; i++) parport_write_data(file->port, in[i]);
    return (int64_t)size;
}

static int ppdev_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    return (int)(events & 0x005);
}

static int ppdev_claim(ppdev_file_t *file, bool force)
{
    parport_t *p = file->port;

    if (!p) return -EIO;
    if (file->claimed) return 0;
    if (!force && p->claimed) return -EBUSY;
    p->claimed    = true;
    file->claimed = true;
    return 0;
}

static int ppdev_release(ppdev_file_t *file)
{
    parport_t *p = file->port;

    if (!p) return -EIO;
    if (file->claimed) {
        p->claimed    = false;
        file->claimed = false;
    }
    return 0;
}

static int ppdev_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *arg)
{
    ppdev_file_t *file = private_data;
    parport_t    *p    = file ? file->port : NULL;
    unsigned char value;
    int           int_value;

    (void)ctx;
    (void)flags;
    if (!p) return -EIO;

    switch (request) {
        case PPCLAIM :
            return ppdev_claim(file, false);
        case PPRELEASE :
            return ppdev_release(file);
        case PPYIELD : {
            int status = ppdev_release(file);
            if (status) return status;
            return ppdev_claim(file, false);
        }
        case PPEXCL :
            file->exclusive = true;
            return 0;
        case PPRSTATUS :
            value = parport_read_status(p);
            return copy_to_user(arg, &value, sizeof(value)) ? -EFAULT : 0;
        case PPRDATA :
            value = parport_read_data(p);
            return copy_to_user(arg, &value, sizeof(value)) ? -EFAULT : 0;
        case PPCONTROL :
            value = parport_read_control(p);
            return copy_to_user(arg, &value, sizeof(value)) ? -EFAULT : 0;
        case PPWDATA :
            if (copy_from_user(&value, arg, sizeof(value))) return -EFAULT;
            parport_write_data(p, value);
            return 0;
        case PPWCONTROL :
            if (copy_from_user(&value, arg, sizeof(value))) return -EFAULT;
            parport_write_control(p, value);
            return 0;
        case PPFCONTROL : {
            struct ppdev_frob_struct frob;
            if (copy_from_user(&frob, arg, sizeof(frob))) return -EFAULT;
            parport_frob_control(p, frob.mask, frob.val);
            return 0;
        }
        case PPDATADIR :
            if (copy_from_user(&int_value, arg, sizeof(int_value))) return -EFAULT;
            parport_data_reverse(p, int_value != 0);
            return 0;
        case PPFWDATA :
            if (copy_from_user(&value, arg, sizeof(value))) return -EFAULT;
            parport_data_reverse(p, false);
            parport_write_data(p, value);
            return 0;
        case PPNEGOT :
            if (copy_from_user(&int_value, arg, sizeof(int_value))) return -EFAULT;
            return int_value == PP_MODE_SPP ? 0 : -ENXIO;
        case PPSETMODE :
            if (copy_from_user(&int_value, arg, sizeof(int_value))) return -EFAULT;
            return int_value == PP_MODE_SPP ? 0 : -ENXIO;
        case PPGETMODE :
            int_value = PP_MODE_SPP;
            return copy_to_user(arg, &int_value, sizeof(int_value)) ? -EFAULT : 0;
        case PPSETFLAGS :
        case PPGETFLAGS :
            return 0;
        default :
            return -ENOTTY;
    }
}

static int ppdev_open(struct vfs_node *node, uint64_t open_flags, void **private_data)
{
    ppdev_file_t *file;

    (void)open_flags;
    if (!node) return -ENXIO;
    file = calloc(1, sizeof(*file));
    if (!file) return -ENOMEM;
    file->port = parport_find_by_number(MINOR(node->rdev));
    if (!file->port) {
        free(file);
        return -ENODEV;
    }
    *private_data = file;
    return 0;
}

static void ppdev_release_node(struct vfs_node *node, void *private_data)
{
    ppdev_file_t *file = private_data;
    (void)node;
    if (!file) return;
    if (file->claimed && file->port) file->port->claimed = false;
    free(file);
}

static const tmpfs_device_ops_t ppdev_operations = {
    .open       = ppdev_open,
    .release    = ppdev_release_node,
    .file_read  = ppdev_read,
    .file_write = ppdev_write,
    .file_poll  = ppdev_poll,
    .file_ioctl = ppdev_ioctl,
};

void ppdev_init(void)
{
    char name[16];

    for (int i = 0; i < parport_count(); i++) {
        parport_t *p = parport_get(i);
        if (!p) continue;
        (void)snprintf(name, sizeof(name), "parport%d", p->number);
        (void)cdev_add("", name, PP_MAJOR, p->number, 1, file_stream, 0666, &ppdev_operations);
    }
}
