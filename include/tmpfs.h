/*
 * 
 *      tmpfs.h
 *      Temporary file system header file
 *
 *      2025/11/3 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TMPFS_H_
#define INCLUDE_TMPFS_H_

#include <vfs.h>

/* Optional device callbacks attached to a tmpfs node. */
typedef size_t (*tmpfs_dev_read_t)(void *ctx, void *addr, size_t offset, size_t size);
typedef size_t (*tmpfs_dev_write_t)(void *ctx, const void *addr, size_t offset, size_t size);
typedef int (*tmpfs_dev_poll_t)(void *ctx, size_t events);
typedef int (*tmpfs_dev_ioctl_t)(void *ctx, size_t req, void *arg);

/* Device operations used to turn a tmpfs node into a device-backed file. */
typedef struct {
        tmpfs_dev_read_t  read;
        tmpfs_dev_write_t write;
        tmpfs_dev_poll_t  poll;
        tmpfs_dev_ioctl_t ioctl;
        void             *ctx;
} tmpfs_device_ops_t;

enum tmpfs_type {
    tp_file_dir,
    tp_file_file,
    tp_file_symlink,
    tp_file_char,
    tp_file_blk,
};

typedef struct {
        enum tmpfs_type type;
        char            name[64];
        char           *data;
        size_t          size;
        vfs_node_t      node;
        vfs_node_t      root;
        size_t          capacity;
        uint16_t        node_type;
        tmpfs_device_ops_t device;
} tmpfs_file_t;

/* Mount the tmpfs file system to a specified VFS node */
int tmpfs_mount(const char *handle, vfs_node_t node);

/* Unmount the tmpfs file system and free related resources */
void tmpfs_umount(void *root);

/* Common function to create a tmpfs file or directory (internal use) */
int tmpfs_mk(void *parent, const char *name, vfs_node_t node, int is_dir);

/* Create a directory in tmpfs */
int tmpfs_mkdir(void *parent, const char *name, vfs_node_t node);

/* Create a regular file in tmpfs */
int tmpfs_mkfile(void *parent, const char *name, vfs_node_t node);

/* Read data from a tmpfs regular file */
size_t tmpfs_read(void *file, void *addr, size_t offset, size_t size);

/* Get file status (type, size) of a tmpfs file/directory */
int tmpfs_stat(void *file, vfs_node_t node);

/* Delete a tmpfs file/directory and free its resources */
int tmpfs_delete(void *parent, vfs_node_t node);

/* Rename a tmpfs file/directory */
int tmpfs_rename(void *current, const char *new_name);

/* Poll a tmpfs file for pending events (simplified implementation) */
int tmpfs_poll(void *file, size_t events);

/* Send control commands to a device or file */
int tmpfs_ioctl(void *file, size_t req, void *arg);

/* Duplicate a VFS node bound to tmpfs */
vfs_node_t tmpfs_dup(vfs_node_t node);

/* Create a symbolic link in tmpfs */
int tmpfs_symlink(void *parent, const char *name, vfs_node_t node);

/* Free resources of a tmpfs file/directory */
int tmpfs_free(void *handle);

/* Dummy function (placeholder for VFS callbacks not implemented) */
void tmpfs_dummy(void);

/* Register tmpfs with the VFS layer (initialize tmpfs) */
void tmpfs_regist(void);

/*
 * Bind device callbacks to a tmpfs-backed node.
 *
 * This keeps the node in tmpfs while routing read/write/poll/ioctl through
 * the supplied callbacks. `node_type` should be set to a VFS device type such
 * as `file_keyboard | file_stream` for /dev/input/event0.
 */
int tmpfs_bind_device(vfs_node_t node, uint16_t node_type, const tmpfs_device_ops_t *device);

#endif // INCLUDE_TMPFS_H_
