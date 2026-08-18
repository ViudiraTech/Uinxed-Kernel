/*
 *
 *      syscall.h
 *      System call interface
 *
 *      2026/7/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSCALL_H_
#define INCLUDE_SYSCALL_H_

#include <fs/core/vfs.h>
#include <libs/std/stdint.h>

#define SYSCALL_VECTOR   0x80
#define SYSCALL_PATH_MAX VFS_PATH_MAX

typedef struct linux_timespec64 {
        int64_t tv_sec;
        int64_t tv_nsec;
} linux_timespec64_t;

#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_ACCMODE  0x0003
#define O_CREAT    0x0040
#define O_APPEND   0x0400
#define O_NONBLOCK 0x0800
#define O_CLOEXEC  0x80000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct syscall_frame {
        uint64_t r15;
        uint64_t r14;
        uint64_t r13;
        uint64_t r12;
        uint64_t r11;
        uint64_t r10;
        uint64_t r9;
        uint64_t r8;
        uint64_t rdi;
        uint64_t rsi;
        uint64_t rbp;
        uint64_t rdx;
        uint64_t rcx;
        uint64_t rbx;
        uint64_t rax;
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
} __attribute__((packed)) syscall_frame_t;

typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* Syscall subsystem lifecycle and entry path. */
void syscall_init(void);
void syscall_init_cpu(void);
void syscall_entry(void);
void syscall_return(void);
int  syscall_dispatch(syscall_frame_t *frame);

/* Internal path helpers shared between syscall.c and syscall_basic.c. */
int         copy_path_from_user(uint64_t upath, char path[SYSCALL_PATH_MAX]);
const char *path_basename(const char *path);
vfs_node_t  vfs_open_parent_of(char *path);

/* Create a filesystem node at an already-resolved path, dispatching on mode. */
int64_t mknod_create_node(char *resolved, uint64_t mode, uint64_t dev);

#endif // INCLUDE_SYSCALL_H_
