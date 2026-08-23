/*
 *
 *      fs_txn.h
 *      Filesystem transaction and write-ahead-log coordination
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FS_TXN_H_
#define INCLUDE_FS_TXN_H_

#include <drivers/block/core/blockdev.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <process/task.h>
#include <sync/spin_lock.h>

#define FS_TXN_METADATA     0x0001U
#define FS_TXN_ORDERED_DATA 0x0002U

typedef struct fs_txn_buffer {
        uint64_t              home_block;
        uint32_t              flags;
        uint8_t              *data;
        struct fs_txn_buffer *next;
} fs_txn_buffer_t;

typedef struct fs_txn_backend_ops {
        int (*recover)(void *context);
        int (*begin)(void *context, uint32_t transaction_id, uint32_t buffers);
        int (*log_block)(void *context, uint32_t transaction_id, uint64_t home_block, const void *data, uint32_t flags);
        int (*commit)(void *context, uint32_t transaction_id);
        int (*checkpoint)(void *context, uint32_t transaction_id);
        void (*abort)(void *context, uint32_t transaction_id, int error);
} fs_txn_backend_ops_t;

typedef struct fs_txn_log {
        blockdev_device_t           device;
        const fs_txn_backend_ops_t *ops;
        void                       *backend_context;
        uint32_t                    block_size;
        uint32_t                    next_transaction_id;
        int                         aborted;
        int                         last_error;
        int                         transaction_active;

        /*
         * Sleepable log mutex.  fs_txn_commit() performs real disk I/O
         * (blockdev_write_bytes / blockdev_flush) that may complete by
         * interrupt; a spinlock would mask IRQs and deadlock the CPU on such
         * a device.  The busy flag is guarded by the brief guard spinlock and
         * contention sleeps on the two-phase wait queue (the vfs_ns_lock()
         * pattern).  All fs_txn callers are process context, so sleeping is
         * legal; the lock is held across the whole commit so no concurrent
         * reader/writer can bypass to the device mid-commit.
         */
        spinlock_t   guard;
        bool         busy;
        wait_queue_t wq;
} fs_txn_log_t;

typedef struct fs_txn {
        fs_txn_log_t    *log;
        fs_txn_buffer_t *buffers;
        fs_txn_buffer_t *tail;
        uint32_t         transaction_id;
        uint32_t         credits;
        uint32_t         used;
        int              error;
        int              active;
} fs_txn_t;

/* Initialize a transaction log bound to a block device. */
int fs_txn_log_init(fs_txn_log_t *log, const blockdev_device_t *device, uint32_t block_size, const fs_txn_backend_ops_t *ops, void *backend_context);

/* Tear down a transaction log and release its device reference. */
void fs_txn_log_destroy(fs_txn_log_t *log);

/* Run backend recovery for the log. */
int fs_txn_recover(fs_txn_log_t *log);

/* Wait for log to become free, using two-phase wait queue. Process context only. */
void fs_txn_log_lock(fs_txn_log_t *log);

/* Release log lock and wake all waiters. */
void fs_txn_log_unlock(fs_txn_log_t *log);

/* Begin a new transaction on the log. */
int fs_txn_begin(fs_txn_log_t *log, uint32_t credits, fs_txn_t *transaction);

/* Stage a whole block for later write-out by the transaction. */
int fs_txn_stage(fs_txn_t *transaction, uint64_t home_block, const void *data, uint32_t flags);

/* Read a whole block, honoring data staged by the transaction. */
int fs_txn_read(fs_txn_t *transaction, uint64_t home_block, void *data);

/* Stage a byte range, reading the surrounding home blocks first. */
int fs_txn_stage_bytes(fs_txn_t *transaction, uint64_t offset, const void *data, size_t size, uint32_t flags);

/* Read a byte range through the transaction. */
int fs_txn_read_bytes(fs_txn_t *transaction, uint64_t offset, void *data, size_t size);

/*
 * The four *_active helpers serve a block/byte-range I/O through the
 * volume's currently-active transaction, if any.  The active-transaction
 * pointer is read and the transaction's buffer list is walked under the log
 * lock, so a concurrent commit/abort cannot clear the pointer and free the
 * buffers while they are in use.  Each returns 0 when no transaction is
 * active (the caller must fall back to a direct device I/O), 1 when the
 * request was served, or a negative errno.  The active-transaction pointer
 * is the fs-private field published by the filesystem (e.g. extfs
 * sb->active_transaction); the filesystem must update it under the same log
 * lock in its begin/commit/abort paths.
 */
int fs_txn_read_active(fs_txn_log_t *log, fs_txn_t **active_pp, uint64_t home_block, void *data);
int fs_txn_stage_active(fs_txn_log_t *log, fs_txn_t **active_pp, uint64_t home_block, const void *data, uint32_t flags);
int fs_txn_read_bytes_active(fs_txn_log_t *log, fs_txn_t **active_pp, uint64_t offset, void *data, size_t size);
int fs_txn_stage_bytes_active(fs_txn_log_t *log, fs_txn_t **active_pp, uint64_t offset, const void *data, size_t size, uint32_t flags);

/* Commit the transaction and write the metadata to its home blocks. */
int fs_txn_commit(fs_txn_t *transaction);

/* Abort the transaction, recording the given error. */
void fs_txn_abort(fs_txn_t *transaction, int error);

/* Return the last error recorded on the log. */
int fs_txn_log_error(const fs_txn_log_t *log);

#endif // INCLUDE_FS_TXN_H_
