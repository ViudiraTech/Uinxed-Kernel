/*
 *
 *      vfs.h
 *      Virtual file system header file
 *
 *      2025/11/2 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VFS_H_
#define INCLUDE_VFS_H_

#include <libs/list/circular_list.h>
#include <libs/list/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

#define callbackof(node, _name_) (fs_callbacks[(node)->fsid]->_name_)

/* Filesystem-owned metadata children do not make a directory non-empty. */
#define VFS_NODE_VIRTUAL          (1ULL << 63)
#define VFS_NODE_DELETE_COMMITTED (1ULL << 62)
#define VFS_NODE_DELETE_SYNC      (1ULL << 61)
#define VFS_NODE_UNLINKED         (1ULL << 60)
#define VFS_NODE_FINALIZING       (1ULL << 59)
#define VFS_NODE_UNLINKING        (1ULL << 58)
#define VFS_NODE_INITIALIZING     (1ULL << 57)
#define VFS_NODE_NOCACHE          (1ULL << 56)
#define VFS_NODE_EVENT_DELETE     (1ULL << 55)
#define VFS_NODE_SWAPFILE         (1ULL << 54)
#define VFS_NODE_PARENT_RETAINED  (1ULL << 53)
#define VFS_NODE_RENAME_BUSY      (1ULL << 52)

/* Persistent mount attributes kept on the namespace mount-point node. */
#define MOUNT_FLAG_RDONLY (1ULL << 0)
#define MOUNT_FLAG_NOSUID (1ULL << 1)
#define MOUNT_FLAG_NODEV  (1ULL << 2)
#define MOUNT_FLAG_NOEXEC (1ULL << 3)

typedef struct vfs_node             *vfs_node_t;
typedef struct pagecache_mapping     pagecache_mapping_t;
typedef struct vfs_poll_subscription vfs_poll_subscription_t;
struct process;

#define VFS_RENAME_NOREPLACE (1U << 0)

/*
 * A rename is one filesystem operation, not a delete followed by a move.
 * Supplying both parents and the optional victim lets the backend commit or
 * reject the complete namespace change without destroying the destination on
 * failure.
 */
typedef struct vfs_rename_context {
        vfs_node_t  old_parent;
        vfs_node_t  source;
        vfs_node_t  new_parent;
        vfs_node_t  target;
        const char *new_name;
        uint32_t    flags;
} vfs_rename_context_t;

typedef void (*vfs_poll_notify_t)(vfs_poll_subscription_t *subscription, uint32_t events);

struct vfs_poll_subscription {
        vfs_poll_subscription_t *next;
        vfs_poll_notify_t        notify;
        void                    *context;
        uint32_t                 events;
        bool                     subscribed;
};

typedef struct vfs_poll_source {
        spinlock_t               lock;
        vfs_poll_subscription_t *subscribers;
        bool                     closed;
} vfs_poll_source_t;
struct vm_area; // forward declaration for vfs_file_mmap_t

/* Linux filesystems expose at most 255 bytes in a single pathname component. */
#define VFS_NAME_MAX 255

typedef struct vfs_dirent {
        char     name[VFS_NAME_MAX + 1];
        uint16_t type;
        uint64_t size;
        uint64_t inode;
} vfs_dirent_t;

typedef struct vfs_dir {
        vfs_node_t   node;
        size_t       index;
        vfs_dirent_t entry;
} *vfs_dir_t;

