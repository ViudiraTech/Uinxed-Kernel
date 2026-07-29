/*
 *
 *      fs_txn_test.c
 *      Host regression tests for filesystem transaction ordering
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INCLUDE_STDINT_H_
#define INCLUDE_STDDEF_H_
#include <fs/core/fs_txn.h>
#include <kernel/errno.h>

#define TEST_BLOCK_SIZE 512
#define TEST_BLOCKS     32

enum test_event {
    EVENT_BEGIN = 1,
    EVENT_LOG,
    EVENT_COMMIT,
    EVENT_CHECKPOINT,
    EVENT_HOME_WRITE,
    EVENT_FLUSH,
};

typedef struct test_backend {
        uint8_t  disk[TEST_BLOCK_SIZE * TEST_BLOCKS];
        int      events[64];
        unsigned event_count;
        int      fail_flush;
} test_backend_t;

static void record_event(test_backend_t *backend, int event)
{
    assert(backend->event_count < sizeof(backend->events) / sizeof(backend->events[0]));
    backend->events[backend->event_count++] = event;
}

void spin_lock(spinlock_t *lock)
{
    assert(!lock->lock);
    lock->lock = 1;
}

void spin_unlock(spinlock_t *lock)
{
    assert(lock->lock);
    lock->lock = 0;
}

void blockdev_retain(const blockdev_device_t *device)
{
    (void)device;
}

void blockdev_release(const blockdev_device_t *device)
{
    (void)device;
}

int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size)
{
    test_backend_t *backend = device->backend_data;
    if (offset + size > sizeof(backend->disk)) return -EINVAL;
    memcpy(buffer, backend->disk + offset, size);
    return EOK;
}

int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size)
{
    test_backend_t *backend = device->backend_data;
    if (offset + size > sizeof(backend->disk)) return -EINVAL;
    memcpy(backend->disk + offset, buffer, size);
    record_event(backend, EVENT_HOME_WRITE);
    return EOK;
}

int blockdev_flush(const blockdev_device_t *device)
{
    test_backend_t *backend = device->backend_data;
    record_event(backend, EVENT_FLUSH);
    return backend->fail_flush ? -EIO : EOK;
}

static int test_begin(void *context, uint32_t transaction_id, uint32_t buffers)
{
    test_backend_t *backend = context;
    assert(transaction_id && buffers > 0 && buffers <= 2);
    record_event(backend, EVENT_BEGIN);
    return EOK;
}

static int test_log(void *context, uint32_t transaction_id, uint64_t block, const void *data, uint32_t flags)
{
    test_backend_t *backend = context;
    (void)transaction_id;
    assert(block == 4 && ((const uint8_t *)data)[0] == 0x4d && (flags & FS_TXN_METADATA));
    record_event(backend, EVENT_LOG);
    return EOK;
}

static int test_commit(void *context, uint32_t transaction_id)
{
    (void)transaction_id;
    record_event(context, EVENT_COMMIT);
    return EOK;
}

static int test_checkpoint(void *context, uint32_t transaction_id)
{
    (void)transaction_id;
    record_event(context, EVENT_CHECKPOINT);
    return EOK;
}

static const fs_txn_backend_ops_t test_ops = {
    .begin      = test_begin,
    .log_block  = test_log,
    .commit     = test_commit,
    .checkpoint = test_checkpoint,
};

static void test_ordering_and_read_your_write(void)
{
    test_backend_t backend = {0};
    blockdev_device_t device = {.backend_data = &backend,
                                .sector_size = TEST_BLOCK_SIZE,
                                .sector_count = TEST_BLOCKS};
    fs_txn_log_t log;
    fs_txn_t transaction;
    uint8_t metadata[TEST_BLOCK_SIZE] = {0x4d};
    uint8_t ordered[TEST_BLOCK_SIZE]  = {0x44};
    uint8_t readback[TEST_BLOCK_SIZE];

    assert(fs_txn_log_init(&log, &device, TEST_BLOCK_SIZE, &test_ops, &backend) == EOK);
    assert(fs_txn_begin(&log, 2, &transaction) == EOK);
    assert(fs_txn_stage(&transaction, 4, metadata, FS_TXN_METADATA) == EOK);
    assert(fs_txn_stage(&transaction, 5, ordered, FS_TXN_ORDERED_DATA) == EOK);
    assert(fs_txn_read(&transaction, 4, readback) == EOK && !memcmp(readback, metadata, sizeof(readback)));
    assert(fs_txn_commit(&transaction) == EOK);
    assert(backend.disk[4 * TEST_BLOCK_SIZE] == 0x4d);
    assert(backend.disk[5 * TEST_BLOCK_SIZE] == 0x44);

    const int expected[] = {EVENT_BEGIN,      EVENT_HOME_WRITE, EVENT_FLUSH, EVENT_LOG, EVENT_COMMIT,
                            EVENT_FLUSH,      EVENT_HOME_WRITE, EVENT_FLUSH, EVENT_CHECKPOINT, EVENT_FLUSH};
    assert(backend.event_count == sizeof(expected) / sizeof(expected[0]));
    assert(!memcmp(backend.events, expected, sizeof(expected)));
    fs_txn_log_destroy(&log);
}

static void test_credit_and_io_failure_abort(void)
{
    test_backend_t backend = {.fail_flush = 1};
    blockdev_device_t device = {.backend_data = &backend,
                                .sector_size = TEST_BLOCK_SIZE,
                                .sector_count = TEST_BLOCKS};
    fs_txn_log_t log;
    fs_txn_t transaction;
    uint8_t block[TEST_BLOCK_SIZE] = {1};

    assert(fs_txn_log_init(&log, &device, TEST_BLOCK_SIZE, &test_ops, &backend) == EOK);
    assert(fs_txn_begin(&log, 1, &transaction) == EOK);
    assert(fs_txn_stage(&transaction, 4, block, FS_TXN_METADATA) == EOK);
    assert(fs_txn_stage(&transaction, 5, block, FS_TXN_METADATA) == -ENOSPC);
    assert(fs_txn_commit(&transaction) == -EIO);
    assert(log.aborted && fs_txn_log_error(&log) == -EIO);
    assert(fs_txn_begin(&log, 1, &transaction) == -EIO);
    fs_txn_log_destroy(&log);
}

static void test_unaligned_byte_staging(void)
{
    test_backend_t backend = {0};
    memset(backend.disk, 0x11, sizeof(backend.disk));
    blockdev_device_t device = {.backend_data = &backend,
                                .sector_size = TEST_BLOCK_SIZE,
                                .sector_count = TEST_BLOCKS};
    fs_txn_log_t log;
    fs_txn_t transaction;
    uint8_t update[40];
    uint8_t readback[40];
    memset(update, 0xa5, sizeof(update));

    assert(fs_txn_log_init(&log, &device, TEST_BLOCK_SIZE, NULL, NULL) == EOK);
    assert(fs_txn_begin(&log, 2, &transaction) == EOK);
    assert(fs_txn_stage_bytes(&transaction, TEST_BLOCK_SIZE - 12, update, sizeof(update), FS_TXN_METADATA) == EOK);
    assert(fs_txn_read_bytes(&transaction, TEST_BLOCK_SIZE - 12, readback, sizeof(readback)) == EOK);
    assert(!memcmp(update, readback, sizeof(update)));
    assert(backend.disk[TEST_BLOCK_SIZE - 12] == 0x11);
    fs_txn_abort(&transaction, -EIO);
    assert(backend.disk[TEST_BLOCK_SIZE - 12] == 0x11);

    assert(fs_txn_begin(&log, 2, &transaction) == EOK);
    assert(fs_txn_stage_bytes(&transaction, TEST_BLOCK_SIZE - 12, update, sizeof(update), FS_TXN_METADATA) == EOK);
    assert(fs_txn_commit(&transaction) == EOK);
    assert(backend.disk[TEST_BLOCK_SIZE - 13] == 0x11);
    assert(!memcmp(backend.disk + TEST_BLOCK_SIZE - 12, update, sizeof(update)));
    assert(backend.disk[TEST_BLOCK_SIZE - 12 + sizeof(update)] == 0x11);
    fs_txn_log_destroy(&log);
}

static void test_geometry_and_range_validation(void)
{
    test_backend_t backend = {0};
    blockdev_device_t invalid = {.backend_data = &backend, .sector_size = 0, .sector_count = TEST_BLOCKS};
    blockdev_device_t device = {.backend_data = &backend,
                                .sector_size = TEST_BLOCK_SIZE,
                                .sector_count = TEST_BLOCKS};
    fs_txn_log_t log;
    fs_txn_t transaction;
    uint8_t block[TEST_BLOCK_SIZE] = {0};

    assert(fs_txn_log_init(&log, &invalid, TEST_BLOCK_SIZE, NULL, NULL) == -EINVAL);
    assert(fs_txn_log_init(&log, &device, TEST_BLOCK_SIZE, NULL, NULL) == EOK);
    assert(fs_txn_begin(&log, 1, &transaction) == EOK);
    assert(fs_txn_stage(&transaction, TEST_BLOCKS, block, FS_TXN_METADATA) == -EIO);
    assert(fs_txn_read(&transaction, TEST_BLOCKS, block) == -EIO);
    assert(fs_txn_read_bytes(&transaction, sizeof(backend.disk), block, 1) == -EINVAL);
    fs_txn_abort(&transaction, -EIO);
    fs_txn_log_destroy(&log);
}

int main(void)
{
    test_ordering_and_read_your_write();
    test_credit_and_io_failure_abort();
    test_unaligned_byte_staging();
    test_geometry_and_range_validation();
    puts("PASS filesystem transaction ordering and failure semantics");
    return 0;
}
