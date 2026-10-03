/*
 *
 *      ntfs_super.c
 *      New Technology File System - volume bootstrap and transactions
 *
 *      2026/7/26 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/fs_txn.h>
#include <fs/ntfs/ntfs.h>
#include <kernel/errno.h>
#include <libs/std/string.h>
#include <libs/util/byteorder.h>
#include <mem/heap.h>

#if CONFIG_NTFS_FS

/* Load the volume bitmap runlist from $Bitmap (record 6). */
static int ntfs_load_bitmap_runlist(ntfs_mount_t *mnt)
{
    uint8_t *record;
    uint32_t offset;
    int      status = -EIO;

    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (ntfs_mft_read(mnt, 6, record) < 0 || load_le32(record) != MFT_MAGIC) goto out;

    offset = load_le16(record + 0x14);
    while (offset + 16 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);

        if (type == AT_END) break;
        if (length < 16 || offset + length > mnt->mft_size || type == AT_ATTRIBUTE_LIST) goto out;
        if (type == AT_DATA && !attribute->name_length) {
            uint16_t mapping_offset;
            uint16_t flags;
            uint64_t data_size;
            uint8_t *runlist;
            int64_t  vcn[256], lcn[256], run[256];

            if (!attribute->non_resident || length < 64) goto out;
            flags = load_le16((uint8_t *)&attribute->flags);
            if (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) goto out;
            mapping_offset = load_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off);
            data_size      = load_le64((uint8_t *)&attribute->d.nres.data_size);
            if (mapping_offset < 64 || mapping_offset >= length || data_size < ((uint64_t)mnt->nr_clusters + 7) / 8) goto out;
            runlist = malloc(length - mapping_offset);
            if (!runlist) {
                status = -ENOMEM;
                goto out;
            }
            memcpy(runlist, (uint8_t *)attribute + mapping_offset, length - mapping_offset);
            if (ntfs_runlist_parse(runlist, (int)(length - mapping_offset), vcn, lcn, run, 256) <= 0) {
                free(runlist);
                goto out;
            }
            mnt->bitmap_runlist      = runlist;
            mnt->bitmap_runlist_size = length - mapping_offset;
            mnt->bitmap_data_size    = data_size;
            status                   = 0;
            goto out;
        }
        offset += length;
    }
out:
    free(record);
    return status;
}

/* Load the Unicode upcase table from $UpCase (record 10). */
static int ntfs_load_upcase(ntfs_mount_t *mnt)
{
    uint8_t *record;
    uint8_t *raw = NULL;
    uint32_t offset;
    int      status = -EIO;

    if (!mnt) return -EINVAL;
    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (ntfs_mft_read(mnt, 10, record) < 0 || load_le32(record) != MFT_MAGIC) goto out;
    offset = load_le16(record + 0x14);
    while (offset + 24 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > mnt->mft_size - length || type == AT_ATTRIBUTE_LIST) goto out;
        if (type == AT_DATA && !attribute->name_length) {
            const uint32_t table_size = 65536 * sizeof(uint16_t);
            raw                       = malloc(table_size);
            if (!raw) {
                status = -ENOMEM;
                goto out;
            }
            if (attribute->non_resident) {
                uint16_t mapping_offset = load_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off);
                uint64_t data_size      = load_le64((uint8_t *)&attribute->d.nres.data_size);
                uint16_t flags          = load_le16((uint8_t *)&attribute->flags);
                if (length < 64 || mapping_offset < 64 || mapping_offset >= length || data_size != table_size || (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE))
                    || ntfs_read_by_runlist(mnt, (uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), 0, raw, table_size, data_size) != (int)table_size) {
                    goto out;
                }
            } else {
                uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
                uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
                if (value_offset > length || value_length != table_size || value_length > length - value_offset) goto out;
                memcpy(raw, (uint8_t *)attribute + value_offset, table_size);
            }
            mnt->upcase = malloc(table_size);
            if (!mnt->upcase) {
                status = -ENOMEM;
                goto out;
            }
            for (uint32_t i = 0; i < 65536; i++) mnt->upcase[i] = load_le16(raw + (i * sizeof(uint16_t)));
            mnt->upcase_length = 65536;
            status             = 0;
            goto out;
        }
        offset += length;
    }
out:
    free(raw);
    free(record);
    return status;
}

/* Enable writes by validating the volume information and loading write data. */
int ntfs_prepare_write(ntfs_mount_t *mnt)
{
    uint8_t *record;
    uint32_t offset;
    int      status = -EROFS;

    if (!mnt || mnt->dev.read_only || ntfs_mft_bootstrap_runlist(mnt) < 0) return -EROFS;
    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (ntfs_mft_read(mnt, 3, record) < 0 || load_le32(record) != MFT_MAGIC) goto out;

    offset = load_le16(record + 0x14);
    while (offset + 24 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || offset + length > mnt->mft_size || type == AT_ATTRIBUTE_LIST) goto out;
        if (type == AT_VOLUME_INFORMATION && !attribute->name_length && !attribute->non_resident) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            uint8_t *value;

            if (value_offset > length || value_length < 12 || value_length > length - value_offset) goto out;
            value = (uint8_t *)attribute + value_offset;
            if (value[8] != 3 || load_le16(value + 10) != 0) goto out;
            status = ntfs_load_bitmap_runlist(mnt);
            if (status == 0) status = ntfs_load_upcase(mnt);
            if (status == 0) mnt->write_enabled = 1;
            goto out;
        }
        offset += length;
    }
