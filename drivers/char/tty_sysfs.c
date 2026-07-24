/*
 *
 *      tty_sysfs.c
 *      TTY class sysfs integration (/sys/class/tty/)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/tty.h>
#include <fs/sysfs.h>
#include <kernel/device.h>
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
    int ret;

    ret = class_register(&tty_class);
    if (ret != EOK) {
        plogk("tty_sysfs: class_register(tty) failed: %d\n", ret);
        return;
    }

    /* Register standard TTY devices */
    /* tty0 — current console */
    device_create(&tty_class, NULL, MKDEV(4, 0), NULL, "tty0");

    /* ttyS0, ttyS1 — serial ports */
    device_create(&tty_class, NULL, MKDEV(4, 64), NULL, "ttyS0");
    device_create(&tty_class, NULL, MKDEV(4, 65), NULL, "ttyS1");
}
