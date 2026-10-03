/*
 *
 *      ntfs_index.c
 *      New Technology File System - $I30 index B-tree
 *
 *      2026/7/26 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/ntfs/ntfs.h>
#include <fs/ntfs/ntfs_index.h>
#include <kernel/errno.h>
#include <libs/std/string.h>
#include <libs/util/byteorder.h>
#include <mem/heap.h>

#if CONFIG_NTFS_FS

/* Insert an entry into a resident $I30 index root. */
static int ntfs_index_root_insert(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, uint32_t file_attributes,
                                  uint64_t data_size, uint64_t allocated_size)
{
    attr_rec_t *attribute = NULL;
    uint32_t    attribute_offset;
    uint32_t    attribute_length = 0;
    uint32_t    bytes_in_use;
    uint16_t    value_offset;
    uint32_t    value_length;
    uint8_t    *value;
    uint8_t    *header;
    uint32_t    first_offset;
    uint32_t    total_size;
    uint32_t    allocation_size;
    uint32_t    insert_offset = 0;
    uint32_t    entry_size;
    uint16_t    key_size;

    if (!mnt || !record || !name || !name_length || mnt->mft_size < sizeof(mft_rec_t)) return -EINVAL;
    if (load_le32(record) != MFT_MAGIC) return -EIO;
    bytes_in_use = load_le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size || bytes_in_use < sizeof(mft_rec_t)) return -EIO;

    attribute_offset = load_le16(record + 0x14);
    while (attribute_offset + 24 <= bytes_in_use) {
        attr_rec_t *current = (attr_rec_t *)(record + attribute_offset);
        uint32_t    type    = load_le32((uint8_t *)&current->type);
        uint32_t    length  = load_le32((uint8_t *)&current->length);

        if (type == AT_END) break;
        if (length < 24 || attribute_offset > bytes_in_use - length) return -EIO;
        if (type == AT_ATTRIBUTE_LIST) return -EOPNOTSUPP;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(current, length)) {
            if (attribute) return -EIO;
            attribute        = current;
            attribute_length = length;
        }
        attribute_offset += length;
    }
    if (!attribute || attribute->non_resident) return -EIO;

    value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
    value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
    if (value_offset > attribute_length || value_length < 32 || value_length > attribute_length - value_offset) return -EIO;
    value  = (uint8_t *)attribute + value_offset;
    header = value + 0x10;
    if (load_le32(value) != AT_FILE_NAME || load_le32(value + 4) != 1) return -EOPNOTSUPP;
    first_offset    = load_le32(header);
    total_size      = load_le32(header + 4);
    allocation_size = load_le32(header + 8);
    if (header[12] & INDEX_ENTRY_NODE) return -EOPNOTSUPP;
    if (first_offset < 16 || total_size < first_offset || allocation_size < total_size || total_size > value_length - 0x10 || allocation_size > value_length - 0x10) return -EIO;

    for (uint32_t position = first_offset; position + 16 <= total_size;) {
        uint8_t *entry       = header + position;
        uint16_t length      = load_le16(entry + 8);
        uint16_t entry_key   = load_le16(entry + 10);
        uint16_t entry_flags = load_le16(entry + 12);

        if (length < 16 || length > total_size - position) return -EIO;
        if (entry_flags & INDEX_ENTRY_END) {
            if (entry_key) return -EIO;
            insert_offset = (uint32_t)(entry - record);
            break;
        }
        if (entry_key < sizeof(fname_attr_t) || entry_key > length - 16) return -EIO;
        fname_attr_t *file_name  = (fname_attr_t *)(entry + 16);
        uint32_t      name_bytes = (uint32_t)file_name->name_len * sizeof(uint16_t);
        if (name_bytes > entry_key - sizeof(fname_attr_t)) return -EIO;
        uint16_t existing[255];
        for (uint32_t i = 0; i < file_name->name_len; i++) existing[i] = load_le16((uint8_t *)file_name->name + (i * sizeof(uint16_t)));
        int comparison = ntfs_utf16_compare(mnt, name, name_length, existing, file_name->name_len);
        if (!comparison) return -EEXIST;
        if (comparison < 0) {
            insert_offset = (uint32_t)(entry - record);
            break;
        }
        position += length;
    }
    if (!insert_offset) return -EIO;

    key_size   = (uint16_t)(sizeof(fname_attr_t) + ((uint32_t)name_length * sizeof(uint16_t)));
    entry_size = ntfs_align8(16 + key_size);
    if (entry_size > mnt->mft_size - bytes_in_use || attribute_length > UINT32_MAX - entry_size || value_length > UINT32_MAX - entry_size || total_size > UINT32_MAX - entry_size
        || allocation_size > UINT32_MAX - entry_size) {
        return -ENOSPC;
    }

    memmove(record + insert_offset + entry_size, record + insert_offset, bytes_in_use - insert_offset);
    memset(record + insert_offset, 0, entry_size);
    store_le64(record + insert_offset, file_reference);
    store_le16(record + insert_offset + 8, (uint16_t)entry_size);
    store_le16(record + insert_offset + 10, key_size);
    fname_attr_t *file_name = (fname_attr_t *)(record + insert_offset + 16);
    store_le64((uint8_t *)&file_name->parent_dir, parent_reference);
    store_le64((uint8_t *)&file_name->alloc_size, allocated_size);
    store_le64((uint8_t *)&file_name->data_size, data_size);
    store_le32((uint8_t *)&file_name->fa, file_attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (uint32_t i = 0; i < name_length; i++) store_le16((uint8_t *)file_name->name + (i * sizeof(uint16_t)), name[i]);

    store_le32((uint8_t *)&attribute->length, attribute_length + entry_size);
    store_le32((uint8_t *)&attribute->d.res.value_length, value_length + entry_size);
    store_le32(header + 4, total_size + entry_size);
    store_le32(header + 8, allocation_size + entry_size);
    store_le32(record + 0x18, bytes_in_use + entry_size);
    return 0;
}

/* Remove an entry from a resident $I30 index root. */
static int ntfs_index_root_remove(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, const uint16_t *name, uint8_t name_length)
{
    attr_rec_t *attribute = NULL;
    uint32_t    offset;
    uint32_t    attribute_length = 0;
    uint32_t    bytes_in_use;
    uint16_t    value_offset;
    uint32_t    value_length;
    uint8_t    *header;
    uint32_t    first_offset;
    uint32_t    total_size;
    uint32_t    allocation_size;
    uint32_t    remove_offset = 0;
    uint16_t    remove_length = 0;

    if (!mnt || !record || !name || !name_length || load_le32(record) != MFT_MAGIC) return -EINVAL;
    bytes_in_use = load_le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size) return -EIO;
    offset = load_le16(record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *current = (attr_rec_t *)(record + offset);
        uint32_t    type    = load_le32((uint8_t *)&current->type);
        uint32_t    length  = load_le32((uint8_t *)&current->length);

        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(current, length)) {
            if (attribute) return -EIO;
            attribute        = current;
            attribute_length = length;
        }
        offset += length;
    }
    if (!attribute || attribute->non_resident) return -EIO;
    value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
    value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
    if (value_offset > attribute_length || value_length < 32 || value_length > attribute_length - value_offset) return -EIO;
    header          = (uint8_t *)attribute + value_offset + 0x10;
    first_offset    = load_le32(header);
    total_size      = load_le32(header + 4);
    allocation_size = load_le32(header + 8);
    if (header[12] & INDEX_ENTRY_NODE) return -EOPNOTSUPP;
    if (first_offset < 16 || total_size < first_offset || allocation_size < total_size || total_size > value_length - 0x10) return -EIO;

    for (uint32_t position = first_offset; position + 16 <= total_size;) {
        uint8_t *entry       = header + position;
        uint16_t length      = load_le16(entry + 8);
        uint16_t key_length  = load_le16(entry + 10);
        uint16_t entry_flags = load_le16(entry + 12);

        if (length < 16 || length > total_size - position) return -EIO;
        if (entry_flags & INDEX_ENTRY_END) break;
        if (key_length < sizeof(fname_attr_t) || key_length > length - 16) return -EIO;
        fname_attr_t *file_name  = (fname_attr_t *)(entry + 16);
        uint32_t      name_bytes = (uint32_t)file_name->name_len * sizeof(uint16_t);
        if (name_bytes > key_length - sizeof(fname_attr_t)) return -EIO;
        if ((load_le64(entry) & 0x0000ffffffffffffULL) == (file_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length) {
            int equal = 1;
            for (uint32_t i = 0; i < name_length; i++) {
                if (load_le16((uint8_t *)file_name->name + (i * sizeof(uint16_t))) != name[i]) {
                    equal = 0;
                    break;
                }
            }
            if (equal) {
                remove_offset = (uint32_t)(entry - record);
                remove_length = length;
            }
        }
        position += length;
    }
    if (!remove_offset) return -ENOENT;
    memmove(record + remove_offset, record + remove_offset + remove_length, bytes_in_use - remove_offset - remove_length);
    memset(record + bytes_in_use - remove_length, 0, remove_length);
    store_le32((uint8_t *)&attribute->length, attribute_length - remove_length);
    store_le32((uint8_t *)&attribute->d.res.value_length, value_length - remove_length);
    store_le32(header + 4, total_size - remove_length);
    store_le32(header + 8, allocation_size - remove_length);
    store_le32(record + 0x18, bytes_in_use - remove_length);
    return 0;
}

