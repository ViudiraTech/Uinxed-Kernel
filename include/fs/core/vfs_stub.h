/*
 *
 *      vfs_stub.h
 *      Canonical no-op VFS callbacks, one per struct vfs_callback slot
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VFS_STUB_H_
#define INCLUDE_VFS_STUB_H_

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <syscall/poll.h>

/*
 * Canonical answers for the struct vfs_callback slots a file system leaves
 * unimplemented.  An empty slot resolves to the shared -ENOSYS sentinel, so a
 * stub here is an explicit answer rather than a synonym for leaving it out.
 * _readonly answers -EROFS, _notty answers -ENOTTY; a file system with any
 * other answer keeps its own callback.
 */

/* Mount a file system that declares no mount operation. */
int vfs_stub_mount(const char *src, vfs_node_t node);

/* Accept an unmount request without tearing anything down. */
void vfs_stub_unmount(void *root);

/* Open a node without allocating per-open state. */
void vfs_stub_open(void *parent, const char *name, vfs_node_t node);

/* Close a node that holds no handle state. */
void vfs_stub_close(void *current);

/* Refuse to read from a node. */
size_t vfs_stub_read(void *file, void *addr, size_t offset, size_t size);

/* Refuse to write to a node. */
size_t vfs_stub_write(void *file, const void *addr, size_t offset, size_t size);

/* Refuse to resolve a symbolic link target. */
size_t vfs_stub_readlink(vfs_node_t node, void *addr, size_t offset, size_t size);

/* Create a directory, file or link in a file system that declares no create operation. */
int vfs_stub_mk(void *parent, const char *name, vfs_node_t node);

/* Create a directory, file or link in a read-only file system. */
int vfs_stub_mk_readonly(void *parent, const char *name, vfs_node_t node);

/* Report that a node carries no status to validate. */
int vfs_stub_stat(void *file, vfs_node_t node);

/* Reject an ioctl on a file system that declares no ioctl operation. */
int vfs_stub_ioctl(void *file, size_t req, void *arg);

/* Reject an ioctl on a node that is not a control device. */
int vfs_stub_ioctl_notty(void *file, size_t req, void *arg);

/* Report that a node handle cannot be duplicated. */
vfs_node_t vfs_stub_dup(vfs_node_t node);

/* Report read and write readiness, ignoring events the node cannot serve. */
int vfs_poll_ready(void *file, size_t events);

/* Report every requested event as ready. */
int vfs_poll_all(void *file, size_t events);

/* Release a node that holds no handle to free. */
int vfs_stub_free(void *handle);

/* Delete a node in a file system that declares no delete operation. */
int vfs_stub_del(void *parent, vfs_node_t node);

/* Delete a node in a read-only file system. */
int vfs_stub_del_readonly(void *parent, vfs_node_t node);

/* Rename a node in a file system that declares no rename operation. */
int vfs_stub_rename(const vfs_rename_context_t *context);

/* Rename a node in a read-only file system. */
int vfs_stub_rename_readonly(const vfs_rename_context_t *context);

/* Report that a node cannot be mapped. */
void *vfs_stub_mmap(void *file, size_t offset, size_t size, int flags);

/* Open a node with no per-open state. */
int vfs_stub_file_open(vfs_node_t node, uint64_t flags, void **private_data);

/* Release per-open state that was never allocated. */
void vfs_stub_file_release(vfs_node_t node, void *private_data);

/* Called when the last descriptor closes; no per-open state to tear down. */
void vfs_stub_file_descriptor_close(vfs_node_t node, void *private_data);

/* Report that a per-open mapping is unavailable. */
void *vfs_stub_file_mmap(vfs_node_t node, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma);

/* Refuse a per-open read. */
int64_t vfs_stub_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);

/* Refuse a per-open write. */
int64_t vfs_stub_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);

/* Refuse to read straight into a userspace buffer. */
int64_t vfs_stub_file_read_user(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, struct process *proc);

/* Refuse to write straight from a userspace buffer. */
int64_t vfs_stub_file_write_user(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, struct process *proc);

/* Reject a per-open ioctl on a node that is not a control device. */
int vfs_stub_file_ioctl(vfs_node_t node, void *private_data, uint64_t flags, size_t req, void *arg);

/* Report that no per-open event is ready. */
int vfs_stub_file_poll(vfs_node_t node, void *private_data, uint64_t flags, size_t events);

/* Report that a node has no readiness-notification source of its own. */
vfs_poll_source_t *vfs_stub_file_poll_source(vfs_node_t node, void *private_data);

/* Refuse to resize a node. */
int vfs_stub_resize(void *current, uint64_t size);

/* Commit nothing; the node has no backing store to flush. */
int vfs_stub_sync(void *current, int data_only);

/* Accept a permission-mode change without a file-system veto. */
int vfs_stub_chmod(vfs_node_t node, uint16_t mode);

#endif // INCLUDE_VFS_STUB_H_
