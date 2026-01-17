/*
 * 
 *      vfs.h
 *      Virtual file system header file
 *
 *      2025/11/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VFS_H_
#define INCLUDE_VFS_H_

#include <doubly_list.h>
#include <intrusive_list.h>
#include <stdint.h>

#define callbackof(node, _name_) (fs_callbacks[(node)->fsid]->_name_)

typedef struct vfs_node *vfs_node_t;
typedef int (*vfs_mount_t)(const char *src, vfs_node_t node);
typedef void (*vfs_umount_t)(void *root);
typedef void (*vfs_open_t)(void *parent, const char *name, vfs_node_t node);
typedef void (*vfs_close_t)(void *current);
typedef void (*vfs_resize_t)(void *current, uint64_t size);
typedef size_t (*vfs_write_t)(void *file, const void *addr, size_t offset, size_t size);
typedef size_t (*vfs_read_t)(void *file, void *addr, size_t offset, size_t size);
typedef size_t (*vfs_readlink_t)(vfs_node_t node, void *addr, size_t offset, size_t size);
typedef int (*vfs_stat_t)(void *file, vfs_node_t node);
typedef int (*vfs_mk_t)(void *parent, const char *name, vfs_node_t node);
typedef int (*vfs_del_t)(void *parent, vfs_node_t node);
typedef int (*vfs_rename_t)(void *current, const char *new);
typedef int (*vfs_ioctl_t)(void *file, size_t req, void *arg);
typedef vfs_node_t (*vfs_dup_t)(vfs_node_t node);
typedef int (*vfs_poll_t)(void *file, size_t events);
typedef int (*vfs_free_t)(void *handle);

enum {
    file_none     = 0x1UL,    // No information retrieved
    file_dir      = 0x2UL,    // Folder
    file_block    = 0x4UL,    // Block device, such as hard drive
    file_stream   = 0x8UL,    // Streaming device, such as terminal
    file_symlink  = 0x10UL,   // Symbolic link
    file_fbdev    = 0x20UL,   // Framebuffer device
    file_keyboard = 0x40UL,   // Keyboard device
    file_mouse    = 0x80UL,   // Mouse device
    file_pipe     = 0x100UL,  // Pipe device
    file_socket   = 0x200UL,  // Socket device
    file_epoll    = 0x400UL,  // Epoll device
    file_ptmx     = 0x800UL,  // ptmx device
    file_pts      = 0x1000UL, // pts device
    file_proxy    = 0x2000UL, // Proxy node
    file_delete   = 0x4000UL, // Delete marker (only used during deletion)
};

typedef struct vfs_callback {
        vfs_mount_t    mount;    // Mount the file system
        vfs_umount_t   unmount;  // Unmount the file system (virtual file systems do not support unmounting)
        vfs_open_t     open;     // Open a file handle
        vfs_close_t    close;    // Close a file handle
        vfs_read_t     read;     // Read a file
        vfs_write_t    write;    // Write to a file
        vfs_readlink_t readlink; // Read a symbolic link
        vfs_mk_t       mkdir;    // Create a folder
        vfs_mk_t       mkfile;   // Create a file
        vfs_mk_t       link;     // Create a hard link
        vfs_mk_t       symlink;  // Create a symbolic link
        vfs_stat_t     stat;     // Check file status information
        vfs_ioctl_t    ioctl;    // I/O control interface (implemented only by special file systems such as devfs)
        vfs_dup_t      dup;      // Copy file node
        vfs_poll_t     poll;     // Polling file status (implemented only for special file systems such as devfs)
        vfs_free_t     free;     // Release file handle
        vfs_del_t delete;        // Delete files or folders
        vfs_rename_t rename;     // Rename files or folders
} *vfs_callback_t;

typedef struct vfs_node {
        vfs_node_t parent;      // Parent directory
        vfs_node_t linkto;      // Node pointed to by the symbolic link
        char      *name;        // Name
        char      *linkname;    // Symbolic link name
        uint64_t   realsize;    // Actual space occupied by the project (optional)
        uint64_t   size;        // File size or 0 if it is a folder
        uint64_t   createtime;  // Creation time
        uint64_t   readtime;    // Last read time
        uint64_t   writetime;   // Last write time
        uint64_t   inode;       // Node number
        uint64_t   blksz;       // Block size
        uint32_t   owner;       // Owner
        uint32_t   group;       // All groups
        uint32_t   permissions; // Permissions
        uint16_t   type;        // Type
        uint32_t   refcount;    // Reference count
        uint16_t   mode;        // Mode
        uint16_t   fsid;        // File system mount ID
        void      *handle;      // Handle to the file
        uint64_t   flags;       // File flags
        clist_t    child;       // Child nodes
        vfs_node_t root;        // Root directory
        int        visited;     // Whether to synchronize with the specific file system
        int        is_mount;    // Whether it is a mount point
        uint64_t   dev;         // Device number
        uint64_t   rdev;        // Real device number
} *vfs_node_t;

extern struct vfs_callback vfs_empty_callback;
extern vfs_node_t          rootdir;

/* Allocate a new vfs node with the given parent and name */
vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name);

/* Get the root directory node */
vfs_node_t get_rootdir(void);

/* Set the root directory node of the Virtual File System (VFS) */
void set_rootdir(vfs_node_t node);

/* Search for a file or directory by name in the specified directory */
vfs_node_t vfs_do_search(vfs_node_t dir, const char *name);

/* Update a file or directory, ensuring it is open and ready */
void vfs_update(vfs_node_t node);

/* Open a file or directory by path */
vfs_node_t vfs_open(const char *str);

/* Create a new directory at the specified path */
int vfs_mkdir(const char *name);

/* Create a new file at the specified path */
int vfs_mkfile(const char *name);

/* Create a hard link at the specified path */
int vfs_link(const char *name, const char *target_name);

/* Create a symlink at the specified path */
int vfs_symlink(const char *name, const char *target_name);

/* Register a vfs callback */
int vfs_regist(vfs_callback_t callback);

/* Mount a file system to a directory */
int vfs_mount(const char *src, vfs_node_t node);

/* Unmount a file system from a directory */
int vfs_umount(const char *path);

/* Read data from a file node into the provided memory buffer */
size_t vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size);

/* Read data from a link file node into the provided memory buffer */
size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize);

/* Write data from the provided memory buffer to a file node */
size_t vfs_write(vfs_node_t file, void *addr, size_t offset, size_t size);

/* Close the file or directory node */
int vfs_close(vfs_node_t node);

/* Delete a VFS (Virtual File System) node and clean up associated resources */
int vfs_delete(vfs_node_t node);

/* Rename a VFS (Virtual File System) node to a new name */
int vfs_rename(vfs_node_t node, const char *new);

/* Send control commands to a device or file */
int vfs_ioctl(vfs_node_t device, size_t options, void *arg);

/* Listen for actionable events on one or more file descriptors */
int vfs_poll(vfs_node_t node, size_t event);

/* Free all child nodes of a VFS node */
void vfs_free_child(vfs_node_t vfs);

/* Free the memory associated with a vfs node */
void vfs_free(vfs_node_t vfs);

/* Initialize the virtual file system */
void init_vfs(void);

#endif // INCLUDE_VFS_H_
