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

#include <fs/core/vfs.h>
#include <libs/std/stdbool.h>
#include <sync/spin_lock.h>

/* Forward declaration for callback signatures. */
typedef struct vfs_node *vfs_node_t;
struct vm_area;

/* Optional device callbacks attached to a tmpfs node. */
typedef size_t (*tmpfs_dev_read_t)(void *ctx, void *addr, size_t offset, size_t size);
typedef size_t (*tmpfs_dev_write_t)(void *ctx, const void *addr, size_t offset, size_t size);
typedef int (*tmpfs_dev_poll_t)(void *ctx, size_t events);
typedef int (*tmpfs_dev_ioctl_t)(void *ctx, size_t req, void *arg);
typedef int (*tmpfs_dev_open_t)(vfs_node_t node, uint64_t flags, void **private_data);
typedef void (*tmpfs_dev_release_t)(vfs_node_t node, void *private_data);
typedef void *(*tmpfs_dev_mmap_t)(void *ctx, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma);
typedef int64_t (*tmpfs_dev_file_read_t)(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
typedef int64_t (*tmpfs_dev_file_write_t)(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
typedef int (*tmpfs_dev_file_poll_t)(void *ctx, void *private_data, uint64_t flags, size_t events);
typedef int (*tmpfs_dev_file_ioctl_t)(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg);
typedef void (*tmpfs_dev_destroy_t)(void *ctx);

/* Device operations used to turn a tmpfs node into a device-backed file. */
typedef struct {
        tmpfs_dev_read_t       read;
        tmpfs_dev_write_t      write;
        tmpfs_dev_poll_t       poll;
        tmpfs_dev_ioctl_t      ioctl;
        tmpfs_dev_open_t       open;    /* per-open-instance allocation */
        tmpfs_dev_release_t    release; /* per-open-instance teardown */
        tmpfs_dev_mmap_t       mmap;    /* per-open-instance mmap (GEM, etc.) */
        tmpfs_dev_file_read_t  file_read;
        tmpfs_dev_file_write_t file_write;
        tmpfs_dev_file_poll_t  file_poll;
        tmpfs_dev_file_ioctl_t file_ioctl;
        tmpfs_dev_destroy_t    destroy; /* final node teardown */
        void                  *ctx;
} tmpfs_device_ops_t;

enum tmpfs_type {
    tp_file_dir,
    tp_file_file,
    tp_file_symlink,
    tp_file_char,
    tp_file_blk,
};

typedef struct {
        enum tmpfs_type    type;
        char               name[64];
        char              *data;
        size_t             size;
        vfs_node_t         node;
        vfs_node_t         root;
        size_t             capacity;
        bool               data_external;
        spinlock_t         data_lock;
        spinlock_t         link_lock;
        uint32_t           link_count;
        uint16_t           node_type;
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

/* Create another directory entry referring to an existing tmpfs inode. */
int tmpfs_link(void *parent, const char *target_name, vfs_node_t node);

/* Read data from a tmpfs regular file */
size_t tmpfs_read(void *file, void *addr, size_t offset, size_t size);

/* Resize a regular tmpfs file, zero-filling any extension. */
int tmpfs_resize(void *file, uint64_t size);

/*
 * Install immutable, externally owned storage for a regular file.  tmpfs
 * switches to private heap storage on the first write or size change.
 * The caller must keep the supplied storage alive for the lifetime of the
 * file (Limine modules satisfy this requirement).
 */
int tmpfs_adopt_file_data(vfs_node_t node, const void *data, size_t size);

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

/* Change the persistent inode type without replacing tmpfs ownership of its
 * handle.  This is used for namespace-only special files such as AF_UNIX
 * pathname sockets, whose live endpoint is tracked outside the filesystem. */
int tmpfs_set_node_type(vfs_node_t node, uint16_t node_type);

#endif // INCLUDE_TMPFS_H_
