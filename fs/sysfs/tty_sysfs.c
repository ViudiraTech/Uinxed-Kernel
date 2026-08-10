/*
 *
 *      tty_sysfs.c
 *      TTY class sysfs integration (/sys/class/tty/)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/core/device.h>
#include <drivers/tty/tty.h>
#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/tty_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

/* ------------------------------------------------------------------ */
/*  TTY class                                                          */
/* ------------------------------------------------------------------ */

static struct class tty_class = {
    .name = "tty",
};

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void tty_sysfs_init(void)
{
#if CONFIG_SYSFS
    int ret;
    int devices = 0;

    ret = class_register(&tty_class);
    if (ret != EOK) {
        plogk("tty_sysfs: Class_register(tty) failed: %d\n", ret);
        return;
    }

    /* Register standard TTY devices */
    /* tty0-7 ?virtual consoles */
    if (device_create(&tty_class, NULL, MKDEV(4, 0), NULL, "tty0")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 1), NULL, "tty1")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 2), NULL, "tty2")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 3), NULL, "tty3")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 4), NULL, "tty4")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 5), NULL, "tty5")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 6), NULL, "tty6")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 7), NULL, "tty7")) devices++;

    /* ttyS0-3 ?serial ports */
    if (device_create(&tty_class, NULL, MKDEV(4, 64), NULL, "ttyS0")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 65), NULL, "ttyS1")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 66), NULL, "ttyS2")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(4, 67), NULL, "ttyS3")) devices++;

    /* Auxiliary tty devices */
    if (device_create(&tty_class, NULL, MKDEV(5, 0), NULL, "tty")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(5, 1), NULL, "console")) devices++;
    if (device_create(&tty_class, NULL, MKDEV(5, 2), NULL, "ptmx")) devices++;

    plogk("tty_sysfs: %d tty device(s) exported to /sys/class/tty\n", devices);
#endif
}
