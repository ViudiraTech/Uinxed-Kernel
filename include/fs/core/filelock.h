/*
 *
 *      filelock.h
 *      Byte-range file locks: POSIX record locks and open file description locks
 *
 *      2026/11/12 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FILELOCK_H_
#define INCLUDE_FILELOCK_H_

#include <fs/core/vfs.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Lock types; the values match F_RDLCK, F_WRLCK and F_UNLCK. */
#define FILELOCK_RDLCK 0
#define FILELOCK_WRLCK 1
#define FILELOCK_UNLCK 2

/* Largest representable offset; a lock whose end is this value extends to EOF. */
#define FILELOCK_OFFSET_MAX INT64_MAX

/* Longest wait-for chain followed before a request is assumed not to deadlock. */
#define FILELOCK_MAX_DEADLK 10

/*
 * Identity of the inode a lock lives on.  Hard links to one file share it, so
 * a lock taken through one name is seen through every other name.
 */
typedef struct filelock_key {
        uintptr_t mount;
        uint64_t  mount_id;
        uint64_t  ino;
        dev_t     dev;
        uint16_t  fsid;
} filelock_key_t;

/*
 * One lock request.  A POSIX lock is owned by the process that took it and an
 * open file description lock by the description, so the two kinds conflict
 * with each other but never with themselves.
 */
typedef struct filelock_request {
        const void *owner; // process_t * for POSIX locks, process_file_t * for OFD locks
        int64_t     start; // first locked byte
        int64_t     end;   // last locked byte, FILELOCK_OFFSET_MAX for "to EOF"
        int32_t     pid;   // process reported to F_GETLK callers
        uint8_t     type;  // FILELOCK_RDLCK, FILELOCK_WRLCK or FILELOCK_UNLCK
        bool        ofd;   // open file description lock
} filelock_request_t;

/* The lock that keeps a request from being granted, as F_GETLK reports it. */
typedef struct filelock_conflict {
        int64_t start;
        int64_t end;
        int32_t pid; // -1 for an open file description lock
        uint8_t type;
} filelock_conflict_t;

/* Build the lock identity of an open node. */
void filelock_key_of(filelock_key_t *key, vfs_node_t node);

/* Report the first lock that would block a request; returns 1 and fills conflict, or 0 when it could be granted. */
int filelock_test(const filelock_key_t *key, const filelock_request_t *request, filelock_conflict_t *conflict);

/*
 * Apply a lock or unlock request.  With wait set the caller sleeps until the
 * request can be granted; POSIX requests that would close a wait-for cycle
 * fail with -EDEADLK.  Returns 0, -EAGAIN, -EDEADLK, -EINTR or -ENOLCK.
 */
int filelock_set(const filelock_key_t *key, const filelock_request_t *request, bool wait);

/* Drop every lock an owner holds on one inode and wake the requests it was blocking. */
void filelock_release(const filelock_key_t *key, const void *owner);

#endif // INCLUDE_FILELOCK_H_
