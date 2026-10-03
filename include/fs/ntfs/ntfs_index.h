/*
 *
 *      ntfs_index.h
 *      New Technology File System - $I30 index B-tree
 *
 *      2026/7/26 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NTFS_INDEX_H_
#define INCLUDE_NTFS_INDEX_H_

#include <fs/ntfs/ntfs.h>

/* A decoded index entry together with its on-disk length. */
typedef struct ntfs_index_item {
        uint8_t *entry;
        uint16_t length;
} ntfs_index_item_t;

/* The $I30 attributes of a directory record: resident root, non-resident allocation, and the bitmap tracking which index blocks are in use. */
typedef struct ntfs_index_attributes {
        attr_rec_t *root;
        attr_rec_t *allocation;
        attr_rec_t *bitmap;
        uint32_t    root_offset;
        uint32_t    allocation_offset;
        uint32_t    bitmap_offset;
} ntfs_index_attributes_t;

/* ntfs_index.c */
int  ntfs_index_reclaim_commit(ntfs_mount_t *mnt);
void ntfs_index_reclaim_abort(ntfs_mount_t *mnt);
int  ntfs_directory_index_insert(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, uint32_t file_attributes,
                                 uint64_t data_size, uint64_t allocated_size);
void ntfs_index_items_free(ntfs_index_item_t *items, uint32_t count);
int  ntfs_directory_index_collect(ntfs_mount_t *mnt, uint8_t *record, ntfs_index_item_t **result, uint32_t *result_count, ntfs_index_attributes_t *result_attributes);
int  ntfs_directory_index_set_reparse_tag(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, const uint16_t *name, uint8_t name_length);
int  ntfs_directory_index_remove(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, const uint16_t *name, uint8_t name_length);

#endif // INCLUDE_NTFS_INDEX_H_