out:
    free(record);
    return status;
}

/* Set or clear the volume's dirty flag in $Volume. */
int ntfs_set_volume_dirty(ntfs_mount_t *mnt, int dirty)
{
    uint8_t *record;
    uint32_t offset;
    int      status = -EIO;

    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (ntfs_mft_read(mnt, 3, record) < 0 || load_le32(record) != MFT_MAGIC) goto out;
    offset = load_le16(record + 0x14);
    while (offset + 24 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || offset + length > mnt->mft_size) goto out;
        if (type == AT_VOLUME_INFORMATION && !attribute->name_length && !attribute->non_resident) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            uint16_t volume_flags;

            if (value_offset > length || value_length < 12 || value_length > length - value_offset) goto out;
            volume_flags = load_le16((uint8_t *)attribute + value_offset + 10);
            if (dirty) {
                volume_flags |= 1;
            } else {
                volume_flags &= (uint16_t)~1U;
            }
            store_le16((uint8_t *)attribute + value_offset + 10, volume_flags);
            status = ntfs_mft_write(mnt, 3, record);
            if (status == EOK) status = blockdev_flush(&mnt->dev);
            goto out;
        }
        offset += length;
    }
out:
    free(record);
    return status;
}

/* Clear the dirty flag previously set by this mount. */
int ntfs_clear_owned_dirty(ntfs_mount_t *mnt)
{
    int status;

    if (!mnt || !mnt->dirty_owned) return EOK;
    status = blockdev_flush(&mnt->dev);
    if (status == EOK) status = ntfs_set_volume_dirty(mnt, 0);
    if (status == EOK) {
        mnt->dirty_owned = 0;
    } else {
        mnt->write_enabled = 0;
    }
    return status;
}

/* Begin a transaction, setting the dirty flag first if needed. */
int ntfs_transaction_begin(ntfs_mount_t *mnt, fs_txn_t *transaction, uint32_t credits)
{
    int  status;
    bool busy;

    if (!mnt || !transaction || !credits || !mnt->transaction_log_initialized) return -EINVAL;

    /*
     * Read the active pointer under the log lock (the publish here and the
     * clear in finish happen under the same lock, so the check is
     * data-race-free).  Mutual exclusion between concurrent begins is
     * provided by fs_txn_begin()->fs_txn_claim_log(); the check is a
     * fast-path rejection.
     */
    fs_txn_log_lock(&mnt->transaction_log);
    busy = mnt->active_transaction != NULL;
    fs_txn_log_unlock(&mnt->transaction_log);
    if (busy) return -EINVAL;

    status = fs_txn_begin(&mnt->transaction_log, credits, transaction);
    if (status == EOK) {
        /* Publish under the log lock so ntfs_dev_read/ntfs_dev_write see a consistent snapshot and can never read a pointer that is being freed. */
        fs_txn_log_lock(&mnt->transaction_log);
        mnt->active_transaction = transaction;
        fs_txn_log_unlock(&mnt->transaction_log);
    } else if (mnt->dirty_owned) {
        (void)ntfs_clear_owned_dirty(mnt);
    }
    return status;
}

/* Commit or abort the active transaction and clear the dirty flag. */
int ntfs_transaction_finish(ntfs_mount_t *mnt, fs_txn_t *transaction, int status)
{
    if (!mnt || !transaction || mnt->active_transaction != transaction) return -EINVAL;

    /*
     * Hold the log lock for the whole finish and only detach
     * active_transaction after commit/abort completes.  fs_txn_read_active()/
     * fs_txn_stage_active() read *active_pp under the same lock, so a
     * concurrent reader/writer either stages through the transaction or falls
     * back to the device after the pointer clear - never bypassing the device
     * while commit is still flushing ordered data/metadata/checkpoint.
     */
    fs_txn_log_lock(&mnt->transaction_log);
    if (status != EOK) {
        fs_txn_abort(transaction, status);
        mnt->active_transaction = NULL;
        fs_txn_log_unlock(&mnt->transaction_log);
        (void)ntfs_clear_owned_dirty(mnt);
        return status;
    }
    status                  = fs_txn_commit(transaction);
    mnt->active_transaction = NULL;
    fs_txn_log_unlock(&mnt->transaction_log);
    if (status == EOK) {
        status = ntfs_clear_owned_dirty(mnt);
    } else {
        /* A failed home-write may have left partially updated metadata. */
        mnt->write_enabled = 0;
    }
    return status;
}

#endif
