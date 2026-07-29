/*
 *
 *      jbd2_recovery_test.c
 *      Host crash-recovery regression test for the native JBD2 backend
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
#include <fs/extfs/extfs.h>
#include <fs/extfs/jbd2.h>
#include <kernel/errno.h>
#include <libs/data/crc32c.h>

#define TEST_BLOCK_SIZE 1024U
#define TEST_BLOCKS     2048U
#define JOURNAL_BASE    512U
#define JOURNAL_BLOCKS  256U

static uint8_t test_disk[TEST_BLOCKS][TEST_BLOCK_SIZE];
static ext2_inode_t journal_inode;

void spin_lock(spinlock_t *lock) { assert(!lock->lock); lock->lock = 1; }
void spin_unlock(spinlock_t *lock) { assert(lock->lock); lock->lock = 0; }
void sched_yield(void) {}

static void put_be32(void *address, uint32_t value)
{
    uint8_t *p = address;
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

void blockdev_retain(const blockdev_device_t *device) { (void)device; }
void blockdev_release(const blockdev_device_t *device) { (void)device; }
int blockdev_flush(const blockdev_device_t *device) { (void)device; return EOK; }

int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size)
{
    (void)device;
    if (offset + size > sizeof(test_disk)) return -EIO;
    memcpy(buffer, (uint8_t *)test_disk + offset, size);
    return EOK;
}

int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size)
{
    (void)device;
    if (offset + size > sizeof(test_disk)) return -EIO;
    memcpy((uint8_t *)test_disk + offset, buffer, size);
    return EOK;
}

int extfs_read_inode_raw(extfs_sb_info_t *sb, uint32_t ino, ext2_inode_t *raw)
{
    if (ino != sb->es->s_journal_inum) return -EINVAL;
    *raw = journal_inode;
    return EOK;
}

extfs_handle_t *extfs_alloc_handle(extfs_sb_info_t *sb, uint32_t ino)
{
    extfs_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle) {
        handle->sb = sb;
        handle->inode_no = ino;
    }
    return handle;
}

uint32_t extfs_map_block(extfs_handle_t *handle, uint32_t logical, int create)
{
    (void)handle;
    (void)create;
    return logical < JOURNAL_BLOCKS ? JOURNAL_BASE + logical : 0;
}

static void format_journal(const ext2_super_block_t *ext_super)
{
    jbd2_superblock_t *super = (jbd2_superblock_t *)test_disk[JOURNAL_BASE];
    memset(super, 0, TEST_BLOCK_SIZE);
    put_be32(&super->header.magic, JBD2_MAGIC_NUMBER);
    put_be32(&super->header.block_type, JBD2_SUPERBLOCK_V2);
    put_be32(&super->block_size, TEST_BLOCK_SIZE);
    put_be32(&super->max_length, JOURNAL_BLOCKS);
    put_be32(&super->first, 1);
    put_be32(&super->sequence, 10);
    put_be32(&super->head, JOURNAL_BLOCKS - 3);
    put_be32(&super->feature_incompat, JBD2_FEATURE_INCOMPAT_CSUM_V3);
    memcpy(super->uuid, ext_super->s_uuid, 16);
    super->checksum_type = 4;
    put_be32(&super->checksum, 0);
    put_be32(&super->checksum, crc32c_update(~0U, super, sizeof(*super)));
}

int main(void)
{
    ext2_super_block_t ext_super;
    extfs_sb_info_t sb;
    extfs_journal_t *journal;
    uint8_t metadata[TEST_BLOCK_SIZE];
    const fs_txn_backend_ops_t *ops = extfs_jbd2_backend_ops();
    memset(&ext_super, 0, sizeof(ext_super));
    memset(&sb, 0, sizeof(sb));
    for (unsigned i = 0; i < sizeof(ext_super.s_uuid); i++) ext_super.s_uuid[i] = (uint8_t)(i * 7 + 1);
    ext_super.s_blocks_count = TEST_BLOCKS;
    ext_super.s_journal_inum = 8;
    sb.es = &ext_super;
    sb.block_size = TEST_BLOCK_SIZE;
    sb.blocks_count = TEST_BLOCKS;
    journal_inode.i_mode = EXT2_S_IFREG | 0600;
    journal_inode.i_size = JOURNAL_BLOCKS * TEST_BLOCK_SIZE;
    format_journal(&ext_super);

    int status = extfs_jbd2_open(&sb, &journal);
    if (status != EOK) fprintf(stderr, "initial open failed: %d, jbd super size=%zu\n", status, sizeof(jbd2_superblock_t));
    assert(status == EOK);
    /* More tags than fit in one descriptor, starting at the ring tail, exercises descriptor splitting and wrap. */
    assert(ops->begin(journal, 1, 80) == EOK);
    for (uint32_t index = 0; index < 80; index++) {
        memset(metadata, (int)(index + 1), sizeof(metadata));
        if (!index) put_be32(metadata, JBD2_MAGIC_NUMBER); /* Exercises escape/unescape. */
        assert(ops->log_block(journal, 1, 42 + index, metadata, FS_TXN_METADATA) == EOK);
    }
    assert(ops->commit(journal, 1) == EOK);
    memset(metadata, 1, sizeof(metadata));
    put_be32(metadata, JBD2_MAGIC_NUMBER);
    assert(memcmp(test_disk[42], metadata, sizeof(metadata)) != 0);
    extfs_jbd2_close(journal);

    /* A new mount must replay the committed metadata and clear s_start. */
    assert(extfs_jbd2_open(&sb, &journal) == EOK);
    assert(ops->recover(journal) == EOK);
    for (uint32_t index = 0; index < 80; index++) {
        memset(metadata, (int)(index + 1), sizeof(metadata));
        if (!index) put_be32(metadata, JBD2_MAGIC_NUMBER);
        assert(memcmp(test_disk[42 + index], metadata, sizeof(metadata)) == 0);
    }
    assert(((uint8_t *)test_disk[JOURNAL_BASE])[28] == 0
           && ((uint8_t *)test_disk[JOURNAL_BASE])[29] == 0
           && ((uint8_t *)test_disk[JOURNAL_BASE])[30] == 0
           && ((uint8_t *)test_disk[JOURNAL_BASE])[31] == 0);
    extfs_jbd2_close(journal);
    puts("PASS JBD2 multi-descriptor ring-wrap crash recovery");
    return 0;
}
