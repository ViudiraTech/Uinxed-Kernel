/*
 *
 *      fs_txn.c
 *      Filesystem transaction and write-ahead-log coordination
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/fs_txn.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <process/sched.h>

/* Wait for log to become free, using two-phase wait queue. Process context only. */
void fs_txn_log_lock(fs_txn_log_t *log)
{
    for (;;) {
        spin_lock(&log->guard);
        if (!log->busy) {
            log->busy = true;
            spin_unlock(&log->guard);
            return;
        }
        wait_queue_prepare(&log->wq);
        spin_unlock(&log->guard);
        wait_queue_sleep();
    }
}

/* Release log lock and wake all waiters. */
void fs_txn_log_unlock(fs_txn_log_t *log)
{
    spin_lock(&log->guard);
    log->busy = false;
    spin_unlock(&log->guard);
    wait_queue_wake_all(&log->wq);
}

/*
 * Wait until the log has no active transaction AND the mutex is free (the
 * previous transaction's commit/abort fully finished), then claim it.  This
 * serialises fs_txn_begin() so two threads can never publish overlapping
 * active-transaction pointers.  Claimers sleep on the same wait queue that
 * fs_txn_log_unlock() wakes.
 */
static void fs_txn_claim_log(fs_txn_log_t *log)
{
    for (;;) {
        spin_lock(&log->guard);
        if (!log->transaction_active && !log->busy) {
            log->transaction_active = 1;
            spin_unlock(&log->guard);
            return;
        }
        wait_queue_prepare(&log->wq);
        spin_unlock(&log->guard);
        wait_queue_sleep();
    }
}

/* Mark the log as no longer active; caller holds the log mutex. */
static void fs_txn_release_log_locked(fs_txn_log_t *log)
{
    spin_lock(&log->guard);
    log->transaction_active = 0;
    spin_unlock(&log->guard);
}

/* Mark the log as no longer active (self-locking, wakes claimers). */
static void fs_txn_release_log(fs_txn_log_t *log)
{
    spin_lock(&log->guard);
    log->transaction_active = 0;
    spin_unlock(&log->guard);
    wait_queue_wake_all(&log->wq);
}

/*
 * Free all staged buffers of a transaction.  The caller holds the log mutex: the
 * buffer list is serialised against concurrent readers/writers
 * (fs_txn_read_active/stage_active) by that lock, and fs_txn_finish() runs
 * under it (fs_txn_commit/abort are invoked with the lock held by the FS
 * wrapper).  Freeing without it would let a concurrent active-helper walk
 * freed nodes.
 */
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

/* Flush the device, mapping -EOPNOTSUPP through unchanged. */
static int fs_txn_flush(fs_txn_log_t *log)
{
    int status = blockdev_flush(&log->device);
    return status == EOK ? EOK : (status == -EOPNOTSUPP ? -EOPNOTSUPP : -EIO);
}

/* Check that home_block lies within the device. */
static int fs_txn_home_block_valid(const fs_txn_log_t *log, uint64_t home_block)
{
    if (!log || !log->device.sector_size || log->block_size < log->device.sector_size || log->block_size % log->device.sector_size) return 0;
    uint64_t sectors_per_block = log->block_size / log->device.sector_size;
    return home_block < log->device.sector_count / sectors_per_block;
}

/* Write staged buffers matching required_flags to their home blocks. */
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
            plogk("fs_txn: Write_home block %llu write failed (drive %u, status %d)\n", (unsigned long long)buffer->home_block, log->device.drive, status);
            return status;
        }
    }
    return EOK;
}

/* Release the transaction's buffers and mark the log inactive. */
static void fs_txn_finish(fs_txn_t *transaction)
{
    fs_txn_log_t *log = transaction->log;

    /* Caller holds the log mutex (see fs_txn_commit/abort). */
    fs_txn_release_buffers(transaction);
    transaction->active = 0;
    fs_txn_release_log_locked(log);
}

