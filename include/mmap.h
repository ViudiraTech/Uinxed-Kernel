/*
 *
 *      mmap.h
 *      Memory mapping subsystem header file
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MMAP_H_
#define INCLUDE_MMAP_H_

#include <process.h>
#include <stddef.h>
#include <stdint.h>

/* mmap protection flags */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* mmap mapping flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_GROWSDOWN 0x0100
#define MAP_LOCKED    0x2000
#define MAP_POPULATE  0x8000
#define MAP_NORESERVE 0x4000
#define MAP_STACK     0x20000
#define MAP_HUGETLB   0x40000

/* msync flags */
#define MS_ASYNC      0x01
#define MS_INVALIDATE 0x02
#define MS_SYNC       0x04

/* madvise hints */
#define MADV_NORMAL      0
#define MADV_RANDOM      1
#define MADV_SEQUENTIAL  2
#define MADV_WILLNEED    3
#define MADV_DONTNEED    4
#define MADV_FREE        8
#define MADV_REMOVE      9
#define MADV_DONTFORK    10
#define MADV_DOFORK      11
#define MADV_HWPOISON    100
#define MADV_MERGEABLE   12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE    14
#define MADV_NOHUGEPAGE  15
#define MADV_COLD        20
#define MADV_PAGEOUT     21

/* mlock flags */
#define MCL_CURRENT 0x01
#define MCL_FUTURE  0x02
#define MCL_ONFAULT 0x04

/* ---------- Public API ---------- */

/* Full mmap syscall implementation */
int64_t sys_mmap_pgoff(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t pgoff);

/* munmap */
int sys_munmap_full(uint64_t addr, uint64_t length);

/* mprotect */
int sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot);

/* msync */
int sys_msync(uint64_t addr, uint64_t length, uint64_t flags);

/* madvise */
int sys_madvise(uint64_t addr, uint64_t length, uint64_t advice);

/* mlock */
int sys_mlock(uint64_t addr, uint64_t length);

/* munlock */
int sys_munlock(uint64_t addr, uint64_t length);

/* mlockall */
int sys_mlockall(uint64_t flags);

/* munlockall */
int sys_munlockall(void);

/* mremap */
int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr);

/* mincore */
int sys_mincore(uint64_t addr, uint64_t length, uint64_t vec);

/* Initialize the mmap subsystem */
void mmap_init(void);

#endif /* INCLUDE_MMAP_H_ */