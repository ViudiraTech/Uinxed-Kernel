/*
 *
 *      vfs_stub.c
 *      Canonical no-op VFS callbacks, one per struct vfs_callback slot
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs_stub.h>

/* Mount a file system that declares no mount operation. */
int vfs_stub_mount(const char *src, vfs_node_t node)
{
    (void)src;
    (void)node;
    return -ENOSYS;
}

/* Accept an unmount request without tearing anything down. */
void vfs_stub_unmount(void *root)
{
    (void)root;
}

/* Open a node without allocating per-open state. */
void vfs_stub_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

/* Close a node that holds no handle state. */
void vfs_stub_close(void *current)
{
    (void)current;
}

/* Refuse to read from a node. */
size_t vfs_stub_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/* Refuse to write to a node. */
size_t vfs_stub_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/* Refuse to resolve a symbolic link target. */
size_t vfs_stub_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    (void)node;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/* Create a directory, file or link in a file system that declares no create operation. */
int vfs_stub_mk(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -ENOSYS;
}

/* Create a directory, file or link in a read-only file system. */
int vfs_stub_mk_readonly(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

/* Report that a node carries no status to validate. */
int vfs_stub_stat(void *file, vfs_node_t node)
{
    (void)file;
    (void)node;
    return EOK;
}

/* Reject an ioctl on a file system that declares no ioctl operation. */
int vfs_stub_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOSYS;
}

/* Reject an ioctl on a node that is not a control device. */
int vfs_stub_ioctl_notty(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOTTY;
}

/* Report that a node handle cannot be duplicated. */
vfs_node_t vfs_stub_dup(vfs_node_t node)
{
    (void)node;
    return NULL;
}

/* Report read and write readiness, ignoring events the node cannot serve. */
int vfs_poll_ready(void *file, size_t events)
{
    (void)file;
    int revents = 0;
    if (events & POLLIN) revents |= POLLIN;
    if (events & POLLOUT) revents |= POLLOUT;
    return revents;
}

/* Report every requested event as ready. */
int vfs_poll_all(void *file, size_t events)
{
    (void)file;
    return (int)events;
}

/* Release a node that holds no handle to free. */
int vfs_stub_free(void *handle)
{
    (void)handle;
    return EOK;
}

/* Delete a node in a file system that declares no delete operation. */
int vfs_stub_del(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -ENOSYS;
}

/* Delete a node in a read-only file system. */
int vfs_stub_del_readonly(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -EROFS;
}

/* Rename a node in a file system that declares no rename operation. */
int vfs_stub_rename(const vfs_rename_context_t *context)
{
    (void)context;
    return -ENOSYS;
}

/* Rename a node in a read-only file system. */
int vfs_stub_rename_readonly(const vfs_rename_context_t *context)
{
    (void)context;
    return -EROFS;
}

/* Report that a node cannot be mapped. */
void *vfs_stub_mmap(void *file, size_t offset, size_t size, int flags)
{
    (void)file;
    (void)offset;
    (void)size;
    (void)flags;
    return NULL;
}

/* Open a node with no per-open state. */
int vfs_stub_file_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    (void)node;
    (void)flags;
    (void)private_data;
    return EOK;
}

/* Release per-open state that was never allocated. */
void vfs_stub_file_release(vfs_node_t node, void *private_data)
{
    (void)node;
    (void)private_data;
}

/* Called when the last descriptor closes; no per-open state to tear down. */
void vfs_stub_file_descriptor_close(vfs_node_t node, void *private_data)
{
    (void)node;
    (void)private_data;
}

/* Report that a per-open mapping is unavailable. */
void *vfs_stub_file_mmap(vfs_node_t node, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    (void)node;
    (void)private_data;
    (void)offset;
    (void)size;
    (void)flags;
    (void)vma;
    return NULL;
}

/* Refuse a per-open read. */
int64_t vfs_stub_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)node;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    return -EINVAL;
}

/* Refuse a per-open write. */
int64_t vfs_stub_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)node;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    return -EINVAL;
}

/* Refuse to read straight into a userspace buffer. */
int64_t vfs_stub_file_read_user(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, struct process *proc)
{
    (void)node;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    (void)proc;
    return -EINVAL;
}

/* Refuse to write straight from a userspace buffer. */
int64_t vfs_stub_file_write_user(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, struct process *proc)
{
    (void)node;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    (void)proc;
    return -EINVAL;
}

/* Reject a per-open ioctl on a node that is not a control device. */
int vfs_stub_file_ioctl(vfs_node_t node, void *private_data, uint64_t flags, size_t req, void *arg)
{
    (void)node;
    (void)private_data;
    (void)flags;
    (void)req;
    (void)arg;
    return -ENOTTY;
}

/* Report that no per-open event is ready. */
int vfs_stub_file_poll(vfs_node_t node, void *private_data, uint64_t flags, size_t events)
{
    (void)node;
    (void)private_data;
    (void)flags;
    (void)events;
    return 0;
}

/* Report that a node has no readiness-notification source of its own. */
vfs_poll_source_t *vfs_stub_file_poll_source(vfs_node_t node, void *private_data)
{
    (void)node;
    (void)private_data;
    return NULL;
}

/* Refuse to resize a node. */
int vfs_stub_resize(void *current, uint64_t size)
{
    (void)current;
    (void)size;
    return -EOPNOTSUPP;
}

/* Commit nothing; the node has no backing store to flush. */
int vfs_stub_sync(void *current, int data_only)
{
    (void)current;
    (void)data_only;
    return EOK;
}

/* Accept a permission-mode change without a file-system veto. */
int vfs_stub_chmod(vfs_node_t node, uint16_t mode)
{
    (void)node;
    (void)mode;
    return EOK;
}
