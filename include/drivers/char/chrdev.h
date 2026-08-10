/*
 *
 *      chrdev.h
 *      Character device registry (Linux fs/char_dev.c analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CHRDEV_H_
#define INCLUDE_CHRDEV_H_

#include <fs/tmpfs/tmpfs.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

/*
 * A static character device. Drivers announce devices they own through
 * cdev_add(); devtmpfs walks the registry to publish the /dev nodes at
 * boot, mirroring how Linux creates device nodes for registered char
 * devices.
 */
typedef struct cdev {
        char                      dir[64];  // /dev-relative parent dir ("" for the root)
        char                      name[64]; // leaf node name, e.g. "null", "ttyS0", "parport0"
        uint32_t                  major;
        uint32_t                  minor_base;
        uint32_t                  count;
        uint16_t                  node_type; // file_stream, file_keyboard, ...
        uint16_t                  mode;      // node mode; 0 selects the driver default
        const tmpfs_device_ops_t *ops;
        struct cdev              *next;
} cdev_t;

/*
 * Register a static character device.
 *
 * @dir:  /dev-relative directory ("" or e.g. "snd", "dri")
 * @name: leaf node name
 * @major/@minor: device number range
 * @count:       number of minors owned
 * @node_type:   VFS type flags (file_stream etc.)
 * @mode:        node permission bits (0 = 0600)
 * @ops:         device operations bound to the node
 */
int cdev_add(const char *dir, const char *name, uint32_t major, uint32_t minor, uint32_t count, uint16_t node_type, uint16_t mode,
             const tmpfs_device_ops_t *ops);

/* Remove a static character device by its full /dev path. */
int cdev_del(const char *path);

/* Look up a registered character device by device number. */
cdev_t *chrdev_lookup(uint32_t major, uint32_t minor);

/* Create every registered /dev node. Returns the number created. */
int chrdev_populate(void);

/* Register the default in-kernel character devices (mem, kmsg, ...). */
void chrdev_init(void);

/* drivers/char/mem.c - /dev/null, zero, full, random, urandom */
void memdev_init(void);

/* drivers/char/kmsg.c - /dev/kmsg */
void kmsgdev_init(void);

#endif // INCLUDE_CHRDEV_H_
