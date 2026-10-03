/*
 *
 *      ntfs_runlist.c
 *      New Technology File System - runlists and cluster allocation
 *
 *      2026/7/26 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/fs_txn.h>
#include <fs/ntfs/ntfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <libs/util/byteorder.h>
#include <mem/heap.h>

#if CONFIG_NTFS_FS

/* Device read, honoring the active transaction. */
int ntfs_dev_read(ntfs_mount_t *mnt, uint64_t byte_off, uint8_t *buf, size_t size)
{
    if (!mnt || !buf || byte_off > UINT64_MAX - size) return -EINVAL;
    int served = fs_txn_read_bytes_active(&mnt->transaction_log, &mnt->active_transaction, byte_off, buf, size);
    if (served > 0) return EOK;
    if (served < 0) return served;
    int status = blockdev_read_bytes(&mnt->dev, byte_off, buf, size);
    if (status != EOK) fs_txn_log_abort(&mnt->transaction_log, status);
    return status;
}

/* Device write, staged through the active transaction. */
int ntfs_dev_write(ntfs_mount_t *mnt, uint64_t byte_off, const uint8_t *buf, size_t size)
{
    if (!mnt || !buf || mnt->dev.read_only || byte_off > UINT64_MAX - size) return mnt && mnt->dev.read_only ? -EROFS : -EINVAL;
    int served = fs_txn_stage_bytes_active(&mnt->transaction_log, &mnt->active_transaction, byte_off, buf, size, FS_TXN_METADATA);
    if (served > 0) return EOK;
    if (served < 0) return served;
    int status = blockdev_write_bytes(&mnt->dev, byte_off, buf, size);
    if (status != EOK) fs_txn_log_abort(&mnt->transaction_log, status);
    return status;
}

/* Decode a runlist into vcn/lcn/run triples, handling sparse runs. */
int ntfs_runlist_parse(uint8_t *rl, int len, int64_t *vcn, int64_t *lcn, int64_t *run, int max)
{
    int           off = 0, cnt = 0;
    int64_t       cur_vcn = 0, cur_lcn = 0;
    const int64_t s64_max = (int64_t)(UINT64_MAX >> 1);
    const int64_t s64_min = -s64_max - 1;

    if (!rl || len <= 0 || !vcn || !lcn || !run || max <= 0) return -EINVAL;
    while (off < len && cnt < max) {
        if (rl[off] == 0) return cnt;
        uint8_t h  = rl[off++];
        int     lb = h & 0x0F, ob = (h >> 4) & 0x0F;
        if (lb == 0 || lb > 8 || ob > 8 || off + lb + ob > len) return -EIO;

        uint64_t raw_length = 0;
        for (int i = 0; i < lb; i++) raw_length |= (uint64_t)rl[off++] << (i * 8);
        if (!raw_length || raw_length > (uint64_t)s64_max || cur_vcn > s64_max - (int64_t)raw_length) return -EIO;
        int64_t rlen = (int64_t)raw_length;

        if (ob == 0) {
            /* sparse / hole run: no LCN delta */
            vcn[cnt] = cur_vcn;
            lcn[cnt] = LCN_HOLE;
            run[cnt] = rlen;
        } else {
            uint64_t raw_offset = 0;
            for (int i = 0; i < ob; i++) raw_offset |= (uint64_t)rl[off++] << (i * 8);
            int64_t roff = (int64_t)raw_offset;
            if (ob < 8 && (rl[off - 1] & 0x80)) {
                uint64_t mask = (1ULL << (ob * 8)) - 1;
                roff          = (int64_t)(raw_offset | ~mask);
            }
            if ((roff > 0 && cur_lcn > s64_max - roff) || (roff < 0 && cur_lcn < s64_min - roff)) return -EIO;
            cur_lcn += roff;
            if (cur_lcn < 0) return -EIO;
            vcn[cnt] = cur_vcn;
            lcn[cnt] = cur_lcn;
            run[cnt] = rlen;
        }
        cur_vcn += rlen;
        cnt++;
    }
    return -EIO;
}

/* Return the byte width of a signed runlist value. */
static int runlist_signed_bytes(int64_t value)
{
    for (int bytes = 1; bytes < 8; bytes++) {
        int64_t limit = (int64_t)1 << (bytes * 8 - 1);
        if (value >= -limit && value < limit) return bytes;
    }
    return 8;
}