/* Set the reparse tag of an index entry. */
static int ntfs_index_root_set_reparse_tag(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, const uint16_t *name, uint8_t name_length)
{
    uint32_t bytes_in_use = load_le32(record + 0x18);
    uint32_t offset       = load_le16(record + 0x14);
    if (!mnt || !record || bytes_in_use > mnt->mft_size) return -EINVAL;
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length) && !attribute->non_resident) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < 32 || value_length > length - value_offset) return -EIO;
            uint8_t *header = (uint8_t *)attribute + value_offset + 0x10;
            uint32_t pos    = load_le32(header);
            uint32_t total  = load_le32(header + 4);
            while (pos + 16 <= total) {
                uint8_t *entry  = header + pos;
                uint16_t elen   = load_le16(entry + 8);
                uint16_t keylen = load_le16(entry + 10);
                uint16_t flags  = load_le16(entry + 12);
                if (elen < 16 || elen > total - pos) return -EIO;
                if (flags & INDEX_ENTRY_END) break;
                if (keylen < sizeof(fname_attr_t) || keylen > elen - 16) return -EIO;
                fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
                if ((load_le64(entry) & 0x0000ffffffffffffULL) == (file_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length) {
                    int equal = 1;
                    for (uint32_t i = 0; i < name_length; i++)
                        if (load_le16((uint8_t *)file_name->name + ((size_t)i * 2)) != name[i]) equal = 0;
                    if (equal) {
                        store_le32((uint8_t *)&file_name->rp_tag, IO_REPARSE_TAG_SYMLINK);
                        return 0;
                    }
                }
                pos += elen;
            }
        }
        offset += length;
    }
    return -ENOENT;
}

/* Return the VCN size of an index record. */
static uint32_t ntfs_index_vcn_size(const ntfs_mount_t *mnt)
{
    return mnt->indx_size < mnt->cluster_size ? mnt->indx_size : mnt->cluster_size;
}

/* Stage index clusters for reclamation once the namespace change commits. */
static int ntfs_index_reclaim_stage(ntfs_mount_t *mnt, const int64_t *lcn, const int64_t *length, int count)
{
    if (!mnt || !lcn || !length || count <= 0 || count > 256) return -EINVAL;
    if (mnt->index_reclaim_count) return -EBUSY;
    for (int extent = 0; extent < count; extent++) {
        if (lcn[extent] < 0 || length[extent] <= 0 || lcn[extent] >= mnt->nr_clusters || length[extent] > mnt->nr_clusters - lcn[extent]) return -EIO;
        mnt->index_reclaim_lcn[extent]    = lcn[extent];
        mnt->index_reclaim_length[extent] = length[extent];
    }
    mnt->index_reclaim_count = count;
    return 0;
}

/* Commit staged index-cluster reclamation. */
int ntfs_index_reclaim_commit(ntfs_mount_t *mnt)
{
    int status;
    if (!mnt) return -EINVAL;
    if (!mnt->index_reclaim_count) return 0;
    status = ntfs_bitmap_change_extents(mnt, mnt->index_reclaim_lcn, mnt->index_reclaim_length, mnt->index_reclaim_count, 0);
    if (status == 0) mnt->index_reclaim_count = 0;
    return status;
}

/* Abort staged index-cluster reclamation. */
void ntfs_index_reclaim_abort(ntfs_mount_t *mnt)
{
    if (mnt) mnt->index_reclaim_count = 0;
}

/* Move index entries out of a full index root into a nonresident allocation. */
static int ntfs_index_root_promote(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, uint32_t file_attributes,
                                   uint64_t data_size, uint64_t allocated_size)
{
    ntfs_mount_t temporary_mount;
    uint8_t     *temporary   = NULL;
    uint8_t     *indx        = NULL;
    attr_rec_t  *root        = NULL;
    uint32_t     root_offset = 0, root_length = 0, used = load_le32(record + 0x18);
    int64_t      lcn[256], run[256];
    int          run_count = 0;
    uint8_t      mapping[2048];
    int          mapping_size;
    int          status = -EIO;

    if (!mnt || !record || !name || !name_length || !mnt->indx_size || used > mnt->mft_size) return -EINVAL;
    for (uint32_t offset = load_le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length)) {
            root        = attribute;
            root_offset = offset;
            root_length = length;
            break;
        }
        offset += length;
    }
    if (!root || root->non_resident) return -EIO;
    uint16_t value_offset = load_le16((uint8_t *)&root->d.res.value_offset);
    uint32_t value_length = load_le32((uint8_t *)&root->d.res.value_length);
    if (value_offset > root_length || value_length < 48 || value_length > root_length - value_offset) return -EIO;
    uint8_t *header = (uint8_t *)root + value_offset + 0x10;
    uint32_t first = load_le32(header), total = load_le32(header + 4);
    if (first < 16 || total < first + 16 || total > value_length - 0x10 || (header[12] & INDEX_ENTRY_NODE)) return -EIO;

    uint32_t capacity = mnt->mft_size + 1024;
    temporary         = calloc(1, capacity);
    if (!temporary) return -ENOMEM;
    memcpy(temporary, record, mnt->mft_size);
    memcpy(&temporary_mount, mnt, sizeof(temporary_mount));
    temporary_mount.mft_size = capacity;
    status                   = ntfs_index_root_insert(&temporary_mount, temporary, file_reference, parent_reference, name, name_length, file_attributes, data_size, allocated_size);
    if (status < 0) goto out;
    attr_rec_t *temp_root   = (attr_rec_t *)(temporary + root_offset);
    uint8_t    *temp_header = (uint8_t *)temp_root + load_le16((uint8_t *)&temp_root->d.res.value_offset) + 0x10;
    uint32_t    temp_first = load_le32(temp_header), entries_size = load_le32(temp_header + 4) - temp_first;
    if (0x40 + entries_size > mnt->indx_size - 2) {
        status = -ENOSPC;
        goto out;
    }

    uint64_t clusters = (mnt->indx_size + mnt->cluster_size - 1) / mnt->cluster_size;
    status            = ntfs_bitmap_find_free(mnt, clusters, lcn, run, &run_count);
    if (status < 0) goto out;
    mapping_size = ntfs_runlist_encode(lcn, run, run_count, mapping, sizeof(mapping));
    if (mapping_size < 0) {
        status = mapping_size;
        goto out;
    }
    uint32_t new_root_length = root_length - (total - first) + 24;
    uint32_t ia_length = ntfs_align8(72 + (uint32_t)mapping_size), bitmap_length = 40;
    if (used - root_length + new_root_length + ia_length + bitmap_length > mnt->mft_size) {
        status = -ENOSPC;
        goto out;
    }

    indx = calloc(1, mnt->indx_size);
    if (!indx) {
        status = -ENOMEM;
        goto out;
    }
    store_le32(indx, INDX_MAGIC);
    store_le16(indx + 4, 0x28);
    store_le16(indx + 6, (uint16_t)((mnt->indx_size / mnt->sector_size) + 1));
    store_le32(indx + 0x18, 0x28);
    store_le32(indx + 0x1c, 0x28 + entries_size);
    store_le32(indx + 0x20, mnt->indx_size - 0x18);
    memcpy(indx + 0x40, temp_header + temp_first, entries_size);
    status = ntfs_record_pack(mnt, indx, mnt->indx_size);
    if (status < 0) goto out;
    if (ntfs_write_by_runlist(mnt, mapping, mapping_size, 0, indx, mnt->indx_size, mnt->indx_size) != (int64_t)mnt->indx_size) {
        status = -EIO;
        goto out;
    }
    status = ntfs_bitmap_change_extents(mnt, lcn, run, run_count, 1);
    if (status < 0) goto out;

    uint32_t old_after = root_offset + root_length, new_after = root_offset + new_root_length;
    memmove(record + new_after, record + old_after, used - old_after);
    if (new_after < old_after) memset(record + used - (old_after - new_after), 0, old_after - new_after);
    used   = used - root_length + new_root_length;
    root   = (attr_rec_t *)(record + root_offset);
    header = (uint8_t *)root + value_offset + 0x10;
    memset(header + first, 0, 24);
    store_le16(header + first + 8, 24);
    store_le16(header + first + 12, INDEX_ENTRY_NODE | INDEX_ENTRY_END);
    store_le32(header + 4, first + 24);
    store_le32(header + 8, first + 24);
    header[12] = INDEX_ENTRY_NODE;
    store_le32((uint8_t *)&root->length, new_root_length);
    store_le32((uint8_t *)&root->d.res.value_length, value_length - (total - first) + 24);

    uint32_t end = used - 8;
    memmove(record + end + ia_length, record + end, 8);
    memset(record + end, 0, ia_length);
    attr_rec_t *allocation = (attr_rec_t *)(record + end);
    store_le32((uint8_t *)&allocation->type, AT_INDEX_ALLOCATION);
    store_le32((uint8_t *)&allocation->length, ia_length);
    allocation->non_resident = 1;
    allocation->name_length  = 4;
    store_le16((uint8_t *)&allocation->name_offset, 64);
    store_le16((uint8_t *)&allocation->instance, load_le16(record + 0x28));
    store_le64((uint8_t *)&allocation->d.nres.highest_vcn, clusters - 1);
    store_le16((uint8_t *)&allocation->d.nres.mapping_pairs_off, 72);
    store_le64((uint8_t *)&allocation->d.nres.alloc_size, clusters * mnt->cluster_size);
    store_le64((uint8_t *)&allocation->d.nres.data_size, mnt->indx_size);
    store_le64((uint8_t *)&allocation->d.nres.init_size, mnt->indx_size);
    for (uint32_t i = 0; i < 4; i++) store_le16((uint8_t *)allocation + 64 + ((size_t)i * 2), ntfs_i30[i]);
    memcpy((uint8_t *)allocation + 72, mapping, mapping_size);
    used += ia_length;
    uint8_t  bitmap     = 1;
    uint32_t bitmap_end = ntfs_resident_attribute(record, used - 8, AT_BITMAP, load_le16(record + 0x28) + 1, ntfs_i30, 4, &bitmap, 1);
    store_le32(record + bitmap_end, AT_END);
    store_le32(record + 0x18, bitmap_end + 8);
    store_le16(record + 0x28, load_le16(record + 0x28) + 2);
    status = 0;
