/*
 *
 *      i2c_sysfs.c
 *      I2C bus and i2c-dev userspace interface
 *
 *      2026/8/6 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/bus/i2c.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/sysfs/i2c_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/uaccess.h>

#define I2C_DEV_MAJOR 89

#define I2C_SLAVE 0x0703
#define I2C_FUNCS 0x0705
#define I2C_RDWR  0x0707

#define I2C_RDWR_MAX_MSGS    42
#define I2C_RDWR_MAX_BUFSIZE 8192
#define I2C_MAX_ADAPTERS     16

/* User-space ABI structures (identical to the kernel layout). */
typedef struct {
        uint16_t addr;
        uint16_t flags;
        uint16_t len;
        uint8_t *buf;
} i2c_msg_user_t;

typedef struct {
        i2c_msg_user_t *msgs;
        uint32_t        nmsgs;
} i2c_rdwr_ioctl_data_t;

typedef struct {
        struct i2c_adapter *adap;
        struct device       dev;
} i2c_adapter_dev_t;

static struct bus_type i2c_bus_type = {.name = "i2c", .dev_name = "i2c"};
static struct class i2c_dev_class   = {.name = "i2c-dev"};
static bool i2c_sysfs_ready;

static struct {
        struct i2c_adapter *adap;
        i2c_adapter_dev_t  *adev;
} i2c_sysfs_adapters[I2C_MAX_ADAPTERS];

/* Free an i2c adapter sysfs device. */
static void i2c_adapter_release(struct device *dev)
{
    i2c_adapter_dev_t *adev = (i2c_adapter_dev_t *)((char *)dev - offsetof(i2c_adapter_dev_t, dev));
    free(adev);
}

/* i2c-dev character device */

typedef struct {
        uint16_t slave_addr;
} i2c_dev_state_t;

/* Allocate per-open state for the i2c-dev character device. */
static int i2c_dev_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    i2c_dev_state_t *state = calloc(1, sizeof(*state));
    (void)node;
    (void)flags;
    if (!state) return -ENOMEM;
    state->slave_addr = 0xFFFF;
    *private_data     = state;
    return 0;
}

/* Free the per-open state of the i2c-dev character device. */
static void i2c_dev_release(vfs_node_t node, void *private_data)
{
    (void)node;
    free(private_data);
}

/* Read bytes from the configured i2c slave device. */
static int64_t i2c_dev_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    i2c_dev_state_t    *state = private_data;
    struct i2c_adapter *adap  = ctx;
    struct i2c_msg      msg;
    uint8_t            *buffer;
    int                 ret;
    (void)flags;
    (void)offset;
    if (!state || !adap || state->slave_addr > 0x7f || !addr || !size || size > I2C_RDWR_MAX_BUFSIZE) return -EINVAL;
    buffer = malloc(size);
    if (!buffer) return -ENOMEM;
    msg.addr  = state->slave_addr;
    msg.flags = I2C_M_RD;
    msg.len   = (uint16_t)size;
    msg.buf   = buffer;
    ret       = i2c_transfer(adap, &msg, 1);
    if (ret >= 0 && copy_to_user(addr, buffer, size)) ret = -EFAULT;
    free(buffer);
    return ret < 0 ? (int64_t)ret : (int64_t)size;
}

/* Write bytes to the configured i2c slave device. */
static int64_t i2c_dev_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    i2c_dev_state_t    *state = private_data;
    struct i2c_adapter *adap  = ctx;
    struct i2c_msg      msg;
    uint8_t            *buffer;
    int                 ret;
    (void)flags;
    (void)offset;
    if (!state || !adap || state->slave_addr > 0x7f || !addr || !size || size > I2C_RDWR_MAX_BUFSIZE) return -EINVAL;
    buffer = malloc(size);
    if (!buffer) return -ENOMEM;
    if (copy_from_user(buffer, addr, size)) {
        free(buffer);
        return -EFAULT;
    }
    msg.addr  = state->slave_addr;
    msg.flags = 0;
    msg.len   = (uint16_t)size;
    msg.buf   = buffer;
    ret       = i2c_transfer(adap, &msg, 1);
    free(buffer);
    return ret < 0 ? (int64_t)ret : (int64_t)size;
}

