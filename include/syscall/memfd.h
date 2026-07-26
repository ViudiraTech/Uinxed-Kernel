/*
 *
 *      memfd.h
 *      Anonymous in-memory file descriptors
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSCALL_MEMFD_H_
#define INCLUDE_SYSCALL_MEMFD_H_

#include <fs/vfs.h>
#include <libs/std/stdint.h>
#include <proc/process.h>

#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#define MFD_HUGETLB       0x0004U

#define MFD_NAME_MAX 249

void    memfd_init(void);
int64_t sys_memfd_create(uint64_t name, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int     memfd_is_node(vfs_node_t node);
int     memfd_get_seals(vfs_node_t node, uint32_t *seals);
int     memfd_add_seals(vfs_node_t node, uint32_t seals);
int     memfd_resize(vfs_node_t node, uint64_t size);
int     memfd_fallocate(vfs_node_t node, uint32_t mode, uint64_t offset, uint64_t length);
int     memfd_map(vfs_node_t node, process_t *proc, uintptr_t addr, size_t length, uint64_t offset, vm_flags_t flags);
void    memfd_vma_retain(vfs_node_t node, vm_flags_t flags);
void    memfd_vma_release(vfs_node_t node, vm_flags_t flags);
int     memfd_vma_protect(vfs_node_t node, vm_flags_t old_flags, vm_flags_t new_flags);

#endif // INCLUDE_SYSCALL_MEMFD_H_
