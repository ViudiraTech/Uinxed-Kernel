/*
 *
 *      extfs_extents_test.c
 *      Host regression tests for ext4 extent tree rebuild and truncation
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

#define TEST_BLOCK_SIZE 1024U
#define TEST_BLOCKS     8192U

static uint8_t test_disk[TEST_BLOCKS][TEST_BLOCK_SIZE];
static uint8_t allocated[TEST_BLOCKS];
static uint32_t allocation_cursor = 100;

int extfs_read_inode_raw(extfs_sb_info_t *sb, uint32_t ino, ext2_inode_t *raw)
{
    (void)sb;
    (void)ino;
    memset(raw, 0, sizeof(*raw));
    return EOK;
}

int extfs_read_block(extfs_sb_info_t *sb, uint32_t block, void *buffer)
{
    (void)sb;
    if (block >= TEST_BLOCKS || !allocated[block]) return -EIO;
    memcpy(buffer, test_disk[block], TEST_BLOCK_SIZE);
    return EOK;
}

int extfs_write_block(extfs_sb_info_t *sb, uint32_t block, const void *buffer)
{
    (void)sb;
    if (block >= TEST_BLOCKS || !allocated[block]) return -EIO;
    memcpy(test_disk[block], buffer, TEST_BLOCK_SIZE);
    return EOK;
}

int extfs_write_data_block(extfs_sb_info_t *sb, uint32_t block, const void *buffer)
{
    return extfs_write_block(sb, block, buffer);
}

int extfs_alloc_block(extfs_sb_info_t *sb, uint32_t goal, uint32_t *out)
{
    (void)sb;
    (void)goal;
    while (allocation_cursor < TEST_BLOCKS && allocated[allocation_cursor]) allocation_cursor++;
    if (allocation_cursor == TEST_BLOCKS) return -ENOSPC;
    allocated[allocation_cursor] = 1;
    *out = allocation_cursor++;
    return EOK;
}

void extfs_free_block(extfs_sb_info_t *sb, uint32_t block)
{
    (void)sb;
    assert(block < TEST_BLOCKS && allocated[block]);
    allocated[block] = 0;
}

int main(void)
{
    ext2_super_block_t superblock = {.s_blocks_count = TEST_BLOCKS};
    extfs_sb_info_t sb = {.es = &superblock, .block_size = TEST_BLOCK_SIZE, .blocks_count = TEST_BLOCKS};
    extfs_handle_t handle = {.sb = &sb};
    handle.ei.i_flags = EXT4_EXTENTS_FL;
    uint8_t header[12] = {0x0a, 0xf3, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
    memcpy(handle.ei.i_data, header, sizeof(header));

    uint32_t physical[7];
    uint32_t logical[] = {0, 1, 10, 20, 30, 40, 50};
    for (unsigned i = 0; i < 7; i++) {
        physical[i] = extfs_extent_map_block(&handle, logical[i], 1);
        assert(physical[i]);
        assert(extfs_extent_map_block(&handle, logical[i], 0) == physical[i]);
    }
    assert(((uint16_t *)handle.ei.i_data)[3] == 1); /* Root grew into an index. */
    assert(extfs_extent_remove_space(&handle, 10, 41) == EOK);
    assert(extfs_extent_map_block(&handle, 0, 0) == physical[0]);
    assert(extfs_extent_map_block(&handle, 1, 0) == physical[1]);
    assert(extfs_extent_map_block(&handle, 10, 0) == 0);
    assert(extfs_extent_map_block(&handle, 40, 0) == 0);
    assert(extfs_extent_map_block(&handle, 50, 0) == physical[6]);
    uint64_t blocks;
    assert(extfs_extent_count_blocks(&handle, &blocks) == EOK && blocks == 3);
    assert(extfs_extent_free_all(&handle) == EOK);
    assert(extfs_extent_count_blocks(&handle, &blocks) == EOK && blocks == 0);
    puts("PASS ext4 extent depth growth, lookup, shrink and free");
    return 0;
}