/* Handle the i2c-dev ioctl commands. */
static int i2c_dev_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *argument)
{
    i2c_dev_state_t    *state = private_data;
    struct i2c_adapter *adap  = ctx;
    int                 ret;
    (void)flags;

    if (!state || !adap) return -EINVAL;

    switch (request) {
        case I2C_SLAVE : {
            uint16_t addr;
            if (copy_from_user(&addr, argument, sizeof(addr))) return -EFAULT;
            if (addr > 0x3ff) return -EINVAL;
            state->slave_addr = addr;
            return 0;
        }
        case I2C_FUNCS : {
            uint32_t funcs = I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
            return copy_to_user(argument, &funcs, sizeof(funcs)) ? -EFAULT : 0;
        }
        case I2C_RDWR : {
            i2c_rdwr_ioctl_data_t data;
            struct i2c_msg       *kmsgs = NULL;
            uint8_t             **bufs  = NULL;
            uint32_t              i     = 0;

            if (copy_from_user(&data, argument, sizeof(data))) return -EFAULT;
            if (!data.msgs || data.nmsgs > I2C_RDWR_MAX_MSGS) return -EINVAL;

            kmsgs = calloc(data.nmsgs, sizeof(*kmsgs));
            bufs  = calloc(data.nmsgs, sizeof(*bufs));
            if (!kmsgs || !bufs) {
                free(kmsgs);
                free(bufs);
                return -ENOMEM;
            }
            for (; i < data.nmsgs; i++) {
                i2c_msg_user_t user;
                if (copy_from_user(&user, &data.msgs[i], sizeof(user))) {
                    ret = -EFAULT;
                    goto rdwr_out;
                }
                if (user.len > I2C_RDWR_MAX_BUFSIZE) {
                    ret = -EINVAL;
                    goto rdwr_out;
                }
                bufs[i] = malloc(user.len ? user.len : 1);
                if (!bufs[i]) {
                    ret = -ENOMEM;
                    goto rdwr_out;
                }
                if (user.len && copy_from_user(bufs[i], user.buf, user.len)) {
                    ret = -EFAULT;
                    goto rdwr_out;
                }
                kmsgs[i].addr  = user.addr;
                kmsgs[i].flags = user.flags;
                kmsgs[i].len   = user.len;
                kmsgs[i].buf   = bufs[i];
            }
            ret = i2c_transfer(adap, kmsgs, (int)data.nmsgs);
            if (ret >= 0) ret = (int)data.nmsgs;
rdwr_out:
            for (uint32_t j = 0; j < i; j++) free(bufs[j]);
            free(bufs);
            free(kmsgs);
            return ret;
        }
        default :
            return -ENOTTY;
    }
}

/* Adapter lifecycle hooks */

/* Publish an i2c adapter as a sysfs device and /dev/i2c-N node. */
int i2c_sysfs_adapter_add(struct i2c_adapter *adap)
{
    i2c_adapter_dev_t *adev;
    char               node_path[32];
    char               dev_name[16];
    int                status;
    int                slot = -1;

#if !CONFIG_SYSFS
    (void)adap;
    return -ENOSYS;
#else
    if (!i2c_sysfs_ready || !adap) return -EINVAL;
    for (int i = 0; i < I2C_MAX_ADAPTERS; i++) {
        if (i2c_sysfs_adapters[i].adap == adap) return -EEXIST;
        if (!i2c_sysfs_adapters[i].adap && slot < 0) slot = i;
    }
    if (slot < 0) return -ENOSPC;

    adev = calloc(1, sizeof(*adev));
    if (!adev) return -ENOMEM;
    adev->adap        = adap;
    adev->dev.bus     = &i2c_bus_type;
    adev->dev.devid   = (uint64_t)adap->nr;
    adev->dev.release = i2c_adapter_release;
    (void)snprintf(dev_name, sizeof(dev_name), "i2c-%d", adap->nr);
    status = kobject_set_name(&adev->dev.kobj, "%s", adap->name);
    if (status != EOK || device_register(&adev->dev) != EOK) {
        free(adev);
        return -ENOMEM;
    }
    i2c_sysfs_adapters[slot].adap = adap;
    i2c_sysfs_adapters[slot].adev = adev;

    (void)device_create(&i2c_dev_class, &adev->dev, MKDEV(I2C_DEV_MAJOR, (uint32_t)adap->nr), adap, "%s", dev_name);

    (void)snprintf(node_path, sizeof(node_path), "/dev/i2c-%d", adap->nr);
    tmpfs_device_ops_t ops = {
        .open       = i2c_dev_open,
        .release    = i2c_dev_release,
        .file_read  = i2c_dev_read,
        .file_write = i2c_dev_write,
        .file_ioctl = i2c_dev_ioctl,
        .ctx        = adap,
    };
    (void)devtmpfs_register_char_device(node_path, MKDEV(I2C_DEV_MAJOR, (uint32_t)adap->nr), MKDEV(I2C_DEV_MAJOR, (uint32_t)adap->nr), file_stream, &ops);
    return EOK;
#endif
}

/* Remove an i2c adapter from sysfs and devtmpfs. */
void i2c_sysfs_adapter_del(struct i2c_adapter *adap)
{
    i2c_adapter_dev_t *adev = NULL;
    int                slot = -1;
    char               node_path[32];

#if !CONFIG_SYSFS
    (void)adap;
    return;
#else
    for (int i = 0; i < I2C_MAX_ADAPTERS; i++) {
        if (i2c_sysfs_adapters[i].adap == adap) {
            slot = i;
            adev = i2c_sysfs_adapters[i].adev;
            break;
        }
    }
    if (slot < 0) return;
    i2c_sysfs_adapters[slot].adap = NULL;
    i2c_sysfs_adapters[slot].adev = NULL;

    (void)snprintf(node_path, sizeof(node_path), "/dev/i2c-%d", adap->nr);
    (void)devtmpfs_unregister_char_device(node_path);
    if (adev) device_unregister(&adev->dev);
#endif
}

/* Registration */

/* Register the i2c bus and i2c-dev class. */
void i2c_sysfs_init(void)
{
#if CONFIG_SYSFS
    if (i2c_sysfs_ready) return;
    if (bus_register(&i2c_bus_type) != EOK) {
        plogk("i2c_sysfs: Bus_register(i2c) failed.\n");
        return;
    }
    if (class_register(&i2c_dev_class) != EOK) {
        plogk("i2c_sysfs: Class_register(i2c-dev) failed.\n");
        return;
    }
    i2c_sysfs_ready = true;
    plogk("i2c_sysfs: registered /sys/bus/i2c and /sys/class/i2c-dev\n");
#endif
}
