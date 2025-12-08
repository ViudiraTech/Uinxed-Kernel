/*
 * 
 *      tmpfs.h
 *      Temporary file system header file
 *
 *      2025/11/3 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TMPFS_H_
#define INCLUDE_TMPFS_H_

#include <vfs.h>

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

#endif // INCLUDE_TMPFS_H_