/* Return the byte width of an unsigned runlist value. */
static int runlist_unsigned_bytes(uint64_t value)
{
    int bytes = 1;
    while (bytes < 8 && value >= (1ULL << (bytes * 8))) bytes++;
    return bytes;
}

/* Encode lcn/run triples back into runlist byte form. */
int ntfs_runlist_encode(const int64_t *lcn, const int64_t *run, int count, uint8_t *out, uint32_t capacity)
{
    int64_t  previous_lcn = 0;
    uint32_t offset       = 0;

    if (!lcn || !run || count <= 0 || !out) return -EINVAL;
    for (int i = 0; i < count; i++) {
        int64_t delta;
        int     length_bytes;
        int     offset_bytes;

        if (lcn[i] < 0 || run[i] <= 0) return -EINVAL;
        delta        = lcn[i] - previous_lcn;
        length_bytes = runlist_unsigned_bytes((uint64_t)run[i]);
        offset_bytes = runlist_signed_bytes(delta);
        if (capacity - offset < (uint32_t)(1 + length_bytes + offset_bytes)) return -ENOSPC;
        out[offset++] = (uint8_t)((offset_bytes << 4) | length_bytes);
        for (int byte = 0; byte < length_bytes; byte++) out[offset++] = (uint8_t)((uint64_t)run[i] >> (byte * 8));
        for (int byte = 0; byte < offset_bytes; byte++) out[offset++] = (uint8_t)((uint64_t)delta >> (byte * 8));
        previous_lcn = lcn[i];
    }
    if (offset >= capacity) return -ENOSPC;
    out[offset++] = 0;
    return (int)offset;
}

/* Read a byte range through a runlist, zero-filling sparse holes. */
int ntfs_read_by_runlist(ntfs_mount_t *mnt, uint8_t *rl, int rl_len, uint64_t offset, uint8_t *buf, size_t size, uint64_t max_size)
{
    int64_t v[256], l[256], r[256];
    int     n = ntfs_runlist_parse(rl, rl_len, v, l, r, 256);
    if (n <= 0) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ntfs: Drive %u: corrupt runlist in read path.\n", mnt->dev.drive);
        return 0;
    }
    if (!size || offset >= max_size) return 0;

    size_t done = 0;
    while (done < size && offset + done < max_size) {
        int64_t pos       = (int64_t)(offset + done);
        int64_t clu       = pos >> mnt->cluster_bits;
        int64_t found_lcn = -1, hole_end = 0;
        int     found = 0;
        for (int i = 0; i < n; i++) {
            if (clu >= v[i] && clu < v[i] + r[i]) {
                if (l[i] >= 0) {
                    found_lcn = l[i] + (clu - v[i]);
                } else {
                    hole_end = (v[i] + r[i]) << mnt->cluster_bits;
                }
                break;
            }
            if (clu < v[i] && !found) {
                hole_end = v[i] << mnt->cluster_bits;
                found    = 1;
            }
        }
        if (found_lcn < 0) {
            int64_t h_end   = hole_end ? hole_end : (int64_t)max_size;
            int64_t remain  = (int64_t)(size - done);
            int64_t to_fill = h_end - pos;
            if (to_fill > remain) to_fill = remain;
            if (to_fill <= 0) break;
            if (pos + to_fill > (int64_t)max_size) to_fill = (int64_t)max_size - pos;
            size_t fill = (size_t)to_fill;
            memset(buf + done, 0, fill);
            done += fill;
        } else {
            int64_t cl_off  = pos & mnt->cluster_mask;
            int64_t to_read = (int64_t)(size - done);
            int64_t max_cl  = (int64_t)mnt->cluster_size - cl_off;
            if (to_read > max_cl) to_read = max_cl;
            int64_t byte_off = (found_lcn << mnt->cluster_bits) + cl_off;
            if (ntfs_dev_read(mnt, (uint64_t)byte_off, buf + done, (size_t)to_read) < 0) {
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("ntfs: Drive %u: block read failed at byte %llu (%zu bytes)\n", mnt->dev.drive, byte_off, (size - done));
                return done > 0 ? (int)done : -EIO;
            }
            done += (size_t)to_read;
        }
    }
    return (int)done;
}