out:
    free(indx);
    free(temporary);
    return status;
}

/* Build an index entry for a file name. */
static uint32_t ntfs_index_entry_build(uint8_t *entry, uint32_t capacity, uint64_t file_reference, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, uint32_t file_attributes,
                                       uint64_t data_size, uint64_t allocated_size)
{
    uint16_t key_length   = sizeof(fname_attr_t) + ((size_t)name_length * 2);
    uint32_t entry_length = ntfs_align8(16 + key_length);
    if (!entry || !name || !name_length || entry_length > capacity) return 0;
    memset(entry, 0, entry_length);
    store_le64(entry, file_reference);
    store_le16(entry + 8, entry_length);
    store_le16(entry + 10, key_length);
    fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
    store_le64((uint8_t *)&file_name->parent_dir, parent_reference);
    store_le64((uint8_t *)&file_name->alloc_size, allocated_size);
    store_le64((uint8_t *)&file_name->data_size, data_size);
    store_le32((uint8_t *)&file_name->fa, file_attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (uint32_t i = 0; i < name_length; i++) store_le16((uint8_t *)file_name->name + ((size_t)i * 2), name[i]);
    return entry_length;
}

/* Insert a promoted child pointer into the index root. */
static int ntfs_index_root_insert_child(ntfs_mount_t *mnt, uint8_t *record, uint64_t old_child_vcn, uint64_t new_child_vcn, const uint8_t *key)
{
    attr_rec_t *root = NULL;
    uint32_t    used = load_le32(record + 0x18), root_offset = 0, root_length = 0;
    uint32_t    key_entry_length = load_le16(key + 8), key_length = load_le16(key + 10);
    if (!mnt || !record || !key || key_entry_length < 16 || key_length > key_entry_length - 16 || used > mnt->mft_size) return -EINVAL;
    for (uint32_t offset = load_le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length)) {
            root        = attribute;
            root_offset = offset;
            root_length = length;
            break;
        }
        offset += length;
    }
    if (!root || root->non_resident) return -EIO;
    uint16_t value_offset = load_le16((uint8_t *)&root->d.res.value_offset);
    uint32_t value_length = load_le32((uint8_t *)&root->d.res.value_length);
    uint8_t *header       = (uint8_t *)root + value_offset + 0x10;
    uint32_t first = load_le32(header), total = load_le32(header + 4), pointer_offset = 0;
    if (value_offset > root_length || value_length > root_length - value_offset || first < 16 || total > value_length - 0x10) return -EIO;
    for (uint32_t position = first; position + 24 <= total;) {
        uint8_t *entry  = header + position;
        uint16_t length = load_le16(entry + 8), flags = load_le16(entry + 12);
        if (length < 24 || length > total - position || !(flags & INDEX_ENTRY_NODE)) return -EIO;
        if (load_le64(entry + length - 8) == old_child_vcn) {
            pointer_offset = (uint32_t)(entry - record);
            break;
        }
        position += length;
    }
    if (!pointer_offset) return -EIO;
    uint32_t promoted_length = ntfs_align8(16 + key_length + 8);
    if (promoted_length > mnt->mft_size - used) return -ENOSPC;
    memmove(record + pointer_offset + promoted_length, record + pointer_offset, used - pointer_offset);
    memset(record + pointer_offset, 0, promoted_length);
    memcpy(record + pointer_offset, key, 16 + key_length);
    store_le16(record + pointer_offset + 8, promoted_length);
    store_le16(record + pointer_offset + 12, INDEX_ENTRY_NODE);
    store_le64(record + pointer_offset + promoted_length - 8, old_child_vcn);
    uint8_t *moved_pointer = record + pointer_offset + promoted_length;
    store_le64(moved_pointer + load_le16(moved_pointer + 8) - 8, new_child_vcn);
    root   = (attr_rec_t *)(record + root_offset);
    header = (uint8_t *)root + value_offset + 0x10;
    store_le32((uint8_t *)&root->length, root_length + promoted_length);
    store_le32((uint8_t *)&root->d.res.value_length, value_length + promoted_length);
    store_le32(header + 4, total + promoted_length);
    store_le32(header + 8, load_le32(header + 8) + promoted_length);
    store_le32(record + 0x18, used + promoted_length);
    return 0;
}

