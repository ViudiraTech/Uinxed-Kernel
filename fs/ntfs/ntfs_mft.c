/*
 *
 *      ntfs_mft.c
 *      New Technology File System - MFT records and attributes
 *
 *      2026/7/26 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/ntfs/ntfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <libs/util/byteorder.h>
#include <mem/heap.h>

#if CONFIG_NTFS_FS

/* Update the standard-information and file-name timestamps in a record. */
int ntfs_record_touch(uint8_t *record, uint32_t record_size, int data_changed)
{
    if (!record || record_size < 48 || load_le32(record) != MFT_MAGIC) return -EIO;
    uint32_t used   = load_le32(record + 0x18);
    uint32_t offset = load_le16(record + 0x14);
    uint64_t now    = ntfs_current_filetime();
    if (used > record_size || offset > used) return -EIO;
    while (offset + 24 <= used) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || length > used - offset) return -EIO;
        if (!attribute->non_resident && !attribute->name_length && (type == AT_STANDARD_INFORMATION || type == AT_FILE_NAME)) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            if (value_offset > length || value_length > length - value_offset) return -EIO;
            uint8_t *value = (uint8_t *)attribute + value_offset;
            if (type == AT_STANDARD_INFORMATION) {
                if (value_length < 32) return -EIO;
                if (data_changed) store_le64(value + 8, now);
                store_le64(value + 16, now);
            } else {
                if (value_length < sizeof(fname_attr_t)) return -EIO;
                fname_attr_t *file_name = (fname_attr_t *)value;
                if (data_changed) store_le64((uint8_t *)&file_name->mtime_data, now);
                store_le64((uint8_t *)&file_name->mtime_mft, now);
            }
        }
        offset += length;
    }
    return EOK;
}

/* Validate the update-sequence-array layout of a record. */
static int ntfs_record_layout(ntfs_mount_t *mnt, uint8_t *record, uint32_t record_size, uint16_t *usa_offset, uint16_t *usa_count)
{
    uint32_t usa_end;

    if (!mnt || !record || !usa_offset || !usa_count || !mnt->sector_size || record_size < mnt->sector_size || record_size % mnt->sector_size) return -EINVAL;

    *usa_offset = load_le16(record + 4);
    *usa_count  = load_le16(record + 6);
    usa_end     = (uint32_t)*usa_offset + ((uint32_t)*usa_count * sizeof(uint16_t));
    if (*usa_offset < 8 || (*usa_offset & 1) || *usa_count != (record_size / mnt->sector_size) + 1 || usa_end > record_size || usa_end > mnt->sector_size - sizeof(uint16_t)) return -EIO;
    return 0;
}

/* Verify and restore the USA-replaced bytes when reading a record. */
int ntfs_record_unpack(ntfs_mount_t *mnt, uint8_t *record, uint32_t record_size)
{
    uint16_t usa_offset;
    uint16_t usa_count;
    uint16_t sequence;
    int      status;

    status = ntfs_record_layout(mnt, record, record_size, &usa_offset, &usa_count);
    if (status < 0) return status;
    sequence = load_le16(record + usa_offset);

    for (uint16_t i = 1; i < usa_count; i++) {
        uint32_t trailer = ((size_t)i * mnt->sector_size) - sizeof(uint16_t);
        if (load_le16(record + trailer) != sequence) {
            plogk("ntfs: Drive %u: record checksum mismatch (size %u)\n", mnt->dev.drive, record_size);
            return -EIO;
        }
    }
    for (uint16_t i = 1; i < usa_count; i++) {
        uint32_t trailer = ((size_t)i * mnt->sector_size) - sizeof(uint16_t);
        store_le16(record + trailer, load_le16(record + usa_offset + ((uint32_t)i * sizeof(uint16_t))));
    }
    return 0;
}

/* Apply the USA substitution and sequence number before writing a record. */
int ntfs_record_pack(ntfs_mount_t *mnt, uint8_t *record, uint32_t record_size)
{
    uint16_t usa_offset;
    uint16_t usa_count;
    uint16_t sequence;
    int      status;

    status = ntfs_record_layout(mnt, record, record_size, &usa_offset, &usa_count);
    if (status < 0) return status;

    sequence = (uint16_t)(load_le16(record + usa_offset) + 1);
    if (!sequence) sequence = 1;
    store_le16(record + usa_offset, sequence);
    for (uint16_t i = 1; i < usa_count; i++) {
        uint32_t trailer = ((size_t)i * mnt->sector_size) - sizeof(uint16_t);
        store_le16(record + usa_offset + ((uint32_t)i * sizeof(uint16_t)), load_le16(record + trailer));
        store_le16(record + trailer, sequence);
    }
    return 0;
}

