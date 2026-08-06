/*
 *
 *      mem_sysfs.c
 *      /sys/class/mem/ - standard memory character devices
 *
 *      2026/8/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/core/device.h>
#include <fs/sysfs/mem_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/string.h>

static bool mem_class_ready;

static struct class mem_class = {
    .name = "mem",
};

static const struct {
        const char *name;
        uint8_t     minor;
} mem_devices[] = {
    {"null",    3},
    {"zero",    5},
    {"full",    7},
    {"random",  8},
    {"urandom", 9},
};

void mem_sysfs_init(void)
{
#if CONFIG_SYSFS
    if (mem_class_ready) return;
    if (class_register(&mem_class) != EOK) {
        plogk("mem_sysfs: class_register(mem) failed\n");
        return;
    }
    mem_class_ready = true;
    for (size_t i = 0; i < sizeof(mem_devices) / sizeof(mem_devices[0]); i++)
        (void)device_create(&mem_class, NULL, MKDEV(1, mem_devices[i].minor), NULL, "%s", mem_devices[i].name);
#endif
}