/* Split a full index block into two blocks and link them into the root. */
static int ntfs_index_allocation_split(ntfs_mount_t *mnt, uint8_t *record, uint8_t *block, uint64_t child_vcn, uint32_t insert_offset, uint64_t file_reference, uint64_t parent_reference,
                                       const uint16_t *name, uint8_t name_length, uint32_t file_attributes, uint64_t data_size, uint64_t allocated_size)
{
    uint8_t *header = block + 0x18;
    uint32_t first = load_le32(header), total = load_le32(header + 4);
    uint32_t area_offset = 0x18 + first, old_size = total - first;
    uint8_t  new_entry[16 + sizeof(fname_attr_t) + ((size_t)255 * 2) + 8];
    uint32_t new_length = ntfs_index_entry_build(new_entry, sizeof(new_entry), file_reference, parent_reference, name, name_length, file_attributes, data_size, allocated_size);
    uint8_t *all = NULL, *left = NULL, *right = NULL, *original = NULL, *record_copy = NULL;
    int64_t  old_vcn[256], old_lcn[256], old_run[256], new_lcn[256], new_run[256];
    int      old_count, new_count, status = -EIO;
    int      bitmap_committed           = 0;
    int      left_committed             = 0;
    uint32_t rollback_allocation_offset = 0;
    uint32_t rollback_allocation_length = 0;
    uint16_t rollback_mapping_offset    = 0;
    uint64_t rollback_stream_size       = 0;

    if (!new_length || insert_offset < area_offset || insert_offset > area_offset + old_size - 16) return -EINVAL;
    uint32_t relative_insert = insert_offset - area_offset;
    all                      = malloc(old_size + new_length);
    left                     = calloc(1, mnt->indx_size);
    right                    = calloc(1, mnt->indx_size);
    original                 = malloc(mnt->indx_size);
    record_copy              = malloc(mnt->mft_size);
    if (!all || !left || !right || !original || !record_copy) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(original, block, mnt->indx_size);
    if (ntfs_record_pack(mnt, original, mnt->indx_size) < 0) goto out;
    memcpy(all, block + area_offset, relative_insert);
    memcpy(all + relative_insert, new_entry, new_length);
    memcpy(all + relative_insert + new_length, block + insert_offset, old_size - relative_insert);
    uint32_t all_size = old_size + new_length, position = 0, entries = 0;
    while (position + 16 <= all_size) {
        uint16_t length = load_le16(all + position + 8), flags = load_le16(all + position + 12);
        if (length < 16 || length > all_size - position) goto out;
        if (flags & INDEX_ENTRY_END) break;
        entries++;
        position += length;
    }
    if (entries < 2 || position + 16 > all_size) goto out;
    uint32_t median_index = entries / 2, median_offset = 0;
    for (uint32_t index = 0; index < median_index; index++) median_offset += load_le16(all + median_offset + 8);
    uint16_t median_length = load_le16(all + median_offset + 8);
    uint32_t right_offset  = median_offset + median_length;
    uint32_t right_size    = all_size - right_offset;
    uint32_t left_size     = median_offset + 16;
    if (0x40 + left_size > mnt->indx_size - 2 || 0x40 + right_size > mnt->indx_size - 2) {
        status = -ENOSPC;
        goto out;
    }

    memcpy(record_copy, record, mnt->mft_size);
    attr_rec_t *allocation = NULL, *index_bitmap = NULL;
    uint32_t    used = load_le32(record_copy + 0x18);
    for (uint32_t attr_offset = load_le16(record_copy + 0x14); attr_offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record_copy + attr_offset);
        uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || attr_offset > used - length) goto out;
        if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
        if (type == AT_BITMAP && ntfs_attr_name_is_i30(attribute, length)) index_bitmap = attribute;
        attr_offset += length;
    }
    if (!allocation || !allocation->non_resident || !index_bitmap || index_bitmap->non_resident) goto out;
    uint32_t allocation_length  = load_le32((uint8_t *)&allocation->length);
    uint16_t mapping_offset     = load_le16((uint8_t *)&allocation->d.nres.mapping_pairs_off);
    uint64_t stream_size        = load_le64((uint8_t *)&allocation->d.nres.data_size);
    uint64_t allocated_clusters = load_le64((uint8_t *)&allocation->d.nres.alloc_size) / mnt->cluster_size;
    if (!stream_size || stream_size % mnt->indx_size || mapping_offset < 64 || mapping_offset >= allocation_length) goto out;
    old_count = ntfs_runlist_parse((uint8_t *)allocation + mapping_offset, allocation_length - mapping_offset, old_vcn, old_lcn, old_run, 256);
    if (old_count <= 0 || (uint64_t)(old_vcn[old_count - 1] + old_run[old_count - 1]) != allocated_clusters) goto out;
    rollback_allocation_offset = (uint32_t)((uint8_t *)allocation - record_copy);
    rollback_allocation_length = allocation_length;
    rollback_mapping_offset    = mapping_offset;
    rollback_stream_size       = stream_size;
    uint64_t required_clusters = (stream_size + mnt->indx_size + mnt->cluster_size - 1) / mnt->cluster_size;
    uint64_t new_clusters      = required_clusters > allocated_clusters ? required_clusters - allocated_clusters : 0;
    uint64_t final_clusters    = allocated_clusters + new_clusters;
    new_count                  = 0;
    if (new_clusters) {
        status = ntfs_bitmap_find_free(mnt, new_clusters, new_lcn, new_run, &new_count);
        if (status < 0) goto out;
    }
    for (int extent = 0; extent < new_count; extent++) {
        if (old_count && old_lcn[old_count - 1] + old_run[old_count - 1] == new_lcn[extent]) {
            old_run[old_count - 1] += new_run[extent];
        } else {
            if (old_count == 256) {
                status = -ENOSPC;
                goto out;
            }
            old_vcn[old_count] = allocated_clusters;
            for (int prior = 0; prior < extent; prior++) old_vcn[old_count] += new_run[prior];
            old_lcn[old_count]   = new_lcn[extent];
            old_run[old_count++] = new_run[extent];
        }
    }
    uint8_t mapping[2048];
    int     mapping_size = ntfs_runlist_encode(old_lcn, old_run, old_count, mapping, sizeof(mapping));
    if (mapping_size < 0) {
        status = mapping_size;
        goto out;
    }
    if ((uint32_t)mapping_size > allocation_length - mapping_offset) {
        uint32_t new_allocation_length = ntfs_align8(mapping_offset + (uint32_t)mapping_size);
        uint32_t allocation_offset     = (uint32_t)((uint8_t *)allocation - record_copy);
        used                           = load_le32(record_copy + 0x18);
        if (new_allocation_length - allocation_length > mnt->mft_size - used) {
            status = -ENOSPC;
            goto out;
        }
        memmove(record_copy + allocation_offset + new_allocation_length, record_copy + allocation_offset + allocation_length, used - allocation_offset - allocation_length);
        memset(record_copy + allocation_offset + allocation_length, 0, new_allocation_length - allocation_length);
        store_le32(record_copy + 0x18, used + new_allocation_length - allocation_length);
        allocation = (attr_rec_t *)(record_copy + allocation_offset);
        store_le32((uint8_t *)&allocation->length, new_allocation_length);
    }
    uint64_t new_child_vcn = stream_size / ntfs_index_vcn_size(mnt);
    status                 = ntfs_index_root_insert_child(mnt, record_copy, child_vcn, new_child_vcn, all + median_offset);
    if (status < 0) goto out;
    allocation   = NULL;
    index_bitmap = NULL;
    used         = load_le32(record_copy + 0x18);
    for (uint32_t attr_offset = load_le16(record_copy + 0x14); attr_offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record_copy + attr_offset);
        uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
        if (type == AT_BITMAP && ntfs_attr_name_is_i30(attribute, length)) index_bitmap = attribute;
        attr_offset += length;
    }
    if (!allocation || !index_bitmap) goto out;
    uint16_t bitmap_value_offset    = load_le16((uint8_t *)&index_bitmap->d.res.value_offset);
    uint32_t bitmap_value_length    = load_le32((uint8_t *)&index_bitmap->d.res.value_length);
    uint64_t block_index            = stream_size / mnt->indx_size;
    uint32_t required_bitmap_length = (uint32_t)((block_index / 8) + 1);
    uint32_t index_bitmap_length    = load_le32((uint8_t *)&index_bitmap->length);
    if (bitmap_value_offset > index_bitmap_length || bitmap_value_length > index_bitmap_length - bitmap_value_offset) goto out;
    if (required_bitmap_length > bitmap_value_length) {
        uint32_t new_bitmap_length = ntfs_align8(bitmap_value_offset + required_bitmap_length);
        uint32_t bitmap_offset     = (uint32_t)((uint8_t *)index_bitmap - record_copy);
        used                       = load_le32(record_copy + 0x18);
        if (new_bitmap_length > index_bitmap_length && new_bitmap_length - index_bitmap_length > mnt->mft_size - used) {
            status = -ENOSPC;
            goto out;
        }
        memmove(record_copy + bitmap_offset + new_bitmap_length, record_copy + bitmap_offset + index_bitmap_length, used - bitmap_offset - index_bitmap_length);
        memset(record_copy + bitmap_offset + bitmap_value_offset + bitmap_value_length, 0, new_bitmap_length - bitmap_value_offset - bitmap_value_length);
        store_le32(record_copy + 0x18, used - index_bitmap_length + new_bitmap_length);
        index_bitmap = (attr_rec_t *)(record_copy + bitmap_offset);
        store_le32((uint8_t *)&index_bitmap->length, new_bitmap_length);
        store_le32((uint8_t *)&index_bitmap->d.res.value_length, required_bitmap_length);
        allocation = NULL;
        used       = load_le32(record_copy + 0x18);
        for (uint32_t attr_offset = load_le16(record_copy + 0x14); attr_offset + 24 <= used;) {
            attr_rec_t *attribute = (attr_rec_t *)(record_copy + attr_offset);
            uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
            if (type == AT_END) break;
            if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
            attr_offset += length;
        }
        if (!allocation) goto out;
    }
    *((uint8_t *)index_bitmap + bitmap_value_offset + (block_index / 8)) |= (uint8_t)(1U << (block_index & 7));
    allocation_length = load_le32((uint8_t *)&allocation->length);
    mapping_offset    = load_le16((uint8_t *)&allocation->d.nres.mapping_pairs_off);
    memset((uint8_t *)allocation + mapping_offset, 0, allocation_length - mapping_offset);
    memcpy((uint8_t *)allocation + mapping_offset, mapping, mapping_size);
    store_le64((uint8_t *)&allocation->d.nres.highest_vcn, final_clusters - 1);
    store_le64((uint8_t *)&allocation->d.nres.alloc_size, final_clusters * mnt->cluster_size);
    store_le64((uint8_t *)&allocation->d.nres.data_size, stream_size + mnt->indx_size);
    store_le64((uint8_t *)&allocation->d.nres.init_size, stream_size + mnt->indx_size);

    uint8_t *old_terminal = all + all_size - 16;
    store_le32(left, INDX_MAGIC);
    store_le16(left + 4, 0x28);
    store_le16(left + 6, (mnt->indx_size / mnt->sector_size) + 1);
    store_le64(left + 16, child_vcn);
    store_le32(left + 0x18, 0x28);
    store_le32(left + 0x1c, 0x28 + left_size);
    store_le32(left + 0x20, mnt->indx_size - 0x18);
    memcpy(left + 0x40, all, median_offset);
    memcpy(left + 0x40 + median_offset, old_terminal, 16);
    store_le32(right, INDX_MAGIC);
    store_le16(right + 4, 0x28);
    store_le16(right + 6, (mnt->indx_size / mnt->sector_size) + 1);
    store_le64(right + 16, new_child_vcn);
    store_le32(right + 0x18, 0x28);
    store_le32(right + 0x1c, 0x28 + right_size);
    store_le32(right + 0x20, mnt->indx_size - 0x18);
    memcpy(right + 0x40, all + right_offset, right_size);
    if (ntfs_record_pack(mnt, left, mnt->indx_size) < 0 || ntfs_record_pack(mnt, right, mnt->indx_size) < 0) goto out;
    if (ntfs_write_by_runlist(mnt, mapping, mapping_size, stream_size, right, mnt->indx_size, stream_size + mnt->indx_size) != (int64_t)mnt->indx_size) goto out;
    status = ntfs_bitmap_change_extents(mnt, new_lcn, new_run, new_count, 1);
    if (status < 0) goto out;
    bitmap_committed = 1;
    left_committed   = 1;
    if (ntfs_write_by_runlist(mnt, mapping, mapping_size, child_vcn * ntfs_index_vcn_size(mnt), left, mnt->indx_size, stream_size + mnt->indx_size) != (int64_t)mnt->indx_size) {
        status = -EIO;
        goto out;
    }
    memcpy(record, record_copy, mnt->mft_size);
    status = 0;