/* Initialize a transaction log bound to a block device. */
int fs_txn_log_init(fs_txn_log_t *log, const blockdev_device_t *device, uint32_t block_size, const fs_txn_backend_ops_t *ops, void *backend_context)
{
    if (!log || !device || !device->sector_size || !device->sector_count || block_size < device->sector_size || block_size % device->sector_size) {
        plogk("fs_txn: Log init invalid parameters (drive %u, block_size %u)\n", device ? device->drive : 0, block_size);
        return -EINVAL;
    }
    memset(log, 0, sizeof(*log));
    wait_queue_init(&log->wq);
    log->device              = *device;
    log->ops                 = ops;
    log->backend_context     = backend_context;
    log->block_size          = block_size;
    log->next_transaction_id = 1;
    blockdev_retain(device);
    return EOK;
}

/* Tear down a transaction log and release its device reference. */
void fs_txn_log_destroy(fs_txn_log_t *log)
{
    if (!log) return;
    blockdev_release(&log->device);
    memset(log, 0, sizeof(*log));
}

/* Run backend recovery for the log. */
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

/* Begin a new transaction on the log. */
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

/* Stage a whole block for later write-out by the transaction. Caller must hold the log lock (see fs_txn_stage). */
static int fs_txn_stage_locked(fs_txn_t *transaction, uint64_t home_block, const void *data, uint32_t flags)
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
        plogk("fs_txn: Stage credits exhausted (home_block %llu, used %u, credits %u)\n", (unsigned long long)home_block, transaction->used, transaction->credits);
        return -ENOSPC;
    }
    buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        plogk("fs_txn: Stage buffer allocation failed (home_block %llu)\n", (unsigned long long)home_block);
        return -ENOMEM;
    }
    buffer->data = malloc(transaction->log->block_size);
    if (!buffer->data) {
        plogk("fs_txn: Stage data allocation failed (home_block %llu, block_size %u)\n", (unsigned long long)home_block, transaction->log->block_size);
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

/* Stage a whole block for later write-out by the transaction. */
int fs_txn_stage(fs_txn_t *transaction, uint64_t home_block, const void *data, uint32_t flags)
{
    if (!transaction) return -EINVAL;
    fs_txn_log_t *log    = transaction->log;
    int           status = -EINVAL;

    fs_txn_log_lock(log);
    status = fs_txn_stage_locked(transaction, home_block, data, flags);
    fs_txn_log_unlock(log);
    return status;
}

/* Read a whole block, honoring data staged by the transaction. Caller must hold the log lock (see fs_txn_read). */
static int fs_txn_read_locked(fs_txn_t *transaction, uint64_t home_block, void *data)
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
    status = blockdev_read_bytes(&transaction->log->device, home_block * (uint64_t)transaction->log->block_size, data, transaction->log->block_size);
    if (status != EOK) plogk("fs_txn: Read home_block %llu failed (drive %u, status %d)\n", (unsigned long long)home_block, transaction->log->device.drive, status);
    return status;
}

/* Read a whole block, honoring data staged by the transaction. */
int fs_txn_read(fs_txn_t *transaction, uint64_t home_block, void *data)
{
    if (!transaction) return -EINVAL;
    fs_txn_log_t *log    = transaction->log;
    int           status = -EINVAL;

    fs_txn_log_lock(log);
    status = fs_txn_read_locked(transaction, home_block, data);
    fs_txn_log_unlock(log);
    return status;
}

/* Check a byte-range request against the device size. */
static int fs_txn_byte_range_valid(fs_txn_t *transaction, uint64_t offset, size_t size)
{
    if (!transaction || !transaction->active || !transaction->log || offset > UINT64_MAX - size) return 0;
    fs_txn_log_t *log = transaction->log;
    if (!log->device.sector_size || log->device.sector_count > UINT64_MAX / log->device.sector_size) return 0;
    uint64_t device_size = log->device.sector_count * log->device.sector_size;
    return offset <= device_size && size <= device_size - offset;
}

