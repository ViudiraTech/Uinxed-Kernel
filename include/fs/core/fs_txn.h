/*
 *
 *      fs_txn.h
 *      Filesystem transaction and write-ahead-log coordination
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_fs_txn_H_
#define INCLUDE_fs_txn_H_

#include <drivers/block/core/blockdev.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
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
        spinlock_t                  lock;
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

int  fs_txn_log_init(fs_txn_log_t *log, const blockdev_device_t *device, uint32_t block_size, const fs_txn_backend_ops_t *ops,
                     void *backend_context);
void fs_txn_log_destroy(fs_txn_log_t *log);
int  fs_txn_recover(fs_txn_log_t *log);
int  fs_txn_begin(fs_txn_log_t *log, uint32_t credits, fs_txn_t *transaction);
int  fs_txn_stage(fs_txn_t *transaction, uint64_t home_block, const void *data, uint32_t flags);
int  fs_txn_read(fs_txn_t *transaction, uint64_t home_block, void *data);
int  fs_txn_stage_bytes(fs_txn_t *transaction, uint64_t offset, const void *data, size_t size, uint32_t flags);
int  fs_txn_read_bytes(fs_txn_t *transaction, uint64_t offset, void *data, size_t size);
int  fs_txn_commit(fs_txn_t *transaction);
void fs_txn_abort(fs_txn_t *transaction, int error);
int  fs_txn_log_error(const fs_txn_log_t *log);

#endif // INCLUDE_fs_txn_H_