/* Write a byte range through a runlist, bounding every LCN to the volume. */
int64_t ntfs_write_by_runlist(ntfs_mount_t *mnt, uint8_t *rl, int rl_len, uint64_t offset, const uint8_t *buf, size_t size, uint64_t max_size)
{
    int64_t v[256], l[256], r[256];
    int     n;
    size_t  done = 0;

    if (!mnt || !buf || !size || offset > max_size || size > max_size - offset) return -EINVAL;
    n = ntfs_runlist_parse(rl, rl_len, v, l, r, 256);
    if (n <= 0) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ntfs: Drive %u: corrupt runlist in write path.\n", mnt->dev.drive);
        return -EIO;
    }

    while (done < size) {
        uint64_t position = offset + done;
        int64_t  cluster  = (int64_t)(position >> mnt->cluster_bits);
        int64_t  run_lcn  = -1;
        int64_t  run_end  = -1;

        for (int i = 0; i < n; i++) {
            if (cluster >= v[i] && cluster < v[i] + r[i]) {
                if (l[i] >= 0) {
                    run_lcn = l[i] + cluster - v[i];
                    run_end = (v[i] + r[i]) << mnt->cluster_bits;
                }
                break;
            }
        }
        if (run_lcn < 0 || run_lcn >= mnt->nr_clusters) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ntfs: Drive %u: write target LCN %lld out of range (nr_clusters=%llu)\n", mnt->dev.drive, run_lcn, mnt->nr_clusters);
            return done ? (int64_t)done : -EIO;
        }

        size_t cluster_offset = (size_t)(position & mnt->cluster_mask);
        size_t chunk          = (size_t)(run_end - (int64_t)position);
        if (chunk > size - done) chunk = size - done;
        if ((uint64_t)run_lcn + (cluster_offset + chunk + mnt->cluster_size - 1) / mnt->cluster_size > (uint64_t)mnt->nr_clusters) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ntfs: Drive %u: write overruns volume end at LCN %lld\n", mnt->dev.drive, run_lcn);
            return done ? (int64_t)done : -EIO;
        }
        if (ntfs_dev_write(mnt, ((uint64_t)run_lcn << mnt->cluster_bits) + cluster_offset, buf + done, chunk) < 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ntfs: Drive %u: block write failed at LCN %lld\n", mnt->dev.drive, run_lcn);
            return done ? (int64_t)done : -EIO;
        }
        done += chunk;
    }
    return (int64_t)done;
}

/* Zero a byte range through a runlist. */
int ntfs_zero_by_runlist(ntfs_mount_t *mnt, uint8_t *rl, int rl_len, uint64_t offset, uint64_t size, uint64_t max_size)
{
    uint8_t *zeros;

    if (!size) return 0;
    zeros = calloc(1, mnt->cluster_size);
    if (!zeros) return -ENOMEM;
    while (size) {
        size_t  chunk = size > mnt->cluster_size ? mnt->cluster_size : (size_t)size;
        int64_t done  = ntfs_write_by_runlist(mnt, rl, rl_len, offset, zeros, chunk, max_size);
        if (done != (int64_t)chunk) {
            free(zeros);
            return -EIO;
        }
        offset += chunk;
        size -= chunk;
    }
    free(zeros);
    return 0;
}

/* Allocate contiguous free clusters from the volume bitmap. */
int ntfs_bitmap_find_free(ntfs_mount_t *mnt, uint64_t count, int64_t *extent_lcn, int64_t *extent_length, int *extent_count)
{
    uint8_t  byte      = 0;
    uint64_t remaining = count;
    int      extents   = 0;

    if (!mnt || !mnt->bitmap_runlist || !count || !extent_lcn || !extent_length || !extent_count) return -EINVAL;
    for (uint64_t cluster = 0; cluster < (uint64_t)mnt->nr_clusters && remaining; cluster++) {
        if (!(cluster & 7) && ntfs_read_by_runlist(mnt, mnt->bitmap_runlist, (int)mnt->bitmap_runlist_size, cluster >> 3, &byte, 1, mnt->bitmap_data_size) != 1) return -EIO;
        if (byte & (1U << (cluster & 7))) continue;
        if (extents && extent_lcn[extents - 1] + extent_length[extents - 1] == (int64_t)cluster) {
            extent_length[extents - 1]++;
        } else {
            if (extents == 256) return -ENOSPC;
            extent_lcn[extents]    = (int64_t)cluster;
            extent_length[extents] = 1;
            extents++;
        }
        remaining--;
    }
    if (remaining) return -ENOSPC;
    *extent_count = extents;
    return 0;
}