/* Read a byte range through the transaction. */
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
        plogk("fs_txn: Read_bytes block allocation failed (offset %llu, size %zu, block_size %u)\n", (unsigned long long)offset, size, block_size);
        return -ENOMEM;
    }
    int           status = EOK;
    fs_txn_log_t *log    = transaction->log;

    fs_txn_log_lock(log);
    while (size) {
        uint64_t logical = offset / block_size;
        uint32_t within  = (uint32_t)(offset % block_size);
        size_t   chunk   = size < block_size - within ? size : block_size - within;
        status           = fs_txn_read_locked(transaction, logical, block);
        if (status != EOK) break;
        memcpy(output, block + within, chunk);
        output += chunk;
        offset += chunk;
        size -= chunk;
    }
    fs_txn_log_unlock(log);
    free(block);
    return status;
}

/* Stage a byte range, reading the surrounding home blocks first. */
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
        plogk("fs_txn: Stage_bytes block allocation failed (offset %llu, size %zu, block_size %u)\n", (unsigned long long)offset, size, block_size);
        return -ENOMEM;
    }
    int           status = EOK;
    fs_txn_log_t *log    = transaction->log;

    fs_txn_log_lock(log);
    while (size) {
        uint64_t logical = offset / block_size;
        uint32_t within  = (uint32_t)(offset % block_size);
        size_t   chunk   = size < block_size - within ? size : block_size - within;
        status           = fs_txn_read_locked(transaction, logical, block);
        if (status == EOK) {
            memcpy(block + within, input, chunk);
            status = fs_txn_stage_locked(transaction, logical, block, flags);
        }
        if (status != EOK) {
            transaction->error = status;
            break;
        }
        input += chunk;
        offset += chunk;
        size -= chunk;
    }
    fs_txn_log_unlock(log);
    free(block);
    return status;
}

/*
 * Serve a whole-block read through the volume's currently-active
 * transaction, if any.  The active-transaction pointer is read and the
 * transaction's buffer list is walked under the log lock, so a concurrent
 * commit cannot clear the pointer and free the buffers while they are in
 * use.
 *
 * Returns 0 when no transaction is active (the caller must fall back to a
 * direct device read), 1 when the block was served, or a negative errno.
 */
int fs_txn_read_active(fs_txn_log_t *log, fs_txn_t **active_pp, uint64_t home_block, void *data)
{
    if (!log || !active_pp || !data) return -EINVAL;

    fs_txn_log_lock(log);
    fs_txn_t *transaction = *active_pp;
    if (!transaction) {
        fs_txn_log_unlock(log);
        return 0;
    }
    int status = fs_txn_read_locked(transaction, home_block, data);
    fs_txn_log_unlock(log);
    return status < 0 ? status : 1;
}

/*
 * Serve a whole-block write through the volume's currently-active
 * transaction, staging it.  Same locking contract and return convention as
 * fs_txn_read_active(): 0 = no active transaction (the caller writes the
 * device directly), 1 = staged, or a negative errno.
 */
int fs_txn_stage_active(fs_txn_log_t *log, fs_txn_t **active_pp, uint64_t home_block, const void *data, uint32_t flags)
{
    if (!log || !active_pp || !data) return -EINVAL;

    fs_txn_log_lock(log);
    fs_txn_t *transaction = *active_pp;
    if (!transaction) {
        fs_txn_log_unlock(log);
        return 0;
    }
    int status = fs_txn_stage_locked(transaction, home_block, data, flags);
    if (status != EOK) transaction->error = status;
    fs_txn_log_unlock(log);
    return status < 0 ? status : 1;
}

/*
 * Serve a byte-range read through the volume's currently-active
 * transaction, if any.  Same locking contract and return convention as
 * fs_txn_read_active().
 */