/* Replace a file-name attribute value in a record. */
int ntfs_file_name_replace(ntfs_mount_t *mnt, uint8_t *record, uint64_t parent_reference, const uint16_t *old_name, uint8_t old_length, const uint16_t *new_name, uint8_t new_length)
{
    uint32_t bytes_in_use;
    uint32_t offset;

    if (!mnt || !record || !old_name || !old_length || !new_name || !new_length || load_le32(record) != MFT_MAGIC) return -EINVAL;
    bytes_in_use = load_le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size) return -EIO;
    offset = load_le16(record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        if (type == AT_FILE_NAME && !attribute->non_resident && !attribute->name_length) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < sizeof(fname_attr_t) || value_length > length - value_offset) return -EIO;
            fname_attr_t *file_name = (fname_attr_t *)((uint8_t *)attribute + value_offset);
            if ((load_le64((uint8_t *)&file_name->parent_dir) & 0x0000ffffffffffffULL) == (parent_reference & 0x0000ffffffffffffULL) && file_name->name_len == old_length) {
                int equal = 1;
                for (uint32_t i = 0; i < old_length; i++) {
                    if (load_le16((uint8_t *)file_name->name + (i * sizeof(uint16_t))) != old_name[i]) {
                        equal = 0;
                        break;
                    }
                }
                if (equal) {
                    uint32_t new_value_length = sizeof(fname_attr_t) + ((uint32_t)new_length * sizeof(uint16_t));
                    uint32_t new_attr_length  = ntfs_align8(value_offset + new_value_length);
                    if (new_attr_length > length && new_attr_length - length > mnt->mft_size - bytes_in_use) return -ENOSPC;
                    if (new_attr_length != length) {
                        uint32_t tail_offset = offset + length;
                        memmove(record + offset + new_attr_length, record + tail_offset, bytes_in_use - tail_offset);
                        if (new_attr_length < length) memset(record + bytes_in_use - (length - new_attr_length), 0, length - new_attr_length);
                        bytes_in_use = bytes_in_use - length + new_attr_length;
                        store_le32(record + 0x18, bytes_in_use);
                        store_le32((uint8_t *)&attribute->length, new_attr_length);
                    }
                    file_name = (fname_attr_t *)((uint8_t *)attribute + value_offset);
                    memset((uint8_t *)file_name + sizeof(fname_attr_t), 0, new_attr_length - value_offset - sizeof(fname_attr_t));
                    file_name->name_len = new_length;
                    for (uint32_t i = 0; i < new_length; i++) store_le16((uint8_t *)file_name->name + (i * sizeof(uint16_t)), new_name[i]);
                    store_le32((uint8_t *)&attribute->d.res.value_length, new_value_length);
                    return 0;
                }
            }
        }
        offset += length;
    }
    return -ENOENT;
}