out:
    if (status < 0 && left_committed) {
        attr_rec_t *rollback_allocation = (attr_rec_t *)(record + rollback_allocation_offset);
        if (rollback_mapping_offset >= 64 && rollback_mapping_offset < rollback_allocation_length)
            ntfs_write_by_runlist(mnt, (uint8_t *)rollback_allocation + rollback_mapping_offset, (int)(rollback_allocation_length - rollback_mapping_offset), child_vcn * ntfs_index_vcn_size(mnt),
                                  original, mnt->indx_size, rollback_stream_size);
    }
    if (status < 0 && bitmap_committed) ntfs_bitmap_change_extents(mnt, new_lcn, new_run, new_count, 0);
    free(record_copy);
    free(original);
    free(right);
    free(left);
    free(all);
    return status;
}

/* Insert an entry into the nonresident $I30 index allocation. */
static int ntfs_index_allocation_insert(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, uint32_t file_attributes,
                                        uint64_t data_size, uint64_t allocated_size)
{
    attr_rec_t *allocation = NULL;
    attr_rec_t *root       = NULL;
    uint32_t    used = load_le32(record + 0x18), offset = load_le16(record + 0x14);
    uint8_t    *block  = NULL;
    int         status = -EIO;

    if (!mnt || !record || !name || !name_length || used > mnt->mft_size) return -EINVAL;
    while (offset + 24 <= used) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length)) root = attribute;
        if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
        offset += length;
    }
    if (!root || root->non_resident || !allocation || !allocation->non_resident) return -EIO;
    uint32_t allocation_length = load_le32((uint8_t *)&allocation->length);
    uint16_t mapping_offset    = load_le16((uint8_t *)&allocation->d.nres.mapping_pairs_off);
    uint64_t stream_size       = load_le64((uint8_t *)&allocation->d.nres.data_size);
    if (mapping_offset < 64 || mapping_offset >= allocation_length || stream_size < mnt->indx_size) return -EIO;
    uint16_t root_value_offset = load_le16((uint8_t *)&root->d.res.value_offset);
    uint32_t root_value_length = load_le32((uint8_t *)&root->d.res.value_length);
    if (root_value_offset > load_le32((uint8_t *)&root->length) || root_value_length < 40 || root_value_length > load_le32((uint8_t *)&root->length) - root_value_offset) return -EIO;
    uint8_t *root_header = (uint8_t *)root + root_value_offset + 0x10;
    uint32_t root_first = load_le32(root_header), root_total = load_le32(root_header + 4);
    uint64_t child_vcn = UINT64_MAX;
    if (!(root_header[12] & INDEX_ENTRY_NODE) || root_first < 16 || root_total < root_first + 24 || root_total > root_value_length - 0x10) return -EIO;
    for (uint32_t position = root_first; position + 24 <= root_total;) {
        uint8_t *entry      = root_header + position;
        uint16_t length     = load_le16(entry + 8);
        uint16_t key_length = load_le16(entry + 10);
        uint16_t flags      = load_le16(entry + 12);
        if (length < 24 || length > root_total - position || !(flags & INDEX_ENTRY_NODE)) return -EIO;
        if (flags & INDEX_ENTRY_END) {
            child_vcn = load_le64(entry + length - 8);
            break;
        }
        if (key_length < sizeof(fname_attr_t) || key_length > length - 24) return -EIO;
        fname_attr_t *root_name = (fname_attr_t *)(entry + 16);
        if ((size_t)root_name->name_len * 2 > key_length - sizeof(fname_attr_t)) return -EIO;
        uint16_t existing[255];
        for (uint32_t i = 0; i < root_name->name_len; i++) existing[i] = load_le16((uint8_t *)root_name->name + ((size_t)i * 2));
        int comparison = ntfs_utf16_compare(mnt, name, name_length, existing, root_name->name_len);
        if (!comparison) return -EEXIST;
        if (comparison < 0) {
            child_vcn = load_le64(entry + length - 8);
            break;
        }
        position += length;
    }
    uint32_t vcn_size = ntfs_index_vcn_size(mnt);
    if (!vcn_size || child_vcn == UINT64_MAX || child_vcn > UINT64_MAX / vcn_size) return -EIO;
    uint64_t block_offset = child_vcn * vcn_size;
    if (block_offset > stream_size || mnt->indx_size > stream_size - block_offset) return -EIO;
    block = malloc(mnt->indx_size);
    if (!block) return -ENOMEM;
    if (ntfs_read_by_runlist(mnt, (uint8_t *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block, mnt->indx_size, stream_size) != (int)mnt->indx_size
        || ntfs_record_unpack(mnt, block, mnt->indx_size) < 0 || load_le32(block) != INDX_MAGIC) {
        goto out;
    }
    uint8_t *header = block + 0x18;
    uint32_t first = load_le32(header), total = load_le32(header + 4), capacity = load_le32(header + 8), insert = 0;
    if (first < 16 || total < first + 16 || capacity < total || capacity > mnt->indx_size - 0x18 || header[12] & INDEX_ENTRY_NODE) goto out;
    for (uint32_t position = first; position + 16 <= total;) {
        uint8_t *entry  = header + position;
        uint16_t length = load_le16(entry + 8), key_length = load_le16(entry + 10), flags = load_le16(entry + 12);
        if (length < 16 || length > total - position) goto out;
        if (flags & INDEX_ENTRY_END) {
            insert = (uint32_t)(entry - block);
            break;
        }
        if (key_length < sizeof(fname_attr_t) || key_length > length - 16) goto out;
        fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
        uint16_t      existing[255];
        if ((size_t)file_name->name_len * 2 > key_length - sizeof(fname_attr_t)) goto out;
        for (uint32_t i = 0; i < file_name->name_len; i++) existing[i] = load_le16((uint8_t *)file_name->name + ((size_t)i * 2));
        int comparison = ntfs_utf16_compare(mnt, name, name_length, existing, file_name->name_len);
        if (!comparison) {
            status = -EEXIST;
            goto out;
        }
        if (comparison < 0) {
            insert = (uint32_t)(entry - block);
            break;
        }
        position += length;
    }
    if (!insert) goto out;
    uint16_t key_length   = sizeof(fname_attr_t) + ((size_t)name_length * 2);
    uint32_t entry_length = ntfs_align8(16 + key_length);
    if (entry_length > capacity - total || insert > mnt->indx_size - entry_length) {
        status = ntfs_index_allocation_split(mnt, record, block, child_vcn, insert, file_reference, parent_reference, name, name_length, file_attributes, data_size, allocated_size);
        goto out;
    }
    memmove(block + insert + entry_length, block + insert, 0x18 + total - insert);
    memset(block + insert, 0, entry_length);
    store_le64(block + insert, file_reference);
    store_le16(block + insert + 8, entry_length);
    store_le16(block + insert + 10, key_length);
    fname_attr_t *file_name = (fname_attr_t *)(block + insert + 16);
    store_le64((uint8_t *)&file_name->parent_dir, parent_reference);
    store_le64((uint8_t *)&file_name->alloc_size, allocated_size);
    store_le64((uint8_t *)&file_name->data_size, data_size);
    store_le32((uint8_t *)&file_name->fa, file_attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (uint32_t i = 0; i < name_length; i++) store_le16((uint8_t *)file_name->name + ((size_t)i * 2), name[i]);
    store_le32(header + 4, total + entry_length);
    status = ntfs_record_pack(mnt, block, mnt->indx_size);
    if (status < 0) goto out;
    status = ntfs_write_by_runlist(mnt, (uint8_t *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block, mnt->indx_size, stream_size) == (int64_t)mnt->indx_size ? 0 :
                                                                                                                                                                                                   -EIO;
out:
    free(block);
    return status;
}

/* Insert a directory entry, promoting to a nonresident index when full. */
int ntfs_directory_index_insert(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, uint32_t file_attributes,
                                uint64_t data_size, uint64_t allocated_size)
{
    uint32_t used = load_le32(record + 0x18);
    if (!mnt || !record || used > mnt->mft_size) return -EINVAL;
    for (uint32_t offset = load_le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length) && !attribute->non_resident) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < 32 || value_length > length - value_offset) return -EIO;
            uint8_t *header = (uint8_t *)attribute + value_offset + 0x10;
            if (header[12] & INDEX_ENTRY_NODE) return ntfs_index_allocation_insert(mnt, record, file_reference, parent_reference, name, name_length, file_attributes, data_size, allocated_size);
            int status = ntfs_index_root_insert(mnt, record, file_reference, parent_reference, name, name_length, file_attributes, data_size, allocated_size);
            if (status != -ENOSPC) return status;
            return ntfs_index_root_promote(mnt, record, file_reference, parent_reference, name, name_length, file_attributes, data_size, allocated_size);
        }
        offset += length;
    }
    return -EIO;
}