int fs_txn_read_bytes_active(fs_txn_log_t *log, fs_txn_t **active_pp, uint64_t offset, void *data, size_t size)
{
    if (!size) return EOK;
    if (!log || !active_pp || !data) return -EINVAL;
    if (!log->block_size) return -EINVAL;

    uint8_t *block = malloc(log->block_size);
    if (!block) {
        plogk("fs_txn: Read_bytes_active block allocation failed (offset %llu, size %zu)\n", (unsigned long long)offset, size);
        return -ENOMEM;
    }

    uint8_t *output = data;
    int      status = EOK;
    fs_txn_log_lock(log);
    fs_txn_t *transaction = *active_pp;
    if (!transaction) {
        fs_txn_log_unlock(log);
        free(block);
        return 0;
    }
    while (size) {
        uint64_t logical = offset / log->block_size;
        uint32_t within  = (uint32_t)(offset % log->block_size);
        size_t   chunk   = size < log->block_size - within ? size : log->block_size - within;
        status           = fs_txn_read_locked(transaction, logical, block);
        if (status != EOK) break;
        memcpy(output, block + within, chunk);
        output += chunk;
        offset += chunk;
        size -= chunk;
    }
    fs_txn_log_unlock(log);
    free(block);
    return status < 0 ? status : 1;
}

/*
 * Serve a byte-range write through the volume's currently-active
 * transaction, staging it.  Same locking contract and return convention as
 * fs_txn_read_active().
 */
int fs_txn_stage_bytes_active(fs_txn_log_t *log, fs_txn_t **active_pp, uint64_t offset, const void *data, size_t size, uint32_t flags)
{
    if (!size) return EOK;
    if (!log || !active_pp || !data) return -EINVAL;
    if (!log->block_size) return -EINVAL;

    uint8_t *block = malloc(log->block_size);
    if (!block) {
        plogk("fs_txn: Stage_bytes_active block allocation failed (offset %llu, size %zu)\n", (unsigned long long)offset, size);
        return -ENOMEM;
    }

    const uint8_t *input  = data;
    int            status = EOK;
    fs_txn_log_lock(log);
    fs_txn_t *transaction = *active_pp;
    if (!transaction) {
        fs_txn_log_unlock(log);
        free(block);
        return 0;
    }
    while (size) {
        uint64_t logical = offset / log->block_size;
        uint32_t within  = (uint32_t)(offset % log->block_size);
        size_t   chunk   = size < log->block_size - within ? size : log->block_size - within;
        status           = fs_txn_read_locked(transaction, logical, block);
        if (status == EOK) {
            memcpy(block + within, input, chunk);
            status = fs_txn_stage_locked(transaction, logical, block, flags);
        }
        if (status != EOK) {
            transaction->error = status;
            break;
        }
        input += chunk;
        offset += chunk;
        size -= chunk;
    }
    fs_txn_log_unlock(log);
    free(block);
    return status < 0 ? status : 1;
}

/*
 * Commit the transaction and write the metadata to its home blocks.
 * The caller must hold the log mutex: the buffer-list walks in this function
 * (fs_txn_write_home / log_block) and the final fs_txn_finish() free must be
 * serialised against concurrent fs_txn_read_active()/fs_txn_stage_active(),
 * and the FS wrapper keeps *active_pp published until commit completes so no
 * concurrent I/O bypasses the transaction mid-commit.
 */
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
    if (status == EOK && log->ops && log->ops->begin) status = log->ops->begin(log->backend_context, transaction->transaction_id, transaction->used);

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
        plogk("fs_txn: Commit failed (transaction %u, drive %u, status %d, buffers %u)\n", transaction->transaction_id, log->device.drive, status, transaction->used);
        log->aborted    = 1;
        log->last_error = status;
        if (log->ops && log->ops->abort) log->ops->abort(log->backend_context, transaction->transaction_id, status);
    }
    fs_txn_finish(transaction);
    return status;
}

/* Abort the transaction, recording the given error.  Caller holds the log mutex. */
void fs_txn_abort(fs_txn_t *transaction, int error)
{
    if (!transaction || !transaction->active) return;
    transaction->error = error ? error : -EIO;
    if (transaction->log->ops && transaction->log->ops->abort) transaction->log->ops->abort(transaction->log->backend_context, transaction->transaction_id, transaction->error);
    fs_txn_finish(transaction);
}

/* Return the last error recorded on the log. */
int fs_txn_log_error(const fs_txn_log_t *log)
{
    if (!log) {
        plogk("fs_txn: Log_error with NULL log.\n");
        return -EINVAL;
    }
    return log->last_error;
}