/* Append a file-name attribute to a record. */
int ntfs_file_name_add(ntfs_mount_t *mnt, uint8_t *record, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, uint32_t file_attributes, uint64_t data_size)
{
    uint8_t  value[sizeof(fname_attr_t) + (255 * sizeof(uint16_t))];
    uint32_t bytes_in_use;
    uint32_t offset;
    uint32_t attr_length;

    if (!mnt || !record || !name || !name_length || load_le32(record) != MFT_MAGIC) return -EINVAL;
    bytes_in_use = load_le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size || bytes_in_use < 8) return -EIO;
    offset = load_le16(record + 0x14);
    while (offset + 8 <= bytes_in_use) {
        uint32_t type   = load_le32(record + offset);
        uint32_t length = load_le32(record + offset + 4);
        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        offset += length;
    }
    if (offset + 8 > bytes_in_use || load_le32(record + offset) != AT_END) return -EIO;
    attr_length = ntfs_align8(24 + sizeof(fname_attr_t) + ((uint32_t)name_length * sizeof(uint16_t)));
    if (attr_length > mnt->mft_size - bytes_in_use) return -ENOSPC;
    memset(value, 0, sizeof(fname_attr_t) + ((uint32_t)name_length * sizeof(uint16_t)));
    fname_attr_t *file_name = (fname_attr_t *)value;
    store_le64((uint8_t *)&file_name->parent_dir, parent_reference);
    store_le64((uint8_t *)&file_name->data_size, data_size);
    store_le64((uint8_t *)&file_name->alloc_size, data_size);
    store_le32((uint8_t *)&file_name->fa, file_attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (uint32_t i = 0; i < name_length; i++) store_le16((uint8_t *)file_name->name + (i * sizeof(uint16_t)), name[i]);
    uint16_t instance = load_le16(record + 0x28);
    uint32_t end      = ntfs_resident_attribute(record, offset, AT_FILE_NAME, instance, NULL, 0, value, sizeof(fname_attr_t) + ((uint32_t)name_length * sizeof(uint16_t)));
    store_le32(record + end, AT_END);
    store_le32(record + 0x18, end + 8);
    store_le16(record + 0x28, instance + 1);
    return 0;
}

/* Remove a file-name attribute matching the given parent and name. */
int ntfs_file_name_remove(ntfs_mount_t *mnt, uint8_t *record, uint64_t parent_reference, const uint16_t *name, uint8_t name_length)
{
    uint32_t bytes_in_use;
    uint32_t offset;

    if (!mnt || !record || !name || !name_length || load_le32(record) != MFT_MAGIC) return -EINVAL;
    bytes_in_use = load_le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size) return -EIO;
    offset = load_le16(record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        if (type == AT_FILE_NAME && !attribute->non_resident && !attribute->name_length) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < sizeof(fname_attr_t) || value_length > length - value_offset) return -EIO;
            fname_attr_t *file_name = (fname_attr_t *)((uint8_t *)attribute + value_offset);
            if ((load_le64((uint8_t *)&file_name->parent_dir) & 0x0000ffffffffffffULL) == (parent_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length) {
                int equal = 1;
                for (uint32_t i = 0; i < name_length; i++)
                    if (load_le16((uint8_t *)file_name->name + (i * sizeof(uint16_t))) != name[i]) equal = 0;
                if (equal) {
                    memmove(record + offset, record + offset + length, bytes_in_use - offset - length);
                    memset(record + bytes_in_use - length, 0, length);
                    store_le32(record + 0x18, bytes_in_use - length);
                    return 0;
                }
            }
        }
        offset += length;
    }
    return -ENOENT;
}

/* Locate the unnamed $Bitmap attribute of MFT record 0. */
static int ntfs_mft_bitmap_attribute(ntfs_mount_t *mnt, uint8_t *record, attr_rec_t **result)
{
    uint32_t offset;
    uint32_t bytes_in_use;

    if (ntfs_mft_read(mnt, 0, record) < 0 || load_le32(record) != MFT_MAGIC) return -EIO;
    bytes_in_use = load_le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size) return -EIO;
    offset = load_le16(record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        if (type == AT_BITMAP && !attribute->name_length) {
            *result = attribute;
            return 0;
        }
        offset += length;
    }
    return -EIO;
}

/* Read one byte of the $MFT bitmap attribute. */
static int ntfs_mft_bitmap_read_byte(ntfs_mount_t *mnt, attr_rec_t *attribute, uint64_t byte_offset, uint8_t *value)
{
    uint32_t length = load_le32((uint8_t *)&attribute->length);

    if (attribute->non_resident) {
        uint16_t mapping_offset = load_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off);
        uint64_t data_size      = load_le64((uint8_t *)&attribute->d.nres.data_size);
        if (length < 64 || mapping_offset < 64 || mapping_offset >= length || byte_offset >= data_size) return -EIO;
        return ntfs_read_by_runlist(mnt, (uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), byte_offset, value, 1, data_size) == 1 ? 0 : -EIO;
    }
    uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
    uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
    if (value_offset > length || value_length > length - value_offset || byte_offset >= value_length) return -EIO;
    *value = *((uint8_t *)attribute + value_offset + byte_offset);
    return 0;
}