/* Replace the key of an index root entry. */
static int ntfs_index_root_replace_key(ntfs_mount_t *mnt, uint8_t *record, uint32_t entry_offset, const uint8_t *replacement)
{
    attr_rec_t *root = NULL;
    uint32_t    used = load_le32(record + 0x18), root_offset = 0, root_length = 0;
    if (!mnt || !record || !replacement || used > mnt->mft_size || entry_offset >= used) return -EINVAL;
    for (uint32_t offset = load_le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length)) {
            root        = attribute;
            root_offset = offset;
            root_length = length;
            break;
        }
        offset += length;
    }
    if (!root || root->non_resident) return -EIO;
    uint16_t old_length = load_le16(record + entry_offset + 8), key_length = load_le16(replacement + 10);
    if (old_length < 24 || key_length < sizeof(fname_attr_t) || key_length > load_le16(replacement + 8) - 16) return -EIO;
    uint32_t new_length = ntfs_align8(16 + key_length + 8);
    if (new_length > old_length && new_length - old_length > mnt->mft_size - used) return -ENOSPC;
    uint64_t child_vcn = load_le64(record + entry_offset + old_length - 8);
    memmove(record + entry_offset + new_length, record + entry_offset + old_length, used - entry_offset - old_length);
    if (new_length < old_length) memset(record + used - (old_length - new_length), 0, old_length - new_length);
    memset(record + entry_offset, 0, new_length);
    memcpy(record + entry_offset, replacement, 16 + key_length);
    store_le16(record + entry_offset + 8, new_length);
    store_le16(record + entry_offset + 12, INDEX_ENTRY_NODE);
    store_le64(record + entry_offset + new_length - 8, child_vcn);
    root                  = (attr_rec_t *)(record + root_offset);
    uint16_t value_offset = load_le16((uint8_t *)&root->d.res.value_offset);
    uint8_t *header       = (uint8_t *)root + value_offset + 0x10;
    store_le32((uint8_t *)&root->length, root_length - old_length + new_length);
    store_le32((uint8_t *)&root->d.res.value_length, load_le32((uint8_t *)&root->d.res.value_length) - old_length + new_length);
    store_le32(header + 4, load_le32(header + 4) - old_length + new_length);
    store_le32(header + 8, load_le32(header + 8) - old_length + new_length);
    store_le32(record + 0x18, used - old_length + new_length);
    return 0;
}

/* Free a collected index item list. */
void ntfs_index_items_free(ntfs_index_item_t *items, uint32_t count)
{
    if (!items) return;
    for (uint32_t index = 0; index < count; index++) free(items[index].entry);
    free(items);
}

/* Locate the index root, allocation, and bitmap attributes. */
static int ntfs_index_attributes_find(ntfs_mount_t *mnt, uint8_t *record, ntfs_index_attributes_t *attributes)
{
    uint32_t used;
    uint32_t offset;

    if (!mnt || !record || !attributes) return -EINVAL;
    memset(attributes, 0, sizeof(*attributes));
    used = load_le32(record + 0x18);
    if (used > mnt->mft_size) return -EIO;
    offset = load_le16(record + 0x14);
    while (offset + 24 <= used) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        if (ntfs_attr_name_is_i30(attribute, length)) {
            if (type == AT_INDEX_ROOT) {
                if (attributes->root) return -EIO;
                attributes->root        = attribute;
                attributes->root_offset = offset;
            } else if (type == AT_INDEX_ALLOCATION) {
                if (attributes->allocation) return -EIO;
                attributes->allocation        = attribute;
                attributes->allocation_offset = offset;
            } else if (type == AT_BITMAP) {
                if (attributes->bitmap) return -EIO;
                attributes->bitmap        = attribute;
                attributes->bitmap_offset = offset;
            }
        }
        offset += length;
    }
    return attributes->root ? 0 : -EIO;
}

/* Test one bit of an index bitmap. */
static int ntfs_index_bitmap_test(ntfs_mount_t *mnt, attr_rec_t *bitmap, uint64_t bit, int *set)
{
    uint32_t length;
    uint8_t  value;

    if (!mnt || !bitmap || !set) return -EINVAL;
    length = load_le32((uint8_t *)&bitmap->length);
    if (bitmap->non_resident) {
        uint16_t mapping_offset = load_le16((uint8_t *)&bitmap->d.nres.mapping_pairs_off);
        uint64_t data_size      = load_le64((uint8_t *)&bitmap->d.nres.data_size);
        if (length < 64 || mapping_offset < 64 || mapping_offset >= length || bit / 8 >= data_size
            || ntfs_read_by_runlist(mnt, (uint8_t *)bitmap + mapping_offset, (int)(length - mapping_offset), bit / 8, &value, 1, data_size) != 1) {
            return -EIO;
        }
    } else {
        uint16_t value_offset = load_le16((uint8_t *)&bitmap->d.res.value_offset);
        uint32_t value_length = load_le32((uint8_t *)&bitmap->d.res.value_length);
        if (value_offset > length || value_length > length - value_offset || bit / 8 >= value_length) return -EIO;
        value = *((uint8_t *)bitmap + value_offset + (bit / 8));
    }
    *set = !!(value & (1U << (bit & 7)));
    return 0;
}

/* Append one index item to the list. */
static int ntfs_index_item_append(ntfs_index_item_t **items, uint32_t *count, uint32_t *capacity, const uint8_t *entry, int node)
{
    uint16_t key_length;
    uint16_t entry_length;
    uint32_t stored_length;
    uint8_t *copy;

    if (!items || !count || !capacity || !entry) return -EINVAL;
    entry_length = load_le16(entry + 8);
    key_length   = load_le16(entry + 10);
    if (entry_length < (node ? 24 : 16) || key_length < sizeof(fname_attr_t) || key_length > entry_length - (node ? 24 : 16)) return -EIO;
    fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
    if ((size_t)file_name->name_len * 2 > key_length - sizeof(fname_attr_t)) return -EIO;
    stored_length = ntfs_align8(16 + key_length);
    copy          = calloc(1, stored_length);
    if (!copy) return -ENOMEM;
    memcpy(copy, entry, 16 + key_length);
    store_le16(copy + 8, (uint16_t)stored_length);
    store_le16(copy + 12, 0);
    if (*count == *capacity) {
        uint32_t new_capacity = *capacity ? *capacity * 2 : 16;
        if (new_capacity < *capacity || new_capacity > UINT32_MAX / sizeof(**items)) {
            free(copy);
            return -EOVERFLOW;
        }
        ntfs_index_item_t *grown = realloc(*items, (size_t)new_capacity * sizeof(**items));
        if (!grown) {
            free(copy);
            return -ENOMEM;
        }
        *items    = grown;
        *capacity = new_capacity;
    }
    (*items)[*count].entry  = copy;
    (*items)[*count].length = (uint16_t)stored_length;
    (*count)++;
    return 0;
}

/* Collect one index header entry into the item list. */
static int ntfs_index_collect_header(ntfs_index_item_t **items, uint32_t *count, uint32_t *capacity, uint8_t *buffer, uint32_t buffer_size, uint32_t header_offset)
{
    uint8_t *header;
    uint32_t first;
    uint32_t total;
    int      node;

    if (!buffer || header_offset > buffer_size || buffer_size - header_offset < 16) return -EIO;
    header = buffer + header_offset;
    first  = load_le32(header);
    total  = load_le32(header + 4);
    node   = !!(header[12] & INDEX_ENTRY_NODE);
    if (first < 16 || total < first + (node ? 24U : 16U) || total > buffer_size - header_offset) return -EIO;
    for (uint32_t position = first; position + (node ? 24U : 16U) <= total;) {
        uint8_t *entry  = header + position;
        uint16_t length = load_le16(entry + 8);
        uint16_t flags  = load_le16(entry + 12);
        if (length < (node ? 24U : 16U) || length > total - position || (!!(flags & INDEX_ENTRY_NODE) != node)) return -EIO;
        if (flags & INDEX_ENTRY_END) {
            if (position + length != total) return -EIO;
            return 0;
        }
        int status = ntfs_index_item_append(items, count, capacity, entry, node);
        if (status < 0) return status;
        position += length;
    }
    return -EIO;
}

/* Flatten every entry of a directory index into an item list. */
int ntfs_directory_index_collect(ntfs_mount_t *mnt, uint8_t *record, ntfs_index_item_t **result, uint32_t *result_count, ntfs_index_attributes_t *result_attributes)
{
    ntfs_index_attributes_t attributes;
    ntfs_index_item_t      *items    = NULL;
    uint32_t                count    = 0;
    uint32_t                capacity = 0;
    uint8_t                *block    = NULL;
    int                     status;

    if (!mnt || !record || !result || !result_count) return -EINVAL;
    status = ntfs_index_attributes_find(mnt, record, &attributes);
    if (status < 0) return status;
    if (attributes.root->non_resident) return -EIO;
    uint32_t root_length       = load_le32((uint8_t *)&attributes.root->length);
    uint16_t root_value_offset = load_le16((uint8_t *)&attributes.root->d.res.value_offset);
    uint32_t root_value_length = load_le32((uint8_t *)&attributes.root->d.res.value_length);
    if (root_value_offset > root_length || root_value_length < 32 || root_value_length > root_length - root_value_offset) return -EIO;
    status = ntfs_index_collect_header(&items, &count, &capacity, (uint8_t *)attributes.root + root_value_offset, root_value_length, 0x10);
    if (status < 0) goto out;

    if (attributes.allocation || attributes.bitmap) {
        if (!attributes.allocation || !attributes.bitmap || !attributes.allocation->non_resident) {
            status = -EIO;
            goto out;
        }
        uint32_t allocation_length = load_le32((uint8_t *)&attributes.allocation->length);
        uint16_t mapping_offset    = load_le16((uint8_t *)&attributes.allocation->d.nres.mapping_pairs_off);
        uint64_t stream_size       = load_le64((uint8_t *)&attributes.allocation->d.nres.data_size);
        if (allocation_length < 64 || mapping_offset < 64 || mapping_offset >= allocation_length || !mnt->indx_size || stream_size % mnt->indx_size) goto corrupt;
        block = malloc(mnt->indx_size);
        if (!block) {
            status = -ENOMEM;
            goto out;
        }
        uint64_t block_count = stream_size / mnt->indx_size;
        for (uint64_t index = 0; index < block_count; index++) {
            int allocated;
            status = ntfs_index_bitmap_test(mnt, attributes.bitmap, index, &allocated);
            if (status < 0) goto out;
            if (!allocated) continue;
            uint64_t logical_offset = index * (uint64_t)mnt->indx_size;
            if (ntfs_read_by_runlist(mnt, (uint8_t *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), logical_offset, block, mnt->indx_size, stream_size)
                    != (int)mnt->indx_size
                || ntfs_record_unpack(mnt, block, mnt->indx_size) < 0 || load_le32(block) != INDX_MAGIC) {
                status = -EIO;
                goto out;
            }
            status = ntfs_index_collect_header(&items, &count, &capacity, block, mnt->indx_size, 0x18);
            if (status < 0) goto out;
        }
    }
    *result       = items;
    *result_count = count;
    if (result_attributes) *result_attributes = attributes;
    free(block);
    return 0;
corrupt:
    status = -EIO;
out:
    free(block);
    ntfs_index_items_free(items, count);
    return status;
}

