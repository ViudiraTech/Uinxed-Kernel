/*
 *
 *      syscall.h
 *      System call interface
 *
 *      2026/7/20 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSCALL_H_
#define INCLUDE_SYSCALL_H_

#include <stdint.h>

#define SYSCALL_VECTOR 0x80

enum {
    SYS_EXIT = 0,
    SYS_GETPID,
    SYS_YIELD,
    SYS_SLEEP,
    SYS_FORK,
    SYS_WAIT,
    SYS_KILL,
    SYS_MMAP,
    SYS_MUNMAP,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_READ,
    SYS_WRITE,
    SYS_LSEEK,
    SYS_DUP,
    SYS_DUP2,
    SYS_IOCTL,
    SYS_POLL,
    SYS_MAX,
};

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_ACCMODE 0x0003
#define O_CREAT   0x0040
#define O_APPEND  0x0400

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

void syscall_init(void);
void syscall_entry(void);
void syscall_return(void);
void syscall_dispatch(syscall_frame_t *frame);

#endif // INCLUDE_SYSCALL_H_
