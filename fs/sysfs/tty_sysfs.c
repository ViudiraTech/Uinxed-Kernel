/*
 *
 *      tty_sysfs.c
 *      TTY class sysfs integration (/sys/class/tty/)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/tty/tty_driver.h>
#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/tty_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

/* TTY class */

static struct class tty_class = {
    .name = "tty",
};

typedef struct {
        struct class *cls;
        int           devices;
} tty_sysfs_ctx_t;

static int tty_sysfs_add_device(tty_driver_t *drv, int index, const char *name, void *opaque)
{
    tty_sysfs_ctx_t *ctx   = opaque;
    uint32_t         minor = drv->minor_start + (uint32_t)index;

    if (device_create(ctx->cls, NULL, MKDEV(drv->major, minor), NULL, name)) ctx->devices++;
    return 0;
}

/* Initialization */

/* Export every registered tty device to /sys/class/tty/. */
void tty_sysfs_init(void)
{
#if CONFIG_SYSFS
    int             ret;
    tty_sysfs_ctx_t ctx;

    ret = class_register(&tty_class);
    if (ret != EOK) {
        plogk("tty_sysfs: Class_register(tty) failed: %d\n", ret);
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.cls = &tty_class;
    tty_for_each_registered(tty_sysfs_add_device, &ctx);

    /* Auxiliary tty device exported by the pty subsystem. */
    if (device_create(&tty_class, NULL, MKDEV(5, 2), NULL, "ptmx")) ctx.devices++;

    plogk("tty_sysfs: %d tty device(s) exported to /sys/class/tty\n", ctx.devices);
#endif
}