/* Set the reparse tag of a directory index entry. */
int ntfs_directory_index_set_reparse_tag(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, const uint16_t *name, uint8_t name_length)
{
    ntfs_index_attributes_t attributes;
    uint8_t                *block = NULL;
    int                     status;

    status = ntfs_index_root_set_reparse_tag(mnt, record, file_reference, name, name_length);
    if (status != -ENOENT) return status;
    status = ntfs_index_attributes_find(mnt, record, &attributes);
    if (status < 0) return status;
    if (!attributes.allocation || !attributes.bitmap || !attributes.allocation->non_resident) return -ENOENT;
    uint32_t allocation_length = load_le32((uint8_t *)&attributes.allocation->length);
    uint16_t mapping_offset    = load_le16((uint8_t *)&attributes.allocation->d.nres.mapping_pairs_off);
    uint64_t stream_size       = load_le64((uint8_t *)&attributes.allocation->d.nres.data_size);
    if (allocation_length < 64 || mapping_offset < 64 || mapping_offset >= allocation_length || !mnt->indx_size || stream_size % mnt->indx_size) return -EIO;
    block = malloc(mnt->indx_size);
    if (!block) return -ENOMEM;
    for (uint64_t index = 0; index < stream_size / mnt->indx_size; index++) {
        int allocated;
        status = ntfs_index_bitmap_test(mnt, attributes.bitmap, index, &allocated);
        if (status < 0) goto out;
        if (!allocated) continue;
        uint64_t logical_offset = index * (uint64_t)mnt->indx_size;
        if (ntfs_read_by_runlist(mnt, (uint8_t *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), logical_offset, block, mnt->indx_size, stream_size)
                != (int)mnt->indx_size
            || ntfs_record_unpack(mnt, block, mnt->indx_size) < 0 || load_le32(block) != INDX_MAGIC) {
            status = -EIO;
            goto out;
        }
        uint8_t *header = block + 0x18;
        uint32_t first  = load_le32(header);
        uint32_t total  = load_le32(header + 4);
        int      node   = !!(header[12] & INDEX_ENTRY_NODE);
        if (first < 16 || total < first + (node ? 24U : 16U) || total > mnt->indx_size - 0x18) {
            status = -EIO;
            goto out;
        }
        for (uint32_t position = first; position + (node ? 24U : 16U) <= total;) {
            uint8_t *entry      = header + position;
            uint16_t length     = load_le16(entry + 8);
            uint16_t key_length = load_le16(entry + 10);
            uint16_t flags      = load_le16(entry + 12);
            if (length < (node ? 24U : 16U) || length > total - position || (!!(flags & INDEX_ENTRY_NODE) != node)) {
                status = -EIO;
                goto out;
            }
            if (flags & INDEX_ENTRY_END) break;
            if (key_length < sizeof(fname_attr_t) || key_length > length - (node ? 24U : 16U)) {
                status = -EIO;
                goto out;
            }
            fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
            int           equal     = (load_le64(entry) & 0x0000ffffffffffffULL) == (file_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length;
            for (uint32_t character = 0; equal && character < name_length; character++)
                if (load_le16((uint8_t *)file_name->name + ((size_t)character * 2)) != name[character]) equal = 0;
            if (equal) {
                store_le32((uint8_t *)&file_name->rp_tag, IO_REPARSE_TAG_SYMLINK);
                status = ntfs_record_pack(mnt, block, mnt->indx_size);
                if (status < 0) goto out;
                status = ntfs_write_by_runlist(mnt, (uint8_t *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), logical_offset, block, mnt->indx_size, stream_size)
                                 == (int64_t)mnt->indx_size ?
                             0 :
                             -EIO;
                goto out;
            }
            position += length;
        }
    }
    status = -ENOENT;
out:
    free(block);
    return status;
}

/* Compare two index items for sorting. */
static int ntfs_index_item_compare(ntfs_mount_t *mnt, const ntfs_index_item_t *left, const ntfs_index_item_t *right)
{
    fname_attr_t *left_name  = (fname_attr_t *)(left->entry + 16);
    fname_attr_t *right_name = (fname_attr_t *)(right->entry + 16);
    uint16_t      left_utf16[255];
    uint16_t      right_utf16[255];
    for (uint32_t index = 0; index < left_name->name_len; index++) left_utf16[index] = load_le16((uint8_t *)left_name->name + ((size_t)index * 2));
    for (uint32_t index = 0; index < right_name->name_len; index++) right_utf16[index] = load_le16((uint8_t *)right_name->name + ((size_t)index * 2));
    return ntfs_utf16_compare(mnt, left_utf16, left_name->name_len, right_utf16, right_name->name_len);
}

/* Collapse a sparse directory index back to resident form. */
static int ntfs_index_try_collapse(ntfs_mount_t *mnt, uint8_t *record)
{
    ntfs_index_attributes_t attributes;
    ntfs_index_item_t      *items = NULL;
    uint8_t                *copy  = NULL;
    uint32_t                count = 0;
    int64_t                 vcn[256], lcn[256], run[256];
    int                     run_count;
    int                     status;

    status = ntfs_directory_index_collect(mnt, record, &items, &count, &attributes);
    if (status < 0) return status;
    if (!attributes.allocation) {
        ntfs_index_items_free(items, count);
        return 0;
    }
    for (uint32_t index = 1; index < count; index++) {
        ntfs_index_item_t item     = items[index];
        uint32_t          position = index;
        while (position && ntfs_index_item_compare(mnt, &item, &items[position - 1]) < 0) {
            items[position] = items[position - 1];
            position--;
        }
        items[position] = item;
    }
    for (uint32_t index = 1; index < count; index++) {
        if (!ntfs_index_item_compare(mnt, &items[index - 1], &items[index])) {
            status = -EIO;
            goto out;
        }
    }

    uint64_t entries_length = 16;
    for (uint32_t index = 0; index < count; index++) entries_length += items[index].length;
    uint32_t root_length       = load_le32((uint8_t *)&attributes.root->length);
    uint16_t root_value_offset = load_le16((uint8_t *)&attributes.root->d.res.value_offset);
    uint32_t new_value_length  = 0x10 + 0x10 + (uint32_t)entries_length;
    uint32_t new_root_length   = ntfs_align8(root_value_offset + new_value_length);
    uint32_t allocation_length = load_le32((uint8_t *)&attributes.allocation->length);
    uint32_t bitmap_length     = load_le32((uint8_t *)&attributes.bitmap->length);
    uint32_t used              = load_le32(record + 0x18);
    if (root_length > used || allocation_length > used - root_length || bitmap_length > used - root_length - allocation_length) {
        status = -EIO;
        goto out;
    }
    uint32_t fixed_length = used - root_length - allocation_length - bitmap_length;
    if (entries_length > UINT32_MAX - 0x20 || new_root_length > mnt->mft_size || fixed_length > mnt->mft_size - new_root_length) {
        status = 0;
        goto out;
    }
    uint16_t mapping_offset = load_le16((uint8_t *)&attributes.allocation->d.nres.mapping_pairs_off);
    if (mapping_offset < 64 || mapping_offset >= allocation_length) {
        status = -EIO;
        goto out;
    }
    run_count = ntfs_runlist_parse((uint8_t *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), vcn, lcn, run, 256);
    if (run_count <= 0) {
        status = -EIO;
        goto out;
    }

    copy = malloc(mnt->mft_size);
    if (!copy) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(copy, record, mnt->mft_size);
    uint32_t first_remove  = attributes.allocation_offset > attributes.bitmap_offset ? attributes.allocation_offset : attributes.bitmap_offset;
    uint32_t second_remove = attributes.allocation_offset > attributes.bitmap_offset ? attributes.bitmap_offset : attributes.allocation_offset;
    status                 = ntfs_record_remove_attribute(copy, mnt->mft_size, first_remove);
    if (status < 0) goto out;
    status = ntfs_record_remove_attribute(copy, mnt->mft_size, second_remove);
    if (status < 0) goto out;
    status = ntfs_index_attributes_find(mnt, copy, &attributes);
    if (status < 0 || attributes.allocation || attributes.bitmap) goto corrupt;
    root_length       = load_le32((uint8_t *)&attributes.root->length);
    root_value_offset = load_le16((uint8_t *)&attributes.root->d.res.value_offset);
    used              = load_le32(copy + 0x18);
    if (new_root_length > root_length && new_root_length - root_length > mnt->mft_size - used) goto corrupt;
    memmove(copy + attributes.root_offset + new_root_length, copy + attributes.root_offset + root_length, used - attributes.root_offset - root_length);
    if (new_root_length < root_length) memset(copy + used - (root_length - new_root_length), 0, root_length - new_root_length);
    store_le32(copy + 0x18, used - root_length + new_root_length);
    attributes.root = (attr_rec_t *)(copy + attributes.root_offset);
    store_le32((uint8_t *)&attributes.root->length, new_root_length);
    store_le32((uint8_t *)&attributes.root->d.res.value_length, new_value_length);
    uint8_t *value  = (uint8_t *)attributes.root + root_value_offset;
    uint8_t *header = value + 0x10;
    memset(header, 0, new_value_length - 0x10);
    store_le32(header, 0x10);
    store_le32(header + 4, 0x10 + (uint32_t)entries_length);
    store_le32(header + 8, 0x10 + (uint32_t)entries_length);
    uint32_t position = 0x10;
    for (uint32_t index = 0; index < count; index++) {
        memcpy(header + position, items[index].entry, items[index].length);
        position += items[index].length;
    }
    store_le16(header + position + 8, 16);
    store_le16(header + position + 12, INDEX_ENTRY_END);
    status = ntfs_index_reclaim_stage(mnt, lcn, run, run_count);
    if (status < 0) goto out;
    memcpy(record, copy, mnt->mft_size);
    status = 1;
    goto out;
corrupt:
    status = -EIO;
out:
    free(copy);
    ntfs_index_items_free(items, count);
    return status;
}

/* Remove a directory entry from the index, collapsing it when sparse. */
int ntfs_directory_index_remove(ntfs_mount_t *mnt, uint8_t *record, uint64_t file_reference, const uint16_t *name, uint8_t name_length)
{
    attr_rec_t *root = NULL, *allocation = NULL;
    uint32_t    used = load_le32(record + 0x18);
    if (!mnt || !record || !name || !name_length || used > mnt->mft_size) return -EINVAL;
    for (uint32_t offset = load_le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type = load_le32((uint8_t *)&attribute->type), length = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length)) root = attribute;
        if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
        offset += length;
    }
    if (!root || root->non_resident) return -EIO;
    uint16_t value_offset = load_le16((uint8_t *)&root->d.res.value_offset);
    uint32_t value_length = load_le32((uint8_t *)&root->d.res.value_length);
    if (value_offset > load_le32((uint8_t *)&root->length) || value_length < 32 || value_length > load_le32((uint8_t *)&root->length) - value_offset) return -EIO;
    uint8_t *root_header = (uint8_t *)root + value_offset + 0x10;
    if (!(root_header[12] & INDEX_ENTRY_NODE)) return ntfs_index_root_remove(mnt, record, file_reference, name, name_length);
    if (!allocation || !allocation->non_resident) return -EIO;
    uint32_t first = load_le32(root_header), total = load_le32(root_header + 4);
    uint64_t child_vcn         = UINT64_MAX;
    uint32_t root_match_offset = 0;
    for (uint32_t position = first; position + 24 <= total;) {
        uint8_t *entry  = root_header + position;
        uint16_t length = load_le16(entry + 8), key_length = load_le16(entry + 10), flags = load_le16(entry + 12);
        if (length < 24 || length > total - position || !(flags & INDEX_ENTRY_NODE)) return -EIO;
        if (flags & INDEX_ENTRY_END) {
            child_vcn = load_le64(entry + length - 8);
            break;
        }
        if (key_length < sizeof(fname_attr_t) || key_length > length - 24) return -EIO;
        fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
        uint16_t      existing[255];
        if ((size_t)file_name->name_len * 2 > key_length - sizeof(fname_attr_t)) return -EIO;
        for (uint32_t i = 0; i < file_name->name_len; i++) existing[i] = load_le16((uint8_t *)file_name->name + ((size_t)i * 2));
        int comparison = ntfs_utf16_compare(mnt, name, name_length, existing, file_name->name_len);
        if (!comparison) {
            if ((load_le64(entry) & 0x0000ffffffffffffULL) != (file_reference & 0x0000ffffffffffffULL)) return -ENOENT;
            uint32_t next_position = position + length;
            if (next_position + 24 > total) return -EIO;
            uint8_t *next        = root_header + next_position;
            uint16_t next_length = load_le16(next + 8), next_flags = load_le16(next + 12);
            if (next_length < 24 || next_length > total - next_position || !(next_flags & INDEX_ENTRY_NODE)) return -EIO;
            root_match_offset = (uint32_t)(entry - record);
            child_vcn         = load_le64(next + next_length - 8);
            break;
        }
        if (comparison < 0) {
            child_vcn = load_le64(entry + length - 8);
            break;
        }
        position += length;
    }
    if (child_vcn == UINT64_MAX) return -EIO;
    uint32_t allocation_length = load_le32((uint8_t *)&allocation->length);
    uint16_t mapping_offset    = load_le16((uint8_t *)&allocation->d.nres.mapping_pairs_off);
    uint64_t stream_size       = load_le64((uint8_t *)&allocation->d.nres.data_size);
    uint32_t vcn_size          = ntfs_index_vcn_size(mnt);
    if (!vcn_size || child_vcn > UINT64_MAX / vcn_size) return -EIO;
    uint64_t block_offset = child_vcn * vcn_size;
    if (mapping_offset < 64 || mapping_offset >= allocation_length || block_offset > stream_size || mnt->indx_size > stream_size - block_offset) return -EIO;
    uint8_t *block = malloc(mnt->indx_size);
    if (!block) return -ENOMEM;
    int status = -EIO;
    if (ntfs_read_by_runlist(mnt, (uint8_t *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block, mnt->indx_size, stream_size) != (int)mnt->indx_size
        || ntfs_record_unpack(mnt, block, mnt->indx_size) < 0 || load_le32(block) != INDX_MAGIC) {
        goto out;
    }
    uint8_t *header = block + 0x18;
    first           = load_le32(header);
    total           = load_le32(header + 4);
    if (root_match_offset) {
        uint8_t *successor        = header + first;
        uint16_t successor_length = load_le16(successor + 8), successor_flags = load_le16(successor + 12);
        if (successor_length < 16 || successor_length > total - first || (successor_flags & INDEX_ENTRY_END)) goto out;
        uint8_t saved[16 + sizeof(fname_attr_t) + ((size_t)255 * 2)];
        if (successor_length > sizeof(saved)) {
            status = -EIO;
            goto out;
        }
        memcpy(saved, successor, successor_length);
        memmove(successor, successor + successor_length, 0x18 + total - (uint32_t)(successor + successor_length - block));
        memset(block + 0x18 + total - successor_length, 0, successor_length);
        store_le32(header + 4, total - successor_length);
        status = ntfs_record_pack(mnt, block, mnt->indx_size);
        if (status < 0) goto out;
        status = ntfs_write_by_runlist(mnt, (uint8_t *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block, mnt->indx_size, stream_size) == (int64_t)mnt->indx_size ?
                     0 :
                     -EIO;
        if (status < 0) goto out;
        status = ntfs_index_root_replace_key(mnt, record, root_match_offset, saved);
        goto out;
    }
    for (uint32_t position = first; position + 16 <= total;) {
        uint8_t *entry  = header + position;
        uint16_t length = load_le16(entry + 8), key_length = load_le16(entry + 10), flags = load_le16(entry + 12);
        if (length < 16 || length > total - position) goto out;
        if (flags & INDEX_ENTRY_END) {
            status = -ENOENT;
            goto out;
        }
        if (key_length < sizeof(fname_attr_t) || key_length > length - 16) goto out;
        fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
        int           equal     = (load_le64(entry) & 0x0000ffffffffffffULL) == (file_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length;
        for (uint32_t i = 0; equal && i < name_length; i++)
            if (load_le16((uint8_t *)file_name->name + ((size_t)i * 2)) != name[i]) equal = 0;
        if (equal) {
            memmove(entry, entry + length, 0x18 + total - (uint32_t)(entry + length - block));
            memset(block + 0x18 + total - length, 0, length);
            store_le32(header + 4, total - length);
            status = ntfs_record_pack(mnt, block, mnt->indx_size);
            if (status < 0) goto out;
            status
                = ntfs_write_by_runlist(mnt, (uint8_t *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block, mnt->indx_size, stream_size) == (int64_t)mnt->indx_size ?
                      0 :
                      -EIO;
            goto out;
        }
        position += length;
    }
out:
    if (status == 0) {
        int collapse_status = ntfs_index_try_collapse(mnt, record);
        if (collapse_status < 0) status = collapse_status;
    }
    free(block);
    return status;
}

#endif