/* Find the first free record number in the $MFT bitmap. */
int ntfs_mft_bitmap_find_free(ntfs_mount_t *mnt, uint64_t *record_number)
{
    uint8_t    *record;
    attr_rec_t *attribute;
    uint64_t    records;
    int         status;

    if (!mnt || !record_number || !mnt->mft_size) return -EINVAL;
    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    status = ntfs_mft_bitmap_attribute(mnt, record, &attribute);
    if (status < 0) goto out;
    records = mnt->mft_data_size / mnt->mft_size;
    for (uint64_t number = 16; number < records; number += 8) {
        uint8_t bitmap;
        status = ntfs_mft_bitmap_read_byte(mnt, attribute, number / 8, &bitmap);
        if (status < 0) goto out;
        for (uint32_t bit = 0; bit < 8 && number + bit < records; bit++) {
            if (!(bitmap & (1U << bit))) {
                *record_number = number + bit;
                status         = 0;
                goto out;
            }
        }
    }
    status = -ENOSPC;
out:
    free(record);
    return status;
}

/* Mark a record allocated or free in the $MFT bitmap. */
int ntfs_mft_bitmap_set(ntfs_mount_t *mnt, uint64_t record_number, int allocated)
{
    uint8_t    *record;
    attr_rec_t *attribute;
    uint8_t     bitmap;
    int         status;

    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    status = ntfs_mft_bitmap_attribute(mnt, record, &attribute);
    if (status < 0) goto out;
    status = ntfs_mft_bitmap_read_byte(mnt, attribute, record_number / 8, &bitmap);
    if (status < 0) goto out;
    if (allocated) {
        bitmap |= (uint8_t)(1U << (record_number & 7));
    } else {
        bitmap &= (uint8_t)~(1U << (record_number & 7));
    }
    if (attribute->non_resident) {
        uint32_t length         = load_le32((uint8_t *)&attribute->length);
        uint16_t mapping_offset = load_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off);
        uint64_t data_size      = load_le64((uint8_t *)&attribute->d.nres.data_size);
        status                  = ntfs_write_by_runlist(mnt, (uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), record_number / 8, &bitmap, 1, data_size) == 1 ? 0 : -EIO;
    } else {
        uint16_t value_offset                                        = load_le16((uint8_t *)&attribute->d.res.value_offset);
        *((uint8_t *)attribute + value_offset + (record_number / 8)) = bitmap;
        status                                                       = ntfs_mft_write(mnt, 0, record);
    }
out:
    free(record);
    return status;
}

/* Write a resident attribute at an offset, returning the next offset. */
uint32_t ntfs_resident_attribute(uint8_t *record, uint32_t offset, uint32_t type, uint16_t instance, const uint16_t *attribute_name, uint8_t attribute_name_length, const uint8_t *value,
                                 uint32_t value_length)
{
    uint16_t value_offset = (uint16_t)ntfs_align8(24 + ((uint32_t)attribute_name_length * sizeof(uint16_t)));
    uint32_t length       = ntfs_align8(value_offset + value_length);

    memset(record + offset, 0, length);
    store_le32(record + offset, type);
    store_le32(record + offset + 4, length);
    record[offset + 9] = attribute_name_length;
    store_le16(record + offset + 10, 24);
    store_le16(record + offset + 14, instance);
    store_le32(record + offset + 16, value_length);
    store_le16(record + offset + 20, value_offset);
    for (uint32_t i = 0; i < attribute_name_length; i++) store_le16(record + offset + 24 + (i * sizeof(uint16_t)), attribute_name[i]);
    if (value_length) memcpy(record + offset + value_offset, value, value_length);
    return offset + length;
}