/* Set or clear cluster extents in the volume bitmap with rollback on failure. */
int ntfs_bitmap_change_extents(ntfs_mount_t *mnt, const int64_t *extent_lcn, const int64_t *extent_length, int extent_count, int allocated)
{
    typedef struct bitmap_byte {
            uint64_t offset;
            uint8_t  before;
            uint8_t  after;
    } bitmap_byte_t;
    bitmap_byte_t *bytes;
    uint64_t       cluster_count = 0;
    uint32_t       byte_count    = 0;
    uint32_t       committed     = 0;
    int            status        = -EIO;

    if (!mnt || !extent_lcn || !extent_length || extent_count < 0) return -EINVAL;
    for (int extent = 0; extent < extent_count; extent++) {
        if (extent_lcn[extent] < 0 || extent_length[extent] <= 0 || extent_lcn[extent] >= mnt->nr_clusters || extent_length[extent] > mnt->nr_clusters - extent_lcn[extent]) return -EIO;
        if ((uint64_t)extent_length[extent] > UINT64_MAX - cluster_count) return -EOVERFLOW;
        cluster_count += (uint64_t)extent_length[extent];
    }
    if (!cluster_count) return 0;
    if (cluster_count > UINT32_MAX / sizeof(*bytes)) return -EOVERFLOW;
    bytes = calloc((size_t)cluster_count, sizeof(*bytes));
    if (!bytes) return -ENOMEM;

    for (int extent = 0; extent < extent_count; extent++) {
        for (int64_t index = 0; index < extent_length[extent]; index++) {
            uint64_t cluster = (uint64_t)(extent_lcn[extent] + index);
            uint64_t offset  = cluster >> 3;
            uint32_t slot    = 0;
            while (slot < byte_count && bytes[slot].offset != offset) slot++;
            if (slot == byte_count) {
                if (ntfs_read_by_runlist(mnt, mnt->bitmap_runlist, (int)mnt->bitmap_runlist_size, offset, &bytes[slot].before, 1, mnt->bitmap_data_size) != 1) goto out;
                bytes[slot].offset = offset;
                bytes[slot].after  = bytes[slot].before;
                byte_count++;
            }
            if (allocated) {
                bytes[slot].after |= (uint8_t)(1U << (cluster & 7));
            } else {
                bytes[slot].after &= (uint8_t) ~(1U << (cluster & 7));
            }
        }
    }
    for (; committed < byte_count; committed++) {
        if (ntfs_write_by_runlist(mnt, mnt->bitmap_runlist, (int)mnt->bitmap_runlist_size, bytes[committed].offset, &bytes[committed].after, 1, mnt->bitmap_data_size) != 1) goto rollback;
    }
    status = 0;
    goto out;
rollback:
    while (committed) {
        committed--;
        ntfs_write_by_runlist(mnt, mnt->bitmap_runlist, (int)mnt->bitmap_runlist_size, bytes[committed].offset, &bytes[committed].before, 1, mnt->bitmap_data_size);
    }
out:
    free(bytes);
    return status;
}

/* Zero the clusters covered by a set of extents. */
static int zero_extents(ntfs_mount_t *mnt, const int64_t *extent_lcn, const int64_t *extent_length, int extent_count)
{
    uint8_t *zeros = calloc(1, mnt->cluster_size);
    if (!zeros) return -ENOMEM;
    for (int extent = 0; extent < extent_count; extent++) {
        for (int64_t index = 0; index < extent_length[extent]; index++) {
            if (ntfs_dev_write(mnt, (uint64_t)(extent_lcn[extent] + index) << mnt->cluster_bits, zeros, mnt->cluster_size) < 0) {
                free(zeros);
                return -EIO;
            }
        }
    }
    free(zeros);
    return 0;
}

