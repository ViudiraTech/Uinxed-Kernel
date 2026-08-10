/*
 *
 *      fs_txn.c
 *      Filesystem transaction and write-ahead-log coordination
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/fs_txn.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <proc/sched.h>

static void fs_txn_claim_log(fs_txn_log_t *log)
{
    for (;;) {
        spin_lock(&log->lock);
        if (!log->transaction_active) {
            log->transaction_active = 1;
            spin_unlock(&log->lock);
            return;
        }
        spin_unlock(&log->lock);
        sched_yield();
    }
}

static void fs_txn_release_log(fs_txn_log_t *log)
{
    spin_lock(&log->lock);
    log->transaction_active = 0;
    spin_unlock(&log->lock);
}

static void fs_txn_release_buffers(fs_txn_t *transaction)
{
    fs_txn_buffer_t *buffer = transaction->buffers;
    while (buffer) {
        fs_txn_buffer_t *next = buffer->next;
        free(buffer->data);
        free(buffer);
        buffer = next;
    }
    transaction->buffers = 0;
    transaction->tail    = 0;
}

static int fs_txn_flush(fs_txn_log_t *log)
{
    int status = blockdev_flush(&log->device);
    return status == EOK ? EOK : (status == -EOPNOTSUPP ? -EOPNOTSUPP : -EIO);
}

static int fs_txn_home_block_valid(const fs_txn_log_t *log, uint64_t home_block)
{
    if (!log || !log->device.sector_size || log->block_size < log->device.sector_size || log->block_size % log->device.sector_size) return 0;
    uint64_t sectors_per_block = log->block_size / log->device.sector_size;
    return home_block < log->device.sector_count / sectors_per_block;
}

static int fs_txn_write_home(fs_txn_t *transaction, uint32_t required_flags)
{
    fs_txn_buffer_t *buffer;
    fs_txn_log_t    *log = transaction->log;

    for (buffer = transaction->buffers; buffer; buffer = buffer->next) {
        if (!(buffer->flags & required_flags)) continue;
        if (required_flags == FS_TXN_ORDERED_DATA && (buffer->flags & FS_TXN_METADATA)) continue;
        if (!fs_txn_home_block_valid(log, buffer->home_block)) {
            plogk("fs_txn: Write_home block %llu out of range (block_size %u)\n", (unsigned long long)buffer->home_block, log->block_size);
            return -EIO;
        }
        int status = blockdev_write_bytes(&log->device, buffer->home_block * (uint64_t)log->block_size, buffer->data, log->block_size);
        if (status != EOK) {
            plogk("fs_txn: Write_home block %llu write failed (drive %u, status %d)\n", (unsigned long long)buffer->home_block,
                  log->device.drive, status);
            return status;
        }
    }
    return EOK;
}

static void fs_txn_finish(fs_txn_t *transaction)
{
    fs_txn_log_t *log = transaction->log;
    fs_txn_release_buffers(transaction);
    transaction->active = 0;
    fs_txn_release_log(log);
}

int fs_txn_log_init(fs_txn_log_t *log, const blockdev_device_t *device, uint32_t block_size, const fs_txn_backend_ops_t *ops,
                    void *backend_context)
{
    if (!log || !device || !device->sector_size || !device->sector_count || block_size < device->sector_size
        || block_size % device->sector_size) {
        plogk("fs_txn: Log init invalid parameters (drive %u, block_size %u)\n", device ? device->drive : 0, block_size);
        return -EINVAL;
    }
    memset(log, 0, sizeof(*log));
    log->device              = *device;
    log->ops                 = ops;
    log->backend_context     = backend_context;
    log->block_size          = block_size;
    log->next_transaction_id = 1;
    blockdev_retain(device);
    return EOK;
}

void fs_txn_log_destroy(fs_txn_log_t *log)
{
    if (!log) return;
    blockdev_release(&log->device);
    memset(log, 0, sizeof(*log));
}

int fs_txn_recover(fs_txn_log_t *log)
{
    int status;
    if (!log) {
        plogk("fs_txn: Recover with NULL log.\n");
        return -EINVAL;
    }
    fs_txn_claim_log(log);
    status = log->ops && log->ops->recover ? log->ops->recover(log->backend_context) : EOK;
    if (status != EOK) {
        plogk("fs_txn: Recover failed (drive %u, status %d)\n", log->device.drive, status);
        log->aborted    = 1;
        log->last_error = status;
    }
    fs_txn_release_log(log);
    return status;
}

int fs_txn_begin(fs_txn_log_t *log, uint32_t credits, fs_txn_t *transaction)
{
    if (!log || !transaction || !credits) {
        plogk("fs_txn: Begin invalid arguments.\n");
        return -EINVAL;
    }
    if (log->device.read_only) {
        plogk("fs_txn: Begin on read-only device (drive %u)\n", log->device.drive);
        return -EROFS;
    }
    fs_txn_claim_log(log);
    if (log->aborted) {
        int error = log->last_error ? log->last_error : -EROFS;
        plogk("fs_txn: Begin on aborted log (drive %u, last_error %d)\n", log->device.drive, error);
        fs_txn_release_log(log);
        return error;
    }
    memset(transaction, 0, sizeof(*transaction));
    transaction->log            = log;
    transaction->credits        = credits;
    transaction->transaction_id = log->next_transaction_id++;
    if (!log->next_transaction_id) log->next_transaction_id = 1;
    transaction->active = 1;
    return EOK;
}

int fs_txn_stage(fs_txn_t *transaction, uint64_t home_block, const void *data, uint32_t flags)
{
    fs_txn_buffer_t *buffer;
    if (!transaction || !transaction->active || !data) {
        plogk("fs_txn: Stage invalid arguments.\n");
        return -EINVAL;
    }
    if (!(flags & (FS_TXN_METADATA | FS_TXN_ORDERED_DATA))) {
        plogk("fs_txn: Stage invalid flags %#x (home_block %llu)\n", (unsigned)flags, (unsigned long long)home_block);
        return -EINVAL;
    }
    if (!fs_txn_home_block_valid(transaction->log, home_block)) {
        plogk("fs_txn: Stage home_block %llu out of range (block_size %u)\n", (unsigned long long)home_block, transaction->log->block_size);
        return -EIO;
    }

    for (buffer = transaction->buffers; buffer; buffer = buffer->next) {
        if (buffer->home_block != home_block) continue;
        memcpy(buffer->data, data, transaction->log->block_size);
        buffer->flags |= flags;
        return EOK;
    }
    if (transaction->used >= transaction->credits) {
        plogk("fs_txn: Stage credits exhausted (home_block %llu, used %u, credits %u)\n", (unsigned long long)home_block, transaction->used,
              transaction->credits);
        return -ENOSPC;
    }
    buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        plogk("fs_txn: Stage buffer allocation failed (home_block %llu)\n", (unsigned long long)home_block);
        return -ENOMEM;
    }
    buffer->data = malloc(transaction->log->block_size);
    if (!buffer->data) {
        plogk("fs_txn: Stage data allocation failed (home_block %llu, block_size %u)\n", (unsigned long long)home_block,
              transaction->log->block_size);
        free(buffer);
        return -ENOMEM;
    }
    memcpy(buffer->data, data, transaction->log->block_size);
    buffer->home_block = home_block;
    buffer->flags      = flags;
    if (transaction->tail)
        transaction->tail->next = buffer;
    else
        transaction->buffers = buffer;
    transaction->tail = buffer;
    transaction->used++;
    return EOK;
}

int fs_txn_read(fs_txn_t *transaction, uint64_t home_block, void *data)
{
    fs_txn_buffer_t *buffer;
    int              status;

    if (!transaction || !transaction->active || !data) {
        plogk("fs_txn: Read invalid arguments.\n");
        return -EINVAL;
    }
    if (!fs_txn_home_block_valid(transaction->log, home_block)) {
        plogk("fs_txn: Read home_block %llu out of range (block_size %u)\n", (unsigned long long)home_block, transaction->log->block_size);
        return -EIO;
    }
    for (buffer = transaction->buffers; buffer; buffer = buffer->next) {
        if (buffer->home_block == home_block) {
            memcpy(data, buffer->data, transaction->log->block_size);
            return EOK;
        }
    }
    status = blockdev_read_bytes(&transaction->log->device, home_block * (uint64_t)transaction->log->block_size, data,
                                 transaction->log->block_size);
    if (status != EOK)
        plogk("fs_txn: Read home_block %llu failed (drive %u, status %d)\n", (unsigned long long)home_block, transaction->log->device.drive,
              status);
    return status;
}

static int fs_txn_byte_range_valid(fs_txn_t *transaction, uint64_t offset, size_t size)
{
    if (!transaction || !transaction->active || !transaction->log || offset > UINT64_MAX - size) return 0;
    fs_txn_log_t *log = transaction->log;
    if (!log->device.sector_size || log->device.sector_count > UINT64_MAX / log->device.sector_size) return 0;
    uint64_t device_size = log->device.sector_count * log->device.sector_size;
    return offset <= device_size && size <= device_size - offset;
}

int fs_txn_read_bytes(fs_txn_t *transaction, uint64_t offset, void *data, size_t size)
{
    if (!size) return EOK;
    if (!data || !fs_txn_byte_range_valid(transaction, offset, size)) {
        plogk("fs_txn: Read_bytes invalid range (offset %llu, size %zu)\n", (unsigned long long)offset, size);
        return -EINVAL;
    }
    uint8_t *output     = data;
    uint32_t block_size = transaction->log->block_size;
    uint8_t *block      = malloc(block_size);
    if (!block) {
        plogk("fs_txn: Read_bytes block allocation failed (offset %llu, size %zu, block_size %u)\n", (unsigned long long)offset, size,
              block_size);
        return -ENOMEM;
    }
    while (size) {
        uint64_t logical = offset / block_size;
        uint32_t within  = (uint32_t)(offset % block_size);
        size_t   chunk   = size < block_size - within ? size : block_size - within;
        int      status  = fs_txn_read(transaction, logical, block);
        if (status != EOK) {
            free(block);
            return status;
        }
        memcpy(output, block + within, chunk);
        output += chunk;
        offset += chunk;
        size -= chunk;
    }
    free(block);
    return EOK;
}

int fs_txn_stage_bytes(fs_txn_t *transaction, uint64_t offset, const void *data, size_t size, uint32_t flags)
{
    if (!size) return EOK;
    if (!data || !fs_txn_byte_range_valid(transaction, offset, size)) {
        plogk("fs_txn: Stage_bytes invalid range (offset %llu, size %zu)\n", (unsigned long long)offset, size);
        return -EINVAL;
    }
    const uint8_t *input      = data;
    uint32_t       block_size = transaction->log->block_size;
    uint8_t       *block      = malloc(block_size);
    if (!block) {
        plogk("fs_txn: Stage_bytes block allocation failed (offset %llu, size %zu, block_size %u)\n", (unsigned long long)offset, size,
              block_size);
        return -ENOMEM;
    }
    while (size) {
        uint64_t logical = offset / block_size;
        uint32_t within  = (uint32_t)(offset % block_size);
        size_t   chunk   = size < block_size - within ? size : block_size - within;
        int      status  = fs_txn_read(transaction, logical, block);
        if (status == EOK) {
            memcpy(block + within, input, chunk);
            status = fs_txn_stage(transaction, logical, block, flags);
        }
        if (status != EOK) {
            transaction->error = status;
            free(block);
            return status;
        }
        input += chunk;
        offset += chunk;
        size -= chunk;
    }
    free(block);
    return EOK;
}

int fs_txn_commit(fs_txn_t *transaction)
{
    fs_txn_buffer_t *buffer;
    fs_txn_log_t    *log;
    int              status = EOK;

    if (!transaction || !transaction->active) {
        plogk("fs_txn: Commit invalid arguments.\n");
        return -EINVAL;
    }
    log = transaction->log;
    if (transaction->error) status = transaction->error;

    if (status == EOK && log->ops && log->ops->begin)
        status = log->ops->begin(log->backend_context, transaction->transaction_id, transaction->used);

    /* Ordered data must reach stable storage before the metadata commit record. */
    if (status == EOK) status = fs_txn_write_home(transaction, FS_TXN_ORDERED_DATA);
    if (status == EOK && transaction->used) status = fs_txn_flush(log);

    if (status == EOK && log->ops && log->ops->log_block) {
        for (buffer = transaction->buffers; buffer; buffer = buffer->next) {
            if (!(buffer->flags & FS_TXN_METADATA)) continue;
            status = log->ops->log_block(log->backend_context, transaction->transaction_id, buffer->home_block, buffer->data, buffer->flags);
            if (status != EOK) break;
        }
    }
    if (status == EOK && log->ops && log->ops->commit) status = log->ops->commit(log->backend_context, transaction->transaction_id);
    if (status == EOK && log->ops && transaction->used) status = fs_txn_flush(log);

    if (status == EOK) status = fs_txn_write_home(transaction, FS_TXN_METADATA);
    if (status == EOK && transaction->used) status = fs_txn_flush(log);
    if (status == EOK && log->ops && log->ops->checkpoint) status = log->ops->checkpoint(log->backend_context, transaction->transaction_id);
    if (status == EOK && log->ops && transaction->used) status = fs_txn_flush(log);

    if (status != EOK) {
        plogk("fs_txn: Commit failed (transaction %u, drive %u, status %d, buffers %u)\n", transaction->transaction_id, log->device.drive,
              status, transaction->used);
        log->aborted    = 1;
        log->last_error = status;
        if (log->ops && log->ops->abort) log->ops->abort(log->backend_context, transaction->transaction_id, status);
    }
    fs_txn_finish(transaction);
    return status;
}

void fs_txn_abort(fs_txn_t *transaction, int error)
{
    if (!transaction || !transaction->active) return;
    transaction->error = error ? error : -EIO;
    if (transaction->log->ops && transaction->log->ops->abort)
        transaction->log->ops->abort(transaction->log->backend_context, transaction->transaction_id, transaction->error);
    fs_txn_finish(transaction);
}

int fs_txn_log_error(const fs_txn_log_t *log)
{
    if (!log) {
        plogk("fs_txn: Log_error with NULL log.\n");
        return -EINVAL;
    }
    return log->last_error;
}