/* Build a new MFT record for a file or directory. */
int ntfs_build_file_record(ntfs_mount_t *mnt, uint8_t *record, uint64_t record_number, uint16_t sequence, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, int directory)
{
    uint8_t  standard[48] = {0};
    uint8_t  file_name_value[sizeof(fname_attr_t) + (255 * sizeof(uint16_t))];
    uint8_t  index_root[48] = {0};
    uint32_t offset;
    uint32_t attributes = directory ? 0x10 : 0x20;

    if (!mnt || !record || !name || !name_length) return -EINVAL;
    memset(record, 0, mnt->mft_size);
    store_le32(record, MFT_MAGIC);
    store_le16(record + 4, 0x30);
    store_le16(record + 6, (uint16_t)((mnt->mft_size / mnt->sector_size) + 1));
    store_le16(record + 0x10, sequence ? sequence : 1);
    store_le16(record + 0x12, 1);
    store_le16(record + 0x14, 0x38);
    store_le16(record + 0x16, directory ? 3 : 1);
    store_le32(record + 0x1c, mnt->mft_size);
    store_le16(record + 0x28, directory ? 3 : 4);
    store_le32(record + 0x2c, (uint32_t)record_number);
    uint64_t now = ntfs_current_filetime();
    store_le64(standard, now);
    store_le64(standard + 8, now);
    store_le64(standard + 16, now);
    store_le64(standard + 24, now);
    store_le32(standard + 32, attributes);

    offset = ntfs_resident_attribute(record, 0x38, AT_STANDARD_INFORMATION, 0, NULL, 0, standard, sizeof(standard));
    memset(file_name_value, 0, sizeof(fname_attr_t) + ((uint32_t)name_length * sizeof(uint16_t)));
    fname_attr_t *file_name = (fname_attr_t *)file_name_value;
    store_le64((uint8_t *)&file_name->parent_dir, parent_reference);
    store_le64((uint8_t *)&file_name->crtime, now);
    store_le64((uint8_t *)&file_name->mtime_data, now);
    store_le64((uint8_t *)&file_name->mtime_mft, now);
    store_le64((uint8_t *)&file_name->atime, now);
    store_le32((uint8_t *)&file_name->fa, attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (uint32_t i = 0; i < name_length; i++) store_le16((uint8_t *)file_name->name + (i * sizeof(uint16_t)), name[i]);
    offset = ntfs_resident_attribute(record, offset, AT_FILE_NAME, 1, NULL, 0, file_name_value, sizeof(fname_attr_t) + ((uint32_t)name_length * sizeof(uint16_t)));
    if (directory) {
        store_le32(index_root, AT_FILE_NAME);
        store_le32(index_root + 4, 1);
        store_le32(index_root + 8, mnt->indx_size);
        index_root[12] = 1;
        store_le32(index_root + 16, 0x10);
        store_le32(index_root + 20, 0x20);
        store_le32(index_root + 24, 0x20);
        store_le16(index_root + 32 + 8, 16);
        store_le16(index_root + 32 + 12, INDEX_ENTRY_END);
        offset = ntfs_resident_attribute(record, offset, AT_INDEX_ROOT, 2, ntfs_i30, 4, index_root, sizeof(index_root));
    } else {
        offset = ntfs_resident_attribute(record, offset, AT_DATA, 2, NULL, 0, NULL, 0);
    }
    if (offset > mnt->mft_size - 8) return -ENOSPC;
    store_le32(record + offset, AT_END);
    store_le32(record + 0x18, offset + 8);
    return 0;
}

/* Build a symlink MFT record carrying a reparse-point attribute. */
int ntfs_build_symlink_record(ntfs_mount_t *mnt, uint8_t *record, uint64_t record_number, uint16_t sequence, uint64_t parent_reference, const uint16_t *name, uint8_t name_length,
                              const uint16_t *target, uint8_t target_length, int relative)
{
    uint8_t  reparse[20 + (255 * 4)];
    uint32_t reparse_size = 20 + ((uint32_t)target_length * 4);
    uint32_t offset;
    int      status;

    status = ntfs_build_file_record(mnt, record, record_number, sequence, parent_reference, name, name_length, 0);
    if (status < 0) return status;
    offset = load_le16(record + 0x14);
    while (offset + 24 <= load_le32(record + 0x18)) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > load_le32(record + 0x18) - length) return -EIO;
        if (!attribute->non_resident && !attribute->name_length && (type == AT_STANDARD_INFORMATION || type == AT_FILE_NAME)) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            if (value_offset > length || value_length > length - value_offset) return -EIO;
            if (type == AT_STANDARD_INFORMATION) {
                if (value_length < 36) return -EIO;
                store_le32((uint8_t *)attribute + value_offset + 32, load_le32((uint8_t *)attribute + value_offset + 32) | FILE_ATTRIBUTE_REPARSE_POINT);
            } else {
                if (value_length < sizeof(fname_attr_t)) return -EIO;
                fname_attr_t *file_name = (fname_attr_t *)((uint8_t *)attribute + value_offset);
                store_le32((uint8_t *)&file_name->fa, load_le32((uint8_t *)&file_name->fa) | FILE_ATTRIBUTE_REPARSE_POINT);
                store_le32((uint8_t *)&file_name->rp_tag, IO_REPARSE_TAG_SYMLINK);
            }
        }
        offset += length;
    }
    if (offset + 8 > load_le32(record + 0x18) || load_le32(record + offset) != AT_END) return -EIO;
    memset(reparse, 0, reparse_size);
    store_le32(reparse, IO_REPARSE_TAG_SYMLINK);
    store_le16(reparse + 4, (uint16_t)(12 + ((uint32_t)target_length * 4)));
    store_le16(reparse + 10, (uint16_t)((uint32_t)target_length * 2));
    store_le16(reparse + 12, (uint16_t)((uint32_t)target_length * 2));
    store_le16(reparse + 14, (uint16_t)((uint32_t)target_length * 2));
    store_le32(reparse + 16, relative ? 1 : 0);
    for (uint32_t i = 0; i < target_length; i++) {
        store_le16(reparse + 20 + ((size_t)i * 2), target[i]);
        store_le16(reparse + 20 + ((size_t)((uint32_t)target_length + i) * 2), target[i]);
    }
    uint32_t attribute_length = ntfs_align8(24 + reparse_size);
    if (attribute_length > mnt->mft_size - offset - 8) return -ENOSPC;
    uint16_t instance = load_le16(record + 0x28);
    uint32_t end      = ntfs_resident_attribute(record, offset, AT_REPARSE_POINT, instance, NULL, 0, reparse, reparse_size);
    if (end > mnt->mft_size - 8) return -ENOSPC;
    store_le32(record + end, AT_END);
    store_le32(record + 0x18, end + 8);
    store_le16(record + 0x28, instance + 1);
    return 0;
}