typedef int (*vfs_mount_t)(const char *src, vfs_node_t node);
typedef void (*vfs_umount_t)(void *root);
typedef void (*vfs_open_t)(void *parent, const char *name, vfs_node_t node);
typedef void (*vfs_close_t)(void *current);
typedef int (*vfs_resize_t)(void *current, uint64_t size);
typedef int (*vfs_sync_t)(void *current, int data_only);
typedef int (*vfs_chmod_t)(vfs_node_t node, uint16_t mode);
typedef size_t (*vfs_write_t)(void *file, const void *addr, size_t offset, size_t size);
typedef size_t (*vfs_read_t)(void *file, void *addr, size_t offset, size_t size);
typedef size_t (*vfs_readlink_t)(vfs_node_t node, void *addr, size_t offset, size_t size);
typedef int (*vfs_stat_t)(void *file, vfs_node_t node);
typedef int (*vfs_mk_t)(void *parent, const char *name, vfs_node_t node);
typedef int (*vfs_del_t)(void *parent, vfs_node_t node);
typedef int (*vfs_rename_t)(const vfs_rename_context_t *context);
typedef int (*vfs_ioctl_t)(void *file, size_t req, void *arg);
typedef vfs_node_t (*vfs_dup_t)(vfs_node_t node);
typedef int (*vfs_poll_t)(void *file, size_t events);
typedef int (*vfs_free_t)(void *handle);
typedef void *(*vfs_mmap_t)(void *file, size_t offset, size_t size, int flags);
typedef void *(*vfs_file_mmap_t)(vfs_node_t node, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma);
typedef int (*vfs_file_open_t)(vfs_node_t node, uint64_t flags, void **private_data);
typedef void (*vfs_file_release_t)(vfs_node_t node, void *private_data);
typedef void (*vfs_file_descriptor_close_t)(vfs_node_t node, void *private_data);
typedef int64_t (*vfs_file_read_cb_t)(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
typedef int64_t (*vfs_file_write_cb_t)(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
typedef int64_t (*vfs_file_read_user_cb_t)(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, struct process *proc);
typedef int64_t (*vfs_file_write_user_cb_t)(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, struct process *proc);
typedef int (*vfs_file_ioctl_cb_t)(vfs_node_t node, void *private_data, uint64_t flags, size_t req, void *arg);
typedef int (*vfs_file_poll_cb_t)(vfs_node_t node, void *private_data, uint64_t flags, size_t events);
typedef vfs_poll_source_t *(*vfs_file_poll_source_cb_t)(vfs_node_t node, void *private_data);

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
    file_audio    = 0x8000UL, // Audio device
};

enum {
    VFS_FS_NODEV = 1U << 0, // filesystem has no block-device backing
};

typedef struct vfs_callback {
        vfs_mount_t    mount;                              // Mount the file system
        vfs_umount_t   unmount;                            // Unmount the file system (virtual file systems do not support unmounting)
        vfs_open_t     open;                               // Open a file handle
        vfs_close_t    close;                              // Close a file handle
        vfs_read_t     read;                               // Read a file
        vfs_write_t    write;                              // Write to a file
        vfs_readlink_t readlink;                           // Read a symbolic link
        vfs_mk_t       mkdir;                              // Create a folder
        vfs_mk_t       mkfile;                             // Create a file
        vfs_mk_t       link;                               // Create a hard link
        vfs_mk_t       symlink;                            // Create a symbolic link
        vfs_stat_t     stat;                               // Check file status information
        vfs_ioctl_t    ioctl;                              // I/O control interface (implemented only by special file systems such as devfs)
        vfs_dup_t      dup;                                // Copy file node
        vfs_poll_t     poll;                               // Polling file status (implemented only for special file systems such as devfs)
        vfs_free_t     free;                               // Release file handle
        vfs_del_t delete;                                  // Delete files or folders
        vfs_rename_t                rename;                // Rename files or folders
        vfs_mmap_t                  mmap;                  // Memory-map a device/file into the process address space
        vfs_file_open_t             file_open;             // Per-open-instance allocation callback
        vfs_file_release_t          file_release;          // Per-open-instance teardown callback
        vfs_file_descriptor_close_t file_descriptor_close; // Last descriptor closed
        vfs_file_mmap_t             file_mmap;             // Per-open mmap callback (for GEM, etc.)
        vfs_file_read_cb_t          file_read;             // Per-open read callback
        vfs_file_write_cb_t         file_write;            // Per-open write callback
        vfs_file_read_user_cb_t     file_read_user;        // Direct userspace-buffer read callback
        vfs_file_write_user_cb_t    file_write_user;       // Direct userspace-buffer write callback
        vfs_file_ioctl_cb_t         file_ioctl;            // Per-open ioctl callback
        vfs_file_poll_cb_t          file_poll;             // Per-open poll callback
        vfs_file_poll_source_cb_t   file_poll_source;      // Per-open readiness notification source
        vfs_resize_t                resize;                // Change the persistent file size
        vfs_sync_t                  sync;                  // Commit data and metadata to stable storage
        vfs_chmod_t                 chmod;                 // Validate a permission-mode change
} *vfs_callback_t;

extern vfs_callback_t fs_callbacks[];

typedef struct vfs_node {
        vfs_node_t           parent;       // Parent directory
        vfs_node_t           linkto;       // Node pointed to by the symbolic link
        char                *name;         // Name
        char                *linkname;     // Symbolic link name
        uint64_t             realsize;     // Actual space occupied by the project (optional)
        uint64_t             size;         // File size or 0 if it is a folder
        int64_t              createtime;   // Status-change time (legacy field name)
        int64_t              readtime;     // Last read time
        int64_t              writetime;    // Last write time
        uint64_t             inode;        // Node number
        uint32_t             nlink;        // Number of namespace links to the inode
        uint64_t             blksz;        // Block size
        uint32_t             owner;        // Owner
        uint32_t             group;        // All groups
        uint32_t             permissions;  // Permissions
        uint16_t             type;         // Type
        uint32_t             refcount;     // Reference count
        uint16_t             mode;         // Mode
        uint16_t             fsid;         // File system mount ID
        void                *handle;       // Handle to the file
        uint64_t             flags;        // File flags
        clist_t              child;        // Child nodes
        vfs_node_t           root;         // Root directory
        int                  visited;      // Whether to synchronize with the specific file system
        int                  is_mount;     // Whether it is a mount point
        uint64_t             mount_id;     // Stable namespace mount identifier
        char                *mount_source; // Informational source shown by procfs
        uint64_t             dev;          // Device number
        uint64_t             rdev;         // Real device number
        vfs_poll_source_t    poll_source;
        uint32_t             inotify_watch_count; // Direct inotify watches; avoids global scans for ordinary I/O
        pagecache_mapping_t *mapping;             // Unified cache for regular-file contents
} *vfs_node_t;

extern struct vfs_callback vfs_empty_callback;
extern vfs_node_t          rootdir;

#define VFS_PATH_MAX 4096

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
vfs_node_t vfs_open_checked(const char *str, int *error);

/* Open without following the final pathname component if it is a symlink. */
vfs_node_t vfs_open_nofollow(const char *str);
vfs_node_t vfs_open_nofollow_checked(const char *str, int *error);

/* Build a normalized absolute path from an absolute base and a pathname. */
int vfs_resolve_path(const char *base, const char *path, char *resolved, size_t size);

/* Return the absolute namespace path of a node. */
int vfs_node_path(vfs_node_t node, char *path, size_t size);

/* Retain an already resolved node without consulting the namespace. */
vfs_node_t vfs_node_retain(vfs_node_t node);

#define VFS_ACCESS_R 4
#define VFS_ACCESS_W 2
#define VFS_ACCESS_X 1

/* Check file access permissions against the current process */
int vfs_access_check(vfs_node_t node, uint32_t access_mask);
int vfs_access_check_process(vfs_node_t node, uint32_t access_mask, struct process *proc);

/* Change a file's mode or ownership using Linux permission semantics. */
int vfs_chmod_process(vfs_node_t node, uint16_t mode, struct process *proc);
int vfs_chown_process(vfs_node_t node, uint32_t owner, uint32_t group, struct process *proc);

#define VFS_SET_TIME_ATIME    (1U << 0)
#define VFS_SET_TIME_MTIME    (1U << 1)
#define VFS_SET_TIME_EXPLICIT (1U << 2)

/* Change atime/mtime and advance ctime using Linux ownership rules. */
int vfs_set_times_process(vfs_node_t node, int64_t atime, int64_t mtime, uint32_t flags, struct process *proc);

/* Create a new directory at the specified path */
int vfs_mkdir(const char *name);
int vfs_mkdir_mode(const char *name, uint16_t mode);

/* Create a new file at the specified path */
int vfs_mkfile(const char *name);
int vfs_mkfile_mode(const char *name, uint16_t mode);

/* Read a directory entry by index from the specified directory node */
int vfs_readdir(vfs_node_t dir, size_t index, vfs_dirent_t *entry);

/* Open a directory stream for sequential iteration */
vfs_dir_t vfs_opendir(const char *path);

/* Read the next entry from an open directory stream */
vfs_dirent_t *vfs_readdir_next(vfs_dir_t dir);

/* Close an open directory stream */
int vfs_closedir(vfs_dir_t dir);

/* Create a hard link at the specified path */
int vfs_link(const char *name, const char *target_name);
int vfs_link_follow(const char *name, const char *target_name);

/* Create a symlink at the specified path */
int vfs_symlink(const char *name, const char *target_name);

/* Register a vfs callback */
int vfs_regist(vfs_callback_t callback);

/* Register a vfs callback with a filesystem name */
int vfs_regist_fs(const char *name, vfs_callback_t callback);
int vfs_regist_fs_flags(const char *name, vfs_callback_t callback, uint32_t flags);

/* List the available filesystem names in /proc/filesystems format. */
size_t vfs_format_filesystems(char *buffer, size_t capacity);

/* Return the stable userspace filesystem type registered for an fsid. */
const char *vfs_filesystem_name(uint16_t fsid);

/* Mount a file system to a directory */
int vfs_mount(const char *src, vfs_node_t node);

/* Mount a named file system to a directory */
int vfs_mount_fs(const char *fstype, const char *src, vfs_node_t node);

/* Unmount a file system from a directory */
int vfs_umount(const char *path);

/* Format the current namespace in Linux /proc/mounts or mountinfo syntax. */
size_t vfs_format_mount_table(char *buffer, size_t capacity, bool mountinfo);

/* Read data from a file node into the provided memory buffer */
size_t vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size);

