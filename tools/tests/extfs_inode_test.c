/*
 *
 *      extfs_inode_test.c
 *      Host regression tests for extfs indirect-tree truncation
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
#include <kernel/errno.h>

#define TEST_BLOCK_SIZE 1024
#define TEST_BLOCKS     1024

static uint8_t      test_disk[TEST_BLOCKS][TEST_BLOCK_SIZE];
static uint8_t      test_allocated[TEST_BLOCKS];
static ext2_inode_t test_inode;
static unsigned     test_freed;

uint32_t timer_realtime_seconds32(void) { return 1774780800U; }

int extfs_read_block(extfs_sb_info_t *sb, uint32_t block, void *buffer)
{
    (void)sb;
    if (block >= TEST_BLOCKS || !test_allocated[block]) return -EIO;
    memcpy(buffer, test_disk[block], TEST_BLOCK_SIZE);
    return EOK;
}

int extfs_write_block(extfs_sb_info_t *sb, uint32_t block, const void *buffer)
{
    (void)sb;
    if (block >= TEST_BLOCKS || !test_allocated[block]) return -EIO;
    memcpy(test_disk[block], buffer, TEST_BLOCK_SIZE);
    return EOK;
}

int extfs_write_data_block(extfs_sb_info_t *sb, uint32_t block, const void *buffer)
{
    return extfs_write_block(sb, block, buffer);
}

int extfs_read_inode_raw(extfs_sb_info_t *sb, uint32_t ino, ext2_inode_t *raw)
{
    (void)sb;
    if (ino != 42) return -EINVAL;
    *raw = test_inode;
    return EOK;
}

int extfs_write_inode_raw(extfs_sb_info_t *sb, uint32_t ino, const ext2_inode_t *raw)
{
    (void)sb;
    if (ino != 42) return -EINVAL;
    test_inode = *raw;
    return EOK;
}

int extfs_alloc_block(extfs_sb_info_t *sb, uint32_t goal, uint32_t *out)
{
    (void)sb;
    (void)goal;
    (void)out;
    return -ENOSPC;
}

void extfs_free_block(extfs_sb_info_t *sb, uint32_t block)
{
    (void)sb;
    assert(block < TEST_BLOCKS && test_allocated[block]);
    test_allocated[block] = 0;
    test_freed++;
}

static void allocate_block(uint32_t block)
{
    assert(block < TEST_BLOCKS && !test_allocated[block]);
    test_allocated[block] = 1;
}

int main(void)
{
    ext2_super_block_t superblock = {.s_blocks_count = TEST_BLOCKS};
    extfs_sb_info_t sb = {.es = &superblock, .block_size = TEST_BLOCK_SIZE};
    extfs_handle_t handle = {.sb = &sb, .inode_no = 42};
    uint32_t *single;
    uint32_t *double_root;
    uint32_t *double_leaf;

    memset(&test_inode, 0, sizeof(test_inode));
    test_inode.i_mode = EXT2_S_IFREG | 0644;
    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        allocate_block(10 + i);
        handle.ei.i_data[i] = 10 + i;
    }

    allocate_block(100);
    handle.ei.i_data[EXT2_IND_BLOCK] = 100;
    single = (uint32_t *)test_disk[100];
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE / sizeof(uint32_t); i++) {
        allocate_block(200 + i);
        single[i] = 200 + i;
    }

    allocate_block(500);
    allocate_block(501);
    allocate_block(600);
    allocate_block(601);
    handle.ei.i_data[EXT2_DIND_BLOCK] = 500;
    double_root = (uint32_t *)test_disk[500];
    double_leaf = (uint32_t *)test_disk[501];
    double_root[0] = 501;
    double_leaf[0] = 600;
    double_leaf[1] = 601;

    memcpy(test_inode.i_block, handle.ei.i_data, sizeof(test_inode.i_block));
    test_inode.i_size = (EXT2_NDIR_BLOCKS + TEST_BLOCK_SIZE / sizeof(uint32_t) + 2) * TEST_BLOCK_SIZE;

    assert(extfs_truncate(&handle, (EXT2_NDIR_BLOCKS + 1) * TEST_BLOCK_SIZE) == EOK);
    assert(handle.ei.i_data[EXT2_IND_BLOCK] == 100);
    assert(handle.ei.i_data[EXT2_DIND_BLOCK] == 0);
    assert(single[0] == 200 && single[1] == 0);
    assert(!test_allocated[500] && !test_allocated[501] && !test_allocated[600] && !test_allocated[601]);
    assert(test_inode.i_size == (EXT2_NDIR_BLOCKS + 1) * TEST_BLOCK_SIZE);
    assert(test_inode.i_blocks == 28);
    assert(test_freed == 259);

    puts("PASS extfs indirect truncate and i_blocks accounting");
    return 0;
}
