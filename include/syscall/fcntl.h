/*
 *
 *      fcntl.h
 *      fcntl syscall definitions
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSCALL_FCNTL_H_
#define INCLUDE_SYSCALL_FCNTL_H_

#include <libs/std/stdint.h>

/* fcntl commands */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_SETOWN        8
#define F_GETOWN        9
#define F_SETSIG        10
#define F_GETSIG        11
#define F_SETOWN_EX     15
#define F_GETOWN_EX     16
#define F_DUPFD_CLOEXEC 1030

/* FD flags (for F_GETFD/F_SETFD) */
#define FD_CLOEXEC 1

/* File status flags for F_GETFL/F_SETFL */
#define O_ACCMODE   0x0003
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DSYNC     0x1000
#define O_SYNC      0x101000
#define O_RSYNC     0x101000
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_CLOEXEC   0x80000
#define O_PATH      0x200000

/* Lock types for F_SETLK/F_SETLKW */
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* Owner types for F_SETOWN_EX/F_GETOWN_EX */
#define F_OWNER_TID  0
#define F_OWNER_PID  1
#define F_OWNER_PGRP 2

/* Signal numbers for F_SETSIG/F_GETSIG */
#define F_SETSIG 10
#define F_GETSIG 11

/* Syscall implementation */
int64_t sys_fcntl(int fd, int cmd, uint64_t arg);

#endif /* INCLUDE_SYSCALL_FCNTL_H_ */