/* Read data from a link file node into the provided memory buffer */
size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize);

/* Write data from the provided memory buffer to a file node */
size_t vfs_write(vfs_node_t file, const void *addr, size_t offset, size_t size);

/* Flush cached contents, truncate a regular file, or invalidate cached data. */
int  vfs_fsync(vfs_node_t file, int data_only);
int  vfs_writeback_range(vfs_node_t file, uint64_t start, uint64_t end, int data_only);
int  vfs_sync_all(void);
int  vfs_truncate(vfs_node_t file, uint64_t size);
int  vfs_invalidate_pages(vfs_node_t file, uint64_t start, uint64_t end, int discard_dirty);
int  vfs_drop_pages(vfs_node_t file, uint64_t start, uint64_t end, int writeback);
int  vfs_readahead(vfs_node_t file, uint64_t offset, size_t size);
int  vfs_cache_mapping_pin(vfs_node_t file);
void vfs_cache_mapping_unpin(vfs_node_t file);
int  vfs_cache_map_page(vfs_node_t file, uint64_t index, int dirty, uint64_t *physical);
int  vfs_cache_mark_dirty_range(vfs_node_t file, uint64_t start, uint64_t end);

/* Per-open operations, falling back to the legacy node callbacks. */
int64_t            vfs_file_read(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
int64_t            vfs_file_write(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
int64_t            vfs_file_read_process(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, struct process *proc);
int64_t            vfs_file_write_process(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, struct process *proc);
int64_t            vfs_file_read_user_process(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, struct process *proc);
int64_t            vfs_file_write_user_process(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, struct process *proc);
int                vfs_file_ioctl(vfs_node_t file, void *private_data, uint64_t flags, size_t req, void *arg);
int                vfs_file_poll(vfs_node_t file, void *private_data, uint64_t flags, size_t events);
int                vfs_mount_is_readonly(vfs_node_t node);
void               vfs_file_descriptor_close(vfs_node_t file, void *private_data);
vfs_poll_source_t *vfs_file_poll_source(vfs_node_t file, void *private_data);

/* Readiness-notification subscriptions on a node or a raw poll source. */
void vfs_poll_subscribe(vfs_node_t file, vfs_poll_subscription_t *subscription, uint32_t events, vfs_poll_notify_t notify, void *context);
void vfs_poll_unsubscribe(vfs_node_t file, vfs_poll_subscription_t *subscription);
void vfs_poll_notify(vfs_node_t file, uint32_t events);
void vfs_poll_source_init(vfs_poll_source_t *source);
void vfs_poll_source_subscribe(vfs_poll_source_t *source, vfs_poll_subscription_t *subscription, uint32_t events, vfs_poll_notify_t notify, void *context);
void vfs_poll_source_unsubscribe(vfs_poll_source_t *source, vfs_poll_subscription_t *subscription);
void vfs_poll_source_notify(vfs_poll_source_t *source, uint32_t events);
void vfs_poll_source_close(vfs_poll_source_t *source, uint32_t events);

/* Close the file or directory node */
int vfs_close(vfs_node_t node);

/* Delete a VFS (Virtual File System) node and clean up associated resources */
int vfs_delete(vfs_node_t node);

/* Remove a node from pathname lookup immediately, retaining open references. */
int vfs_namespace_unlink(vfs_node_t node);

/* Detach a kernel-owned virtual subtree, deferring frees until open references close. */
void vfs_namespace_detach(vfs_node_t node);

/* Atomically rename a node within one mounted filesystem. */
int vfs_rename(vfs_node_t node, vfs_node_t new_parent, const char *new_name, uint32_t flags);

/* Send control commands to a device or file */
int vfs_ioctl(vfs_node_t device, size_t options, void *arg);

/* Listen for actionable events on one or more file descriptors */
int vfs_poll(vfs_node_t node, size_t event);

/* Memory-map a device or file into the process address space */
void *vfs_mmap(vfs_node_t node, size_t offset, size_t size, int flags);

/* Free all child nodes of a VFS node */
void vfs_free_child(vfs_node_t vfs);

/* Free the memory associated with a vfs node */
void vfs_free(vfs_node_t vfs);

/* Initialize the virtual file system */
void init_vfs(void);

#endif // INCLUDE_VFS_H_
