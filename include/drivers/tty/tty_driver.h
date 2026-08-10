/*
 *
 *      tty_driver.h
 *      TTY driver registration (Linux drivers/tty/tty_io.c analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_tty_driver_H_
#define INCLUDE_tty_driver_H_

#include <drivers/tty/tty_core.h>
#include <fs/core/vfs.h>
#include <libs/std/stdint.h>

typedef struct tty_driver tty_driver_t;

/* Per-open handle produced by a tty driver; points at the line discipline. */
typedef struct tty_file_endpoint {
        tty_core_t *core;
        bool        virtual_console;
} tty_file_endpoint_t;

typedef int (*tty_drv_open_t)(tty_driver_t *drv, int index, uint64_t flags, void **private_data);
typedef int (*tty_drv_release_t)(tty_driver_t *drv, int index, void *private_data);
typedef int64_t (*tty_drv_read_t)(tty_driver_t *drv, int index, void *private_data, uint64_t flags, void *addr, size_t size);
typedef int64_t (*tty_drv_write_t)(tty_driver_t *drv, int index, void *private_data, uint64_t flags, const void *addr, size_t size);
typedef int (*tty_drv_ioctl_t)(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t req, void *arg);
typedef int (*tty_drv_poll_t)(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t events);

struct tty_driver {
        const char       *name; // driver name, e.g. "tty", "ttyS"
        uint32_t          major;
        uint32_t          minor_start;
        uint32_t          num;       // number of minor slots
        uint16_t          node_type; // file_stream, ...
        uint16_t          mode;      // default node mode (0 = keep)
        tty_drv_open_t    open;
        tty_drv_release_t release;
        tty_drv_read_t    read;
        tty_drv_write_t   write;
        tty_drv_ioctl_t   ioctl;
        tty_drv_poll_t    poll;
        tty_driver_t     *next;
};

/* Register a tty driver and publish a node for every minor slot. */
int tty_register_driver(tty_driver_t *drv);

/* Register a tty driver and publish only the given index as <name>. */
int tty_register_device(tty_driver_t *drv, int index, const char *node_name);

void          tty_unregister_driver(tty_driver_t *drv);
tty_driver_t *tty_driver_for_dev(uint32_t major, uint32_t minor);

/* Iterate every published tty device (used by /sys/class/tty). */
void tty_for_each_registered(int (*cb)(tty_driver_t *drv, int index, const char *name, void *opaque), void *opaque);

/* Create the /dev nodes for every registered tty device. Call after devtmpfs_init. */
int tty_devices_populate(void);

/* Generic per-node dispatch used as the tmpfs device operations. */
int     tty_dispatch_open(struct vfs_node *node, uint64_t flags, void **private_data);
void    tty_dispatch_release(struct vfs_node *node, void *private_data);
int64_t tty_dispatch_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
int64_t tty_dispatch_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
int     tty_dispatch_poll(void *ctx, void *private_data, uint64_t flags, size_t events);
int     tty_dispatch_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg);

#endif // INCLUDE_tty_driver_H_