/* Parse the $MFT data runlist from the raw MFT record at its boot LCN. */
int ntfs_mft_bootstrap_runlist(ntfs_mount_t *mnt)
{
    uint8_t *record;
    uint32_t offset;
    int      status = -EIO;

    if (!mnt || mnt->mft_runlist || !mnt->mft_size || mnt->mft_lcn < 0) return mnt && mnt->mft_runlist ? 0 : -EINVAL;
    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (ntfs_dev_read(mnt, (uint64_t)mnt->mft_lcn << mnt->cluster_bits, record, mnt->mft_size) < 0 || ntfs_record_unpack(mnt, record, mnt->mft_size) < 0 || load_le32(record) != MFT_MAGIC) goto out;

    offset = load_le16(record + 0x14);
    while (offset + 16 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);

        if (type == AT_END) break;
        if (length < 16 || offset + length > mnt->mft_size) goto out;
        if (type == AT_ATTRIBUTE_LIST) goto out;
        if (type == AT_DATA && !attribute->name_length) {
            uint16_t mapping_offset;
            uint16_t flags;
            uint32_t runlist_size;
            uint8_t *runlist;
            int64_t  vcn[256], lcn[256], run[256];
            int      run_count;
            uint64_t data_size;

            if (!attribute->non_resident || length < 64 || load_le64((uint8_t *)&attribute->d.nres.lowest_vcn) != 0) goto out;
            flags = load_le16((uint8_t *)&attribute->flags);
            if (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) goto out;
            mapping_offset = load_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off);
            if (mapping_offset < 64 || mapping_offset >= length) goto out;
            runlist_size = length - mapping_offset;
            runlist      = malloc(runlist_size);
            if (!runlist) {
                status = -ENOMEM;
                goto out;
            }
            memcpy(runlist, (uint8_t *)attribute + mapping_offset, runlist_size);
            run_count = ntfs_runlist_parse(runlist, (int)runlist_size, vcn, lcn, run, 256);
            data_size = load_le64((uint8_t *)&attribute->d.nres.data_size);
            if (run_count <= 0 || data_size < mnt->mft_size || (uint64_t)run[run_count - 1] + (uint64_t)vcn[run_count - 1] < (data_size + mnt->cluster_size - 1) / mnt->cluster_size) {
                free(runlist);
                goto out;
            }
            mnt->mft_runlist      = runlist;
            mnt->mft_runlist_size = runlist_size;
            mnt->mft_data_size    = data_size;
            status                = 0;
            goto out;
        }
        offset += length;
    }