/* Extend a nonresident data attribute, allocating clusters as needed. */
size_t ntfs_nonresident_grow(ntfs_handle_t *h, uint8_t *mft, attr_rec_t *attribute, uint32_t length, uint16_t mapping_offset, uint64_t offset, const uint8_t *buffer, size_t size, uint64_t data_size,
                             uint64_t initialized_size)
{
    int64_t  vcn[256], lcn[256], run[256];
    int64_t  new_lcn[256], new_length[256];
    int      run_count;
    int      extent_count;
    uint64_t write_end       = offset + size;
    uint64_t allocated_size  = load_le64((uint8_t *)&attribute->d.nres.alloc_size);
    uint64_t allocated_count = (allocated_size + h->mnt->cluster_size - 1) >> h->mnt->cluster_bits;
    uint64_t required_count  = (write_end + h->mnt->cluster_size - 1) >> h->mnt->cluster_bits;
    uint64_t new_allocated_size;
    uint8_t *mapping;
    uint8_t *new_cache;
    int      encoded_size;

    if (!h->mnt->bitmap_runlist || required_count > (uint64_t)h->mnt->nr_clusters) return 0;
    run_count = ntfs_runlist_parse((uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), vcn, lcn, run, 256);
    if (run_count <= 0 || (uint64_t)(vcn[run_count - 1] + run[run_count - 1]) != allocated_count) return 0;

    /* Extending inside the already allocated last cluster needs no new run. */
    if (required_count <= allocated_count) {
        if ((offset > initialized_size
             && ntfs_zero_by_runlist(h->mnt, (uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), initialized_size, offset - initialized_size, allocated_size) < 0)
            || ntfs_write_by_runlist(h->mnt, (uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), offset, buffer, size, allocated_size) != (int64_t)size) {
            return 0;
        }
        store_le64((uint8_t *)&attribute->d.nres.data_size, write_end);
        store_le64((uint8_t *)&attribute->d.nres.init_size, write_end);
        if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0 || ntfs_mft_write(h->mnt, h->mft_no, mft) < 0) return 0;
        h->file_size = write_end;
        return size;
    }
    if (required_count - allocated_count > (uint64_t)h->mnt->nr_clusters) return 0;
    if (ntfs_bitmap_find_free(h->mnt, required_count - allocated_count, new_lcn, new_length, &extent_count) < 0) return 0;

    for (int extent = 0; extent < extent_count; extent++) {
        if (run_count && lcn[run_count - 1] + run[run_count - 1] == new_lcn[extent]) {
            run[run_count - 1] += new_length[extent];
        } else {
            if (run_count == 256) return 0;
            vcn[run_count] = (int64_t)allocated_count;
            for (int previous = 0; previous < extent; previous++) vcn[run_count] += new_length[previous];
            lcn[run_count] = new_lcn[extent];
            run[run_count] = new_length[extent];
            run_count++;
        }
    }

    mapping = calloc(1, length - mapping_offset);
    if (!mapping) return 0;
    encoded_size = ntfs_runlist_encode(lcn, run, run_count, mapping, length - mapping_offset);
    if (encoded_size < 0) {
        free(mapping);
        return 0;
    }
    new_cache = malloc((size_t)encoded_size);
    if (!new_cache) {
        free(mapping);
        return 0;
    }
    memcpy(new_cache, mapping, (size_t)encoded_size);

    if (zero_extents(h->mnt, new_lcn, new_length, extent_count) < 0
        || (offset > initialized_size && ntfs_zero_by_runlist(h->mnt, mapping, encoded_size, initialized_size, offset - initialized_size, required_count << h->mnt->cluster_bits) < 0)
        || ntfs_write_by_runlist(h->mnt, mapping, encoded_size, offset, buffer, size, required_count << h->mnt->cluster_bits) != (int64_t)size) {
        free(new_cache);
        free(mapping);
        return 0;
    }
    if (ntfs_bitmap_change_extents(h->mnt, new_lcn, new_length, extent_count, 1) < 0) {
        ntfs_bitmap_change_extents(h->mnt, new_lcn, new_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }

    memset((uint8_t *)attribute + mapping_offset, 0, length - mapping_offset);
    memcpy((uint8_t *)attribute + mapping_offset, mapping, (size_t)encoded_size);
    new_allocated_size = required_count << h->mnt->cluster_bits;
    store_le64((uint8_t *)&attribute->d.nres.highest_vcn, required_count - 1);
    store_le64((uint8_t *)&attribute->d.nres.alloc_size, new_allocated_size);
    store_le64((uint8_t *)&attribute->d.nres.data_size, write_end);
    store_le64((uint8_t *)&attribute->d.nres.init_size, write_end);
    if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0) {
        ntfs_bitmap_change_extents(h->mnt, new_lcn, new_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }
    if (ntfs_mft_write(h->mnt, h->mft_no, mft) < 0) {
        ntfs_bitmap_change_extents(h->mnt, new_lcn, new_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }

    free(h->runlist_buf);
    h->runlist_buf = new_cache;
    h->runlist_sz  = (uint32_t)encoded_size;
    h->file_size   = write_end;
    free(mapping);
    (void)data_size;
    return size;
}

/* Convert a resident data attribute to a nonresident runlist-backed one. */
size_t ntfs_resident_convert(ntfs_handle_t *h, uint8_t *mft, attr_rec_t *attribute, uint32_t old_length, uint16_t value_offset, uint32_t value_length, uint64_t offset, const uint8_t *buffer,
                             size_t size)
{
    int64_t  extent_lcn[256], extent_length[256];
    int64_t  runs[256];
    uint64_t write_end       = offset + size;
    uint64_t cluster_count   = (write_end + h->mnt->cluster_size - 1) >> h->mnt->cluster_bits;
    uint32_t attribute_off   = (uint32_t)((uint8_t *)attribute - mft);
    uint32_t bytes_in_use    = load_le32(mft + 0x18);
    uint32_t bytes_allocated = load_le32(mft + 0x1c);
    uint16_t flags           = load_le16((uint8_t *)&attribute->flags);
    uint16_t instance        = load_le16((uint8_t *)&attribute->instance);
    int      extent_count;
    int      encoded_size;
    uint32_t new_length;
    uint8_t *mapping;
    uint8_t *new_cache;

    if (!h->mnt->bitmap_runlist || !cluster_count || bytes_in_use > bytes_allocated || bytes_allocated > h->mnt->mft_size || attribute_off + old_length > bytes_in_use) return 0;
    if (ntfs_bitmap_find_free(h->mnt, cluster_count, extent_lcn, extent_length, &extent_count) < 0) return 0;
    for (int extent = 0; extent < extent_count; extent++) runs[extent] = extent_length[extent];

    mapping = calloc(1, h->mnt->mft_size);
    if (!mapping) return 0;
    encoded_size = ntfs_runlist_encode(extent_lcn, runs, extent_count, mapping, h->mnt->mft_size);
    if (encoded_size < 0) {
        free(mapping);
        return 0;
    }
    new_length = (uint32_t)ALIGN_UP(64 + encoded_size, 8);
    if (new_length < old_length || new_length - old_length > bytes_allocated - bytes_in_use) {
        free(mapping);
        return 0;
    }
    new_cache = malloc((size_t)encoded_size);
    if (!new_cache) {
        free(mapping);
        return 0;
    }
    memcpy(new_cache, mapping, (size_t)encoded_size);

    if (zero_extents(h->mnt, extent_lcn, extent_length, extent_count) < 0
        || (value_length && ntfs_write_by_runlist(h->mnt, mapping, encoded_size, 0, (uint8_t *)attribute + value_offset, value_length, cluster_count << h->mnt->cluster_bits) != value_length)
        || ntfs_write_by_runlist(h->mnt, mapping, encoded_size, offset, buffer, size, cluster_count << h->mnt->cluster_bits) != (int64_t)size) {
        free(new_cache);
        free(mapping);
        return 0;
    }
    if (ntfs_bitmap_change_extents(h->mnt, extent_lcn, extent_length, extent_count, 1) < 0) {
        ntfs_bitmap_change_extents(h->mnt, extent_lcn, extent_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }

    memmove(mft + attribute_off + new_length, mft + attribute_off + old_length, bytes_in_use - attribute_off - old_length);
    memset(attribute, 0, new_length);
    store_le32((uint8_t *)&attribute->type, AT_DATA);
    store_le32((uint8_t *)&attribute->length, new_length);
    attribute->non_resident = 1;
    store_le16((uint8_t *)&attribute->flags, flags);
    store_le16((uint8_t *)&attribute->instance, instance);
    store_le64((uint8_t *)&attribute->d.nres.lowest_vcn, 0);
    store_le64((uint8_t *)&attribute->d.nres.highest_vcn, cluster_count - 1);
    store_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off, 64);
    store_le64((uint8_t *)&attribute->d.nres.alloc_size, cluster_count << h->mnt->cluster_bits);
    store_le64((uint8_t *)&attribute->d.nres.data_size, write_end);
    store_le64((uint8_t *)&attribute->d.nres.init_size, write_end);
    memcpy((uint8_t *)attribute + 64, mapping, (size_t)encoded_size);
    store_le32(mft + 0x18, bytes_in_use + new_length - old_length);
    if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0) {
        ntfs_bitmap_change_extents(h->mnt, extent_lcn, extent_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }
    if (ntfs_mft_write(h->mnt, h->mft_no, mft) < 0) {
        ntfs_bitmap_change_extents(h->mnt, extent_lcn, extent_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }

    free(h->runlist_buf);
    h->runlist_buf = new_cache;
    h->runlist_sz  = (uint32_t)encoded_size;
    h->file_size   = write_end;
    h->is_resident = 0;
    free(mapping);
    return size;
}

#endif
