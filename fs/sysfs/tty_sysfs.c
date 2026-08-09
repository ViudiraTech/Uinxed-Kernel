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
#include <drivers/tty/tty/tty.h>
#include <fs/sysfs/sysfs.h>
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

    ret = class_register(&tty_class);
    if (ret != EOK) {
        plogk("tty_sysfs: class_register(tty) failed: %d\n", ret);
        return;
    }

    /* Register standard TTY devices */
    /* tty0-7 ?virtual consoles */
    device_create(&tty_class, NULL, MKDEV(4, 0), NULL, "tty0");
    device_create(&tty_class, NULL, MKDEV(4, 1), NULL, "tty1");
    device_create(&tty_class, NULL, MKDEV(4, 2), NULL, "tty2");
    device_create(&tty_class, NULL, MKDEV(4, 3), NULL, "tty3");
    device_create(&tty_class, NULL, MKDEV(4, 4), NULL, "tty4");
    device_create(&tty_class, NULL, MKDEV(4, 5), NULL, "tty5");
    device_create(&tty_class, NULL, MKDEV(4, 6), NULL, "tty6");
    device_create(&tty_class, NULL, MKDEV(4, 7), NULL, "tty7");

    /* ttyS0-3 ?serial ports */
    device_create(&tty_class, NULL, MKDEV(4, 64), NULL, "ttyS0");
    device_create(&tty_class, NULL, MKDEV(4, 65), NULL, "ttyS1");
    device_create(&tty_class, NULL, MKDEV(4, 66), NULL, "ttyS2");
    device_create(&tty_class, NULL, MKDEV(4, 67), NULL, "ttyS3");

    /* Auxiliary tty devices */
    device_create(&tty_class, NULL, MKDEV(5, 0), NULL, "tty");
    device_create(&tty_class, NULL, MKDEV(5, 1), NULL, "console");
    device_create(&tty_class, NULL, MKDEV(5, 2), NULL, "ptmx");
#endif
}