out:
    if (status != EOK && status != -ENOMEM) plogk("ntfs: Drive %u: MFT data attribute parse failed.\n", mnt->dev.drive);
    free(record);
    return status;
}

/* Read and unpack one MFT record. */
int ntfs_mft_read(ntfs_mount_t *mnt, uint64_t mft_no, uint8_t *buf)
{
    uint64_t logical_offset;

    if (!mnt || !buf || !mnt->cluster_size || mft_no > UINT64_MAX / mnt->mft_size) return -EIO;
    if (mft_no && !mnt->mft_runlist && ntfs_mft_bootstrap_runlist(mnt) < 0) return -EIO;
    logical_offset = mft_no * mnt->mft_size;
    if (mnt->mft_runlist) {
        if (ntfs_read_by_runlist(mnt, mnt->mft_runlist, (int)mnt->mft_runlist_size, logical_offset, buf, mnt->mft_size, mnt->mft_data_size) != (int)mnt->mft_size) return -EIO;
    } else {
        uint64_t byte_offset = ((uint64_t)mnt->mft_lcn << mnt->cluster_bits) + logical_offset;
        if (ntfs_dev_read(mnt, byte_offset, buf, mnt->mft_size) < 0) {
            plogk("ntfs: Drive %u: failed to read MFT record %llu at byte %llu\n", mnt->dev.drive, mft_no, byte_offset);
            return -EIO;
        }
    }
    return ntfs_record_unpack(mnt, buf, mnt->mft_size);
}

/* Pack and write one MFT record, mirroring the first records when required. */
int ntfs_mft_write(ntfs_mount_t *mnt, uint64_t mft_no, const uint8_t *buf)
{
    uint64_t byte_off;
    uint8_t *record;
    int      status;

    if (!mnt || !buf || !mnt->cluster_size || mnt->dev.read_only) return -EROFS;
    if (!mnt->mft_runlist && ntfs_mft_bootstrap_runlist(mnt) < 0) return -EIO;
    if (mft_no > UINT64_MAX / mnt->mft_size) return -EOVERFLOW;
    byte_off = mft_no * mnt->mft_size;

    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    memcpy(record, buf, mnt->mft_size);
    status = ntfs_record_pack(mnt, record, mnt->mft_size);
    if (status == 0) {
        if (mnt->mft_runlist) {
            if (ntfs_write_by_runlist(mnt, mnt->mft_runlist, (int)mnt->mft_runlist_size, byte_off, record, mnt->mft_size, mnt->mft_data_size) != (int64_t)mnt->mft_size) status = -EIO;
        } else {
            byte_off += (uint64_t)mnt->mft_lcn << mnt->cluster_bits;
            if (ntfs_dev_write(mnt, byte_off, record, mnt->mft_size) < 0) status = -EIO;
        }
        if (status == 0 && mnt->mftmirr_lcn > 0) {
            uint64_t mirror_records = mnt->cluster_size / mnt->mft_size;
            if (mirror_records < 4) mirror_records = 4;
            if (mft_no < mirror_records && ntfs_dev_write(mnt, ((uint64_t)mnt->mftmirr_lcn << mnt->cluster_bits) + (mft_no * mnt->mft_size), record, mnt->mft_size) < 0) status = -EIO;
        }
    }
    if (status != EOK) plogk("ntfs: Drive %u: MFT record %llu write failed: %d\n", mnt->dev.drive, mft_no, status);
    free(record);
    return status;
}

/* Remove an attribute from a record. */
int ntfs_record_remove_attribute(uint8_t *record, uint32_t record_size, uint32_t offset)
{
    uint32_t used;
    uint32_t length;

    if (!record || offset + 8 > record_size) return -EINVAL;
    used   = load_le32(record + 0x18);
    length = load_le32(record + offset + 4);
    if (used > record_size || length < 24 || offset > used - length) return -EIO;
    memmove(record + offset, record + offset + length, used - offset - length);
    memset(record + used - length, 0, length);
    store_le32(record + 0x18, used - length);
    return 0;
}

#endif
