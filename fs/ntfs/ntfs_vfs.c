/*
 *
 *      ntfs_vfs.c
 *      New Technology File System - VFS integration
 *
 *      2026/7/26 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/fs_txn.h>
#include <fs/core/vfs_stub.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/ntfs/ntfs.h>
#include <fs/ntfs/ntfs_index.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <libs/util/bitops.h>
#include <libs/util/byteorder.h>
#include <mem/heap.h>

#if CONFIG_NTFS_FS

/* Materialize one directory index entry as a VFS child node. */
static void add_dir_entry(vfs_node_t parent, ntfs_mount_t *mnt, uint8_t *entry, uint32_t entry_len)
{
    uint64_t mft_ref = load_le64(entry);

    /* validate FILE_NAME attr fits inside the entry */
    if (entry_len < 0x52) return; // need at least: entry_hdr(0x10) + fname_hdr(0x42)
    fname_attr_t *fna      = (fname_attr_t *)(entry + 0x10);
    uint32_t      fn_bytes = (uint32_t)fna->name_len * 2;
    if (fn_bytes > 510 || (uint32_t)0x52 + fn_bytes > entry_len) return; // filename overflow

    uint16_t *fname16 = ntfs_utf16_from(entry, 0x52, fna->name_len);
    if (!fname16) return;
    char *fname8 = ntfs_utf8_from_utf16(fname16, fna->name_len);
    if (!fname8) {
        free(fname16);
        return;
    }

    if (!strcmp(fname8, ".") || !strcmp(fname8, "..")) {
        free(fname8);
        free(fname16);
        return;
    }
    if (vfs_do_search(parent, fname8)) {
        free(fname8);
        free(fname16);
        return;
    }

    ntfs_handle_t *ch = calloc(1, sizeof(ntfs_handle_t));
    if (!ch) {
        free(fname8);
        free(fname16);
        return;
    }
    ch->mnt           = mnt;
    ch->mft_no        = mft_ref & 0x0000FFFFFFFFFFFFULL;
    ch->parent_mft_no = load_le64((uint8_t *)&fna->parent_dir) & 0x0000FFFFFFFFFFFFULL;
    ch->name          = fname16;
    ch->name_length   = fna->name_len;
    ch->is_dir        = (load_le32((uint8_t *)&fna->fa) & 0x10) ? 1 : 0;
    ch->file_size     = load_le64((uint8_t *)&fna->data_size);
    ch->file_attr     = load_le32((uint8_t *)&fna->fa);

    vfs_node_t child = vfs_node_alloc(parent, fname8);
    free(fname8);
    if (child) {
        child->handle = ch;
        if (ch->is_dir) {
            child->type = file_dir;
        } else if (load_le32((uint8_t *)&fna->rp_tag) == IO_REPARSE_TAG_SYMLINK) {
            child->type = file_symlink;
        } else {
            child->type = file_none;
        }
        child->size = ch->file_size;

        /*
         * Loaded nodes must use the same synchronous unlink path as newly
         * created nodes so namespace/metadata failures reach the caller.
         */
        child->flags |= VFS_NODE_DELETE_SYNC;
    } else {
        free(ch->name);
        free(ch);
    }
}

/* Materialize a directory's index entries as VFS children. */
static int ntfs_load_directory(ntfs_handle_t *h, vfs_node_t node)
{
    ntfs_mount_t      *mnt        = h->mnt;
    ntfs_index_item_t *items      = NULL;
    uint32_t           item_count = 0;
    uint8_t           *mft        = malloc(mnt->mft_size);
    int                status;
    if (!mft) return -ENOMEM;
    if (ntfs_mft_read(mnt, h->mft_no, mft) < 0) {
        free(mft);
        return -EIO;
    }

    mft_rec_t *mr = (mft_rec_t *)mft;
    if (mr->magic != MFT_MAGIC) {
        free(mft);
        return -EIO;
    }

    uint32_t off = load_le16((uint8_t *)&mr->attrs);

    while (off + 16 <= mnt->mft_size) {
        attr_rec_t *a  = (attr_rec_t *)(mft + off);
        uint32_t    at = load_le32((uint8_t *)&a->type);
        uint32_t    al = load_le32((uint8_t *)&a->length);
        if (at == AT_END || al < 16 || off + al > mnt->mft_size) break;

        if (at == AT_STANDARD_INFORMATION && !a->non_resident) {
            uint32_t voff = load_le16((uint8_t *)&a->d.res.value_offset);
            if (off + voff + 36 <= mnt->mft_size) h->file_attr = load_le32(mft + off + voff + 32);
        } else if (at == AT_DATA && a->name_length == 0) {
            uint16_t aflags = load_le16((uint8_t *)&a->flags);
            if (aflags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)) {
                h->file_size   = 0;
                h->is_resident = 0;
            } else if (a->non_resident) {
                h->file_size = load_le64((uint8_t *)&a->d.nres.data_size);
                uint32_t mp  = load_le16((uint8_t *)&a->d.nres.mapping_pairs_off);
                if (mp < al) {
                    h->runlist_sz = al - mp;
                    free(h->runlist_buf);
                    h->runlist_buf = malloc(h->runlist_sz);
                    if (h->runlist_buf) {
                        memcpy(h->runlist_buf, mft + off + mp, h->runlist_sz);
                    } else {
                        plogk("ntfs: runlist malloc %u failed for mft %llu\n", h->runlist_sz, h->mft_no);
                        h->runlist_sz = 0;
                    }
                }
                h->is_resident = 0;
            } else {
                h->file_size  = load_le32((uint8_t *)&a->d.res.value_length);
                uint16_t voff = load_le16((uint8_t *)&a->d.res.value_offset);
                if (voff + h->file_size <= al && h->file_size > 0) {
                    h->runlist_sz = (uint32_t)h->file_size;
                    free(h->runlist_buf);
                    h->runlist_buf = malloc(h->runlist_sz);
                    if (h->runlist_buf) {
                        memcpy(h->runlist_buf, mft + off + voff, h->runlist_sz);
                    } else {
                        plogk("ntfs: resident data malloc %u failed for mft %llu\n", h->runlist_sz, h->mft_no);
                        h->runlist_sz = 0;
                        h->file_size  = 0;
                    }
                }
                h->is_resident = 1;
            }
        }
        off += al;
    }

    /*
     * Use the same validated collector as namespace mutation.  The previous
     * ad-hoc root/allocation parser could silently omit valid leaf entries,
     * leaving files present on disk but absent from the VFS child cache.
     */
    status = ntfs_directory_index_collect(mnt, mft, &items, &item_count, NULL);
    if (status < 0) {
        free(mft);
        return status;
    }
    for (uint32_t index = 0; index < item_count; index++) add_dir_entry(node, mnt, items[index].entry, items[index].length);
    ntfs_index_items_free(items, item_count);

    free(mft);
    h->dir_loaded = 1;
    node->visited = 1;
    return 0;
}

/* Load a file's data runlist (or resident data) into the handle. */
static int ntfs_load_file_runlist(ntfs_handle_t *h, vfs_node_t node)
{
    ntfs_mount_t *mnt = h->mnt;
    uint8_t      *mft = malloc(mnt->mft_size);
    if (!mft) return -ENOMEM;
    if (ntfs_mft_read(mnt, h->mft_no, mft) < 0) {
        free(mft);
        return -EIO;
    }

    mft_rec_t *mr = (mft_rec_t *)mft;
    if (mr->magic != MFT_MAGIC) {
        free(mft);
        return -EIO;
    }

    uint32_t off = load_le16((uint8_t *)&mr->attrs);
    while (off + 16 <= mnt->mft_size) {
        attr_rec_t *a  = (attr_rec_t *)(mft + off);
        uint32_t    at = load_le32((uint8_t *)&a->type);
        uint32_t    al = load_le32((uint8_t *)&a->length);
        if (at == AT_END || al < 16 || off + al > mnt->mft_size) break;
        if (at == AT_DATA && a->name_length == 0) {
            uint16_t aflags = load_le16((uint8_t *)&a->flags);
            if (aflags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)) {
                /* compressed/encrypted: refuse to return garbage */
                h->file_size = 0;
                node->size   = 0;
                break;
            }
            if (a->non_resident) {
                h->file_size = load_le64((uint8_t *)&a->d.nres.data_size);
                node->size   = h->file_size;
                uint32_t mp  = load_le16((uint8_t *)&a->d.nres.mapping_pairs_off);
                if (mp < al) {
                    h->runlist_sz  = al - mp;
                    h->runlist_buf = malloc(h->runlist_sz);
                    if (h->runlist_buf) {
                        memcpy(h->runlist_buf, mft + off + mp, h->runlist_sz);
                    } else {
                        plogk("ntfs: runlist malloc %u failed for file mft %llu\n", h->runlist_sz, h->mft_no);
                        h->runlist_sz = 0;
                    }
                }
                h->is_resident = 0;
            } else {
                h->file_size  = load_le32((uint8_t *)&a->d.res.value_length);
                node->size    = h->file_size;
                uint16_t voff = load_le16((uint8_t *)&a->d.res.value_offset);
                if (voff + h->file_size <= al && h->file_size > 0) {
                    h->runlist_sz  = (uint32_t)h->file_size;
                    h->runlist_buf = malloc(h->runlist_sz);
                    if (h->runlist_buf) {
                        memcpy(h->runlist_buf, mft + off + voff, h->runlist_sz);
                    } else {
                        plogk("ntfs: resident malloc %u failed for file mft %llu\n", h->runlist_sz, h->mft_no);
                        h->runlist_sz = 0;
                        h->file_size  = 0;
                        node->size    = 0;
                    }
                }
                h->is_resident = 1;
            }
            break;
        }
        off += al;
    }

    free(mft);
    return 0;
}

/* Mount an NTFS volume from its boot sector and MFT. */
static int ntfs_vfs_mount(const char *src, vfs_node_t node)
{
    int  status;
    bool device_retained = false;
    if (!src || !node) return -EINVAL;

    blockdev_device_t dev;
    status = devtmpfs_open_block_device(src, &dev);
    if (status == EOK) {
        device_retained = true;
    } else if (status == -ENOENT) {
        status = blockdev_open_name(src, &dev);
    }
    if (status != EOK) return status == -ENOENT ? -ENODEV : status;

    uint8_t boot[512];
    if (blockdev_read_bytes(&dev, 0, boot, 512) < 0) {
        if (device_retained) blockdev_release(&dev);
        return -EIO;
    }

    ntfs_boot_sector_t *bs = (ntfs_boot_sector_t *)boot;
    if (load_le64((uint8_t *)&bs->oem_id) != magicNTFS) {
        if (device_retained) blockdev_release(&dev);
        return -EINVAL;
    }

    uint16_t bps = load_le16((uint8_t *)&bs->bpb.bytes_per_sector);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1))) {
        if (device_retained) blockdev_release(&dev);
        return -EINVAL;
    }

    int8_t   spc = bs->bpb.sectors_per_cluster;
    uint32_t sec_per_cluster;
    if ((uint8_t)spc >= 0xf4) {
        sec_per_cluster = 1U << -spc;
    } else {
        sec_per_cluster = (uint32_t)(uint8_t)spc;
    }
    if (!sec_per_cluster) {
        if (device_retained) blockdev_release(&dev);
        return -EINVAL;
    }

    ntfs_mount_t *mnt = calloc(1, sizeof(ntfs_mount_t));
    if (!mnt) {
        if (device_retained) blockdev_release(&dev);
        return -ENOMEM;
    }
    memcpy(&mnt->dev, &dev, sizeof(dev));

    mnt->sector_size  = bps;
    mnt->cluster_size = bps * sec_per_cluster;
    mnt->cluster_bits = ctz32(mnt->cluster_size);
    mnt->cluster_mask = mnt->cluster_size - 1;

    int8_t cmr    = bs->clusters_per_mft_record;
    mnt->mft_size = cmr > 0 ? mnt->cluster_size << (ffs32((uint32_t)cmr) - 1) : (1U << -cmr);
    if (mnt->mft_size < 256 || mnt->mft_size > 65536) {
        if (device_retained) blockdev_release(&dev);
        free(mnt);
        return -EINVAL;
    }
    mnt->mft_bits = ctz32(mnt->mft_size);

    int8_t cir     = bs->clusters_per_index_record;
    mnt->indx_size = cir > 0 ? mnt->cluster_size << (ffs32((uint32_t)cir) - 1) : (1U << -cir);
    if (mnt->indx_size < 512 || mnt->indx_size > 65536) {
        if (device_retained) blockdev_release(&dev);
        free(mnt);
        return -EINVAL;
    }

    mnt->mft_lcn     = load_le64((uint8_t *)&bs->mft_lcn);
    mnt->mftmirr_lcn = load_le64((uint8_t *)&bs->mftmirr_lcn);
    mnt->nr_clusters = load_le64((uint8_t *)&bs->number_of_sectors) >> (ctz32(sec_per_cluster));
    if (mnt->mft_lcn >= mnt->nr_clusters || mnt->mftmirr_lcn >= mnt->nr_clusters) {
        if (device_retained) blockdev_release(&dev);
        free(mnt);
        return -EINVAL;
    }

    /* blocks per cluster for index allocation VCN -> LCN mapping */
    mnt->indx_vcn_per_cluster = mnt->cluster_size / mnt->indx_size;
    if (mnt->indx_vcn_per_cluster == 0) mnt->indx_vcn_per_cluster = 1;

    status = fs_txn_log_init(&mnt->transaction_log, &mnt->dev, mnt->dev.sector_size, NULL, NULL);
    if (device_retained) blockdev_release(&dev);
    if (status != EOK) {
        free(mnt);
        return status;
    }
    mnt->transaction_log_initialized = 1;
    status                           = fs_txn_recover(&mnt->transaction_log);
    if (status != EOK) {
        fs_txn_log_destroy(&mnt->transaction_log);
        free(mnt);
        return status;
    }

    if (ntfs_prepare_write(mnt) < 0) plogk("ntfs: Volume mounted with write path disabled.\n");

    ntfs_handle_t *root_h = calloc(1, sizeof(ntfs_handle_t));
    if (!root_h) {
        free(mnt->upcase);
        free(mnt->bitmap_runlist);
        free(mnt->mft_runlist);
        fs_txn_log_destroy(&mnt->transaction_log);
        free(mnt);
        return -ENOMEM;
    }
    root_h->mnt    = mnt;
    root_h->mft_no = 5;
    root_h->is_dir = 1;

    node->type  = file_dir;
    node->blksz = mnt->cluster_size;

    if (ntfs_load_directory(root_h, node) < 0) {
        free(root_h);
        free(mnt->upcase);
        free(mnt->bitmap_runlist);
        free(mnt->mft_runlist);
        fs_txn_log_destroy(&mnt->transaction_log);
        free(mnt);
        return -EIO;
    }
    node->handle = root_h;
    return 0;
}

/* Unmount and release the NTFS mount. */
static void ntfs_vfs_unmount(void *root)
{
    ntfs_handle_t *h = root;
    if (!h) return;
    if (h->mnt->dirty_owned) {
        int status = ntfs_clear_owned_dirty(h->mnt);
        if (status != EOK) plogk("ntfs: Sync failed; volume dirty flag was preserved.\n");
    }
    free(h->runlist_buf);
    free(h->mnt->upcase);
    free(h->mnt->bitmap_runlist);
    free(h->mnt->mft_runlist);
    if (h->mnt->transaction_log_initialized) fs_txn_log_destroy(&h->mnt->transaction_log);
    free(h->mnt);
    free(h);
}

/* Lazily load a node's runlist or directory children. */
static void ntfs_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    if (!node || !node->handle) return;
    ntfs_handle_t *h = node->handle;

    if (!h->is_dir && !h->runlist_buf) ntfs_load_file_runlist(h, node);

    if (h->is_dir && !h->dir_loaded) ntfs_load_directory(h, node);
}

/* Read from a file, handling resident data and runlist extents. */
static size_t ntfs_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    ntfs_handle_t *h = file;
    if (!h || !addr || h->is_dir || !h->runlist_buf) return 0;
    if (h->is_resident) {
        if (offset >= h->file_size) return 0;
        size_t avail = (size_t)(h->file_size - offset);
        if (size > avail) size = avail;
        memcpy(addr, h->runlist_buf + offset, size);
        return size;
    }
    int r = ntfs_read_by_runlist(h->mnt, h->runlist_buf, h->runlist_sz, offset, addr, size, h->file_size);
    return r > 0 ? (size_t)r : 0;
}

/* Write to a file without the transaction wrapper. */
static size_t ntfs_vfs_write_locked(ntfs_handle_t *h, const void *addr, size_t offset, size_t size)
{
    uint8_t *mft;
    uint32_t attr_offset;

    if (!size) return 0;
    if (!h || !h->mnt || !h->mnt->write_enabled || !addr || h->is_dir || h->mnt->dev.read_only || offset > UINT64_MAX - size) return 0;

    mft = malloc(h->mnt->mft_size);
    if (!mft) return 0;
    if (ntfs_mft_read(h->mnt, h->mft_no, mft) < 0 || load_le32(mft) != MFT_MAGIC) {
        plogk("ntfs: Drive %u: MFT read failed or bad magic for record %llu during write.\n", h->mnt->dev.drive, h->mft_no);
        free(mft);
        return 0;
    }

    attr_offset = load_le16(mft + 0x14);
    while (attr_offset + 24 <= h->mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(mft + attr_offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || attr_offset + length > h->mnt->mft_size) break;
        if (type == AT_ATTRIBUTE_LIST) {
            plogk("ntfs: Drive %u: write of record %llu with unsupported ATTRIBUTE_LIST.\n", h->mnt->dev.drive, h->mft_no);
            free(mft);
            return 0;
        }
        if (type == AT_DATA && !attribute->name_length) {
            uint16_t flags     = load_le16((uint8_t *)&attribute->flags);
            uint64_t write_end = (uint64_t)offset + size;

            if (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) {
                plogk("ntfs: Drive %u: write to record %llu with unsupported attribute flags 0x%04x\n", h->mnt->dev.drive, h->mft_no, flags);
                free(mft);
                return 0;
            }

            if (attribute->non_resident) {
                uint16_t mapping_offset;
                uint64_t data_size;
                uint64_t initialized_size;
                int64_t  written;

                if (length < 64 || load_le64((uint8_t *)&attribute->d.nres.lowest_vcn) != 0) {
                    plogk("ntfs: Drive %u: corrupt non-resident attribute in record %llu (corrupt runlist)\n", h->mnt->dev.drive, h->mft_no);
                    free(mft);
                    return 0;
                }
                mapping_offset   = load_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off);
                data_size        = load_le64((uint8_t *)&attribute->d.nres.data_size);
                initialized_size = load_le64((uint8_t *)&attribute->d.nres.init_size);
                if (mapping_offset < 64 || mapping_offset >= length || initialized_size > data_size) {
                    plogk("ntfs: Drive %u: invalid attribute layout in record %llu\n", h->mnt->dev.drive, h->mft_no);
                    free(mft);
                    return 0;
                }
                if (write_end > data_size) {
                    size_t grown = ntfs_nonresident_grow(h, mft, attribute, length, mapping_offset, offset, addr, size, data_size, initialized_size);
                    free(mft);
                    return grown;
                }

                if (write_end > initialized_size && offset > initialized_size
                    && ntfs_zero_by_runlist(h->mnt, (uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), initialized_size, offset - initialized_size, data_size) < 0) {
                    free(mft);
                    return 0;
                }
                written = ntfs_write_by_runlist(h->mnt, (uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), offset, addr, size, data_size);
                if (written != (int64_t)size) {
                    free(mft);
                    return 0;
                }
                if (write_end > initialized_size) store_le64((uint8_t *)&attribute->d.nres.init_size, write_end);
                if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0 || ntfs_mft_write(h->mnt, h->mft_no, mft) < 0) {
                    free(mft);
                    return 0;
                }
                free(mft);
                return written > 0 ? (size_t)written : 0;
            }
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            uint8_t *new_cache;

            if (value_offset > length || value_length > length - value_offset || write_end > UINT32_MAX) {
                free(mft);
                return 0;
            }
            if (write_end > length - value_offset) {
                size_t converted = ntfs_resident_convert(h, mft, attribute, length, value_offset, value_length, offset, addr, size);
                free(mft);
                return converted;
            }

            if (write_end > value_length) {
                memset((uint8_t *)attribute + value_offset + value_length, 0, (size_t)write_end - value_length);
                value_length = (uint32_t)write_end;
                store_le32((uint8_t *)&attribute->d.res.value_length, value_length);
            }
            memcpy((uint8_t *)attribute + value_offset + offset, addr, size);

            new_cache = malloc(value_length);
            if (!new_cache) {
                free(mft);
                return 0;
            }
            memcpy(new_cache, (uint8_t *)attribute + value_offset, value_length);
            if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0 || ntfs_mft_write(h->mnt, h->mft_no, mft) < 0) {
                free(new_cache);
                free(mft);
                return 0;
            }
            free(h->runlist_buf);
            h->runlist_buf = new_cache;
            h->runlist_sz  = value_length;
            h->file_size   = value_length;
            h->is_resident = 1;
            free(mft);
            return size;
        }
        attr_offset += length;
    }

    free(mft);
    return 0;
}

/* Resize a file without the transaction wrapper. */
static int ntfs_vfs_resize_locked(ntfs_handle_t *h, uint64_t size)
{
    uint8_t *mft;
    uint32_t attr_offset;

    if (!h || !h->mnt || h->is_dir) return h && h->is_dir ? -EISDIR : -EINVAL;
    if (!h->mnt->write_enabled || h->mnt->dev.read_only) return -EROFS;
    if (size == h->file_size) return EOK;
    if (size > h->file_size) {
        uint8_t zero = 0;
        return ntfs_vfs_write_locked(h, &zero, size - 1, 1) == 1 ? EOK : -EIO;
    }

    mft = malloc(h->mnt->mft_size);
    if (!mft) return -ENOMEM;
    if (ntfs_mft_read(h->mnt, h->mft_no, mft) < 0 || load_le32(mft) != MFT_MAGIC) {
        free(mft);
        return -EIO;
    }

    attr_offset = load_le16(mft + 0x14);
    while (attr_offset + 24 <= h->mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(mft + attr_offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        int         status    = -EIO;

        if (type == AT_END) break;
        if (length < 24 || attr_offset + length > h->mnt->mft_size || type == AT_ATTRIBUTE_LIST) break;
        if (type != AT_DATA || attribute->name_length) {
            attr_offset += length;
            continue;
        }
        if (load_le16((uint8_t *)&attribute->flags) & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) {
            free(mft);
            return -EOPNOTSUPP;
        }

        if (!attribute->non_resident) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            uint8_t *new_cache    = NULL;

            if (value_offset > length || value_length > length - value_offset || size > value_length) break;
            if (size) {
                new_cache = malloc((size_t)size);
                if (!new_cache) {
                    free(mft);
                    return -ENOMEM;
                }
                memcpy(new_cache, (uint8_t *)attribute + value_offset, (size_t)size);
            }
            memset((uint8_t *)attribute + value_offset + size, 0, value_length - (uint32_t)size);
            store_le32((uint8_t *)&attribute->d.res.value_length, (uint32_t)size);
            status = ntfs_record_touch(mft, h->mnt->mft_size, 1);
            if (status == EOK) status = ntfs_mft_write(h->mnt, h->mft_no, mft);
            if (status == EOK) {
                free(h->runlist_buf);
                h->runlist_buf = new_cache;
                h->runlist_sz  = (uint32_t)size;
                h->file_size   = size;
                h->is_resident = 1;
                new_cache      = NULL;
            }
            free(new_cache);
            free(mft);
            return status;
        }

        if (length < 64 || load_le64((uint8_t *)&attribute->d.nres.lowest_vcn) != 0) break;
        uint16_t mapping_offset = load_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off);
        uint64_t data_size      = load_le64((uint8_t *)&attribute->d.nres.data_size);
        uint64_t allocated_size = load_le64((uint8_t *)&attribute->d.nres.alloc_size);
        uint64_t initialized    = load_le64((uint8_t *)&attribute->d.nres.init_size);
        if (mapping_offset < 64 || mapping_offset >= length || size > data_size || initialized > data_size || allocated_size % h->mnt->cluster_size) break;

        int64_t  vcn[256], lcn[256], run[256];
        int64_t  kept_lcn[256], kept_run[256], free_lcn[256], free_run[256];
        int      run_count       = ntfs_runlist_parse((uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), vcn, lcn, run, 256);
        uint64_t allocated_count = allocated_size >> h->mnt->cluster_bits;
        uint64_t required_count  = (size + h->mnt->cluster_size - 1) >> h->mnt->cluster_bits;
        int      kept_count = 0, free_count = 0;
        if (run_count <= 0 || (uint64_t)(vcn[run_count - 1] + run[run_count - 1]) != allocated_count) break;
        for (int index = 0; index < run_count; index++) {
            if (lcn[index] < 0 || run[index] <= 0) break;
            uint64_t keep = 0;
            if ((uint64_t)vcn[index] < required_count) {
                keep = required_count - (uint64_t)vcn[index];
                if (keep > (uint64_t)run[index]) keep = (uint64_t)run[index];
            }
            if (keep) {
                kept_lcn[kept_count] = lcn[index];
                kept_run[kept_count] = (int64_t)keep;
                kept_count++;
            }
            if (keep < (uint64_t)run[index]) {
                free_lcn[free_count] = lcn[index] + (int64_t)keep;
                free_run[free_count] = run[index] - (int64_t)keep;
                free_count++;
            }
        }
        if ((required_count && !kept_count) || free_count > 256) break;

        uint8_t *new_cache    = NULL;
        int      encoded_size = 0;
        if (required_count) {
            new_cache = calloc(1, length - mapping_offset);
            if (!new_cache) {
                free(mft);
                return -ENOMEM;
            }
            encoded_size = ntfs_runlist_encode(kept_lcn, kept_run, kept_count, new_cache, length - mapping_offset);
            if (encoded_size < 0) {
                free(new_cache);
                break;
            }
            uint64_t retained_size = required_count << h->mnt->cluster_bits;
            if (retained_size > size && ntfs_zero_by_runlist(h->mnt, (uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), size, retained_size - size, allocated_size) < 0) {
                free(new_cache);
                break;
            }
        }
        status = ntfs_bitmap_change_extents(h->mnt, free_lcn, free_run, free_count, 0);
        if (status != EOK) {
            free(new_cache);
            free(mft);
            return status;
        }

        if (!required_count) {
            uint32_t bytes_in_use = load_le32(mft + 0x18);
            uint16_t flags        = load_le16((uint8_t *)&attribute->flags);
            uint16_t instance     = load_le16((uint8_t *)&attribute->instance);
            if (bytes_in_use > h->mnt->mft_size || attr_offset + length > bytes_in_use) {
                status = -EIO;
            } else {
                memmove(mft + attr_offset + 24, mft + attr_offset + length, bytes_in_use - attr_offset - length);
                memset(mft + bytes_in_use - (length - 24), 0, length - 24);
                attribute = (attr_rec_t *)(mft + attr_offset);
                memset(attribute, 0, 24);
                store_le32((uint8_t *)&attribute->type, AT_DATA);
                store_le32((uint8_t *)&attribute->length, 24);
                store_le16((uint8_t *)&attribute->flags, flags);
                store_le16((uint8_t *)&attribute->instance, instance);
                store_le16((uint8_t *)&attribute->d.res.value_offset, 24);
                store_le32(mft + 0x18, bytes_in_use - length + 24);
                status = ntfs_record_touch(mft, h->mnt->mft_size, 1);
            }
        } else {
            memset((uint8_t *)attribute + mapping_offset, 0, length - mapping_offset);
            memcpy((uint8_t *)attribute + mapping_offset, new_cache, (size_t)encoded_size);
            store_le64((uint8_t *)&attribute->d.nres.highest_vcn, required_count - 1);
            store_le64((uint8_t *)&attribute->d.nres.alloc_size, required_count << h->mnt->cluster_bits);
            store_le64((uint8_t *)&attribute->d.nres.data_size, size);
            store_le64((uint8_t *)&attribute->d.nres.init_size, initialized > size ? size : initialized);
            status = ntfs_record_touch(mft, h->mnt->mft_size, 1);
        }
        if (status == EOK) status = ntfs_mft_write(h->mnt, h->mft_no, mft);
        if (status != EOK) {
            (void)ntfs_bitmap_change_extents(h->mnt, free_lcn, free_run, free_count, 1);
        } else {
            free(h->runlist_buf);
            h->runlist_buf = new_cache;
            h->runlist_sz  = required_count ? (uint32_t)encoded_size : 0;
            h->file_size   = size;
            h->is_resident = !required_count;
            new_cache      = NULL;
        }
        free(new_cache);
        free(mft);
        return status;
    }

    free(mft);
    return -EIO;
}

/* VFS write callback wrapped in a transaction. */
static size_t ntfs_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    ntfs_handle_t *h = file;
    size_t         written;

    if (!h || !h->mnt) return 0;
    fs_txn_t transaction;
    int      status;
    spin_lock(&h->mnt->write_lock);
    if (!h->mnt->dirty_owned) {
        if (ntfs_set_volume_dirty(h->mnt, 1) < 0) {
            spin_unlock(&h->mnt->write_lock);
            return 0;
        }
        h->mnt->dirty_owned = 1;
    }
    uint64_t credits = (((uint64_t)size + h->mnt->dev.sector_size - 1) / h->mnt->dev.sector_size) + 8192;
    if (credits > UINT32_MAX || ntfs_transaction_begin(h->mnt, &transaction, (uint32_t)credits) != EOK) {
        spin_unlock(&h->mnt->write_lock);
        return 0;
    }
    written = ntfs_vfs_write_locked(h, addr, offset, size);
    status  = ntfs_transaction_finish(h->mnt, &transaction, written == size ? EOK : -EIO);
    if (status != EOK) written = 0;
    spin_unlock(&h->mnt->write_lock);
    return written;
}

/* VFS resize callback wrapped in a transaction. */
static int ntfs_vfs_resize(void *file, uint64_t size)
{
    ntfs_handle_t *h = file;
    fs_txn_t       transaction;
    int            status;

    if (!h || !h->mnt) return -EINVAL;
    spin_lock(&h->mnt->write_lock);
    if (!h->mnt->dirty_owned) {
        status = ntfs_set_volume_dirty(h->mnt, 1);
        if (status != EOK) {
            spin_unlock(&h->mnt->write_lock);
            return status;
        }
        h->mnt->dirty_owned = 1;
    }
    status = ntfs_transaction_begin(h->mnt, &transaction, 65536);
    if (status == EOK) {
        status = ntfs_vfs_resize_locked(h, size);
        status = ntfs_transaction_finish(h->mnt, &transaction, status);
    }
    spin_unlock(&h->mnt->write_lock);
    return status;
}

/* Read a symlink target from the record's reparse-point attribute. */
static size_t ntfs_vfs_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    ntfs_handle_t *handle;
    uint8_t       *record;
    size_t         copied = 0;

    if (!n || !n->handle || !a || !s) return 0;
    handle = n->handle;
    record = malloc(handle->mnt->mft_size);
    if (!record) return 0;
    if (ntfs_mft_read(handle->mnt, handle->mft_no, record) < 0) goto out;
    uint32_t offset = load_le16(record + 0x14);
    uint32_t used   = load_le32(record + 0x18);
    while (offset + 24 <= used) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) break;
        if (type == AT_REPARSE_POINT && !attribute->non_resident && !attribute->name_length) {
            uint16_t value_offset = load_le16((uint8_t *)&attribute->d.res.value_offset);
            uint32_t value_length = load_le32((uint8_t *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < 20 || value_length > length - value_offset) break;
            uint8_t *value = (uint8_t *)attribute + value_offset;
            if (load_le32(value) != IO_REPARSE_TAG_SYMLINK || load_le16(value + 4) != value_length - 8) break;
            uint16_t substitute_offset = load_le16(value + 8);
            uint16_t substitute_length = load_le16(value + 10);
            if ((substitute_offset & 1) || (substitute_length & 1) || substitute_offset > value_length - 20 || substitute_length > value_length - 20 - substitute_offset) break;
            uint16_t *target = ntfs_utf16_from(value + 20, substitute_offset, substitute_length / 2);
            char     *utf8   = ntfs_utf8_from_utf16(target, substitute_length / 2);
            free(target);
            if (!utf8) break;
            size_t target_length = strlen(utf8);
            if (o < target_length) {
                copied = target_length - o;
                if (copied > s) copied = s;
                memcpy(a, utf8 + o, copied);
            }
            free(utf8);
            break;
        }
        offset += length;
    }
out:
    free(record);
    return copied;
}

/* Create a file or directory record and link it into the parent index. */
static int ntfs_namespace_create_locked(ntfs_handle_t *parent, const char *source_name, vfs_node_t node, int directory)
{
    ntfs_mount_t  *mnt;
    ntfs_handle_t *child         = NULL;
    uint8_t       *parent_record = NULL;
    uint8_t       *child_record  = NULL;
    uint16_t      *name          = NULL;
    uint8_t        name_length;
    uint64_t       record_number;
    uint64_t       parent_reference;
    uint64_t       child_reference;
    uint16_t       child_sequence = 1;
    int            status;

    if (!parent || !parent->mnt || !source_name || !node || !parent->is_dir) return -EINVAL;
    mnt = parent->mnt;
    if (!mnt->write_enabled || mnt->dev.read_only) return -EROFS;
    status = ntfs_name_from_utf8(source_name, &name, &name_length);
    if (status < 0) return status;
    parent_record = malloc(mnt->mft_size);
    child_record  = malloc(mnt->mft_size);
    child         = calloc(1, sizeof(*child));
    if (!parent_record || !child_record || !child) {
        status = -ENOMEM;
        goto out;
    }
    child->name = malloc((uint32_t)name_length * sizeof(uint16_t));
    if (!child->name) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(child->name, name, (uint32_t)name_length * sizeof(uint16_t));

    if (ntfs_mft_read(mnt, parent->mft_no, parent_record) < 0 || load_le32(parent_record) != MFT_MAGIC || !(load_le16(parent_record + 0x16) & 2)) {
        status = -EIO;
        goto out;
    }
    status = ntfs_mft_bitmap_find_free(mnt, &record_number);
    if (status < 0) goto out;
    parent_reference = parent->mft_no | ((uint64_t)load_le16(parent_record + 0x10) << 48);
    child_reference  = record_number | ((uint64_t)child_sequence << 48);
    status           = ntfs_build_file_record(mnt, child_record, record_number, child_sequence, parent_reference, name, name_length, directory);
    if (status < 0) goto out;
    status = ntfs_mft_write(mnt, record_number, child_record);
    if (status < 0) goto out;
    status = ntfs_mft_bitmap_set(mnt, record_number, 1);
    if (status < 0) goto out;
    status = ntfs_directory_index_insert(mnt, parent_record, child_reference, parent_reference, name, name_length, directory ? 0x10 : 0x20, 0, 0);
    if (status < 0) {
        ntfs_mft_bitmap_set(mnt, record_number, 0);
        goto out;
    }
    status = ntfs_mft_write(mnt, parent->mft_no, parent_record);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, record_number, name, name_length);
        ntfs_index_reclaim_abort(mnt);
        ntfs_mft_bitmap_set(mnt, record_number, 0);
        goto out;
    }

    child->mnt           = mnt;
    child->mft_no        = record_number;
    child->parent_mft_no = parent->mft_no;
    child->name_length   = name_length;
    child->file_attr     = directory ? 0x10 : 0x20;
    child->is_dir        = directory;
    child->is_resident   = directory ? 0 : 1;
    node->handle         = child;
    node->inode          = record_number;
    node->size           = 0;
    node->blksz          = mnt->cluster_size;
    node->type           = directory ? file_dir : file_none;
    node->flags |= VFS_NODE_DELETE_SYNC;
    child  = NULL;
    status = 0;
out:
    if (child) free(child->name);
    free(child);
    free(child_record);
    free(parent_record);
    free(name);
    return status;
}

/* Create a namespace entry inside a transaction. */
static int ntfs_namespace_create(ntfs_handle_t *parent, const char *name, vfs_node_t node, int directory)
{
    int status;

    if (!parent || !parent->mnt) return -EINVAL;
    fs_txn_t transaction;
    spin_lock(&parent->mnt->write_lock);
    if (!parent->mnt->dirty_owned) {
        status = ntfs_set_volume_dirty(parent->mnt, 1);
        if (status < 0) {
            spin_unlock(&parent->mnt->write_lock);
            return status;
        }
        parent->mnt->dirty_owned = 1;
    }
    status = ntfs_transaction_begin(parent->mnt, &transaction, 65536);
    if (status != EOK) {
        spin_unlock(&parent->mnt->write_lock);
        return status;
    }
    status = ntfs_namespace_create_locked(parent, name, node, directory);
    status = ntfs_transaction_finish(parent->mnt, &transaction, status);
    spin_unlock(&parent->mnt->write_lock);
    return status;
}

/* Add a second file-name attribute and index entry for an inode. */
static int ntfs_namespace_hardlink_locked(ntfs_handle_t *parent, ntfs_handle_t *target, const char *source_name, vfs_node_t node)
{
    ntfs_mount_t  *mnt;
    ntfs_handle_t *link          = NULL;
    uint8_t       *parent_record = NULL;
    uint8_t       *target_record = NULL;
    uint8_t       *old_record    = NULL;
    uint16_t      *name          = NULL;
    uint8_t        name_length;
    uint64_t       parent_reference;
    uint64_t       target_reference;
    uint16_t       links;
    int            status;

    if (!parent || !target || !node || !source_name || parent->mnt != target->mnt || !parent->is_dir || target->is_dir) return -EINVAL;
    mnt = parent->mnt;
    if (!mnt->write_enabled || mnt->dev.read_only) return -EROFS;
    status = ntfs_name_from_utf8(source_name, &name, &name_length);
    if (status < 0) return status;
    parent_record = malloc(mnt->mft_size);
    target_record = malloc(mnt->mft_size);
    old_record    = malloc(mnt->mft_size);
    link          = calloc(1, sizeof(*link));
    if (!parent_record || !target_record || !old_record || !link) {
        status = -ENOMEM;
        goto out;
    }
    link->name = malloc((uint32_t)name_length * sizeof(uint16_t));
    if (!link->name) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(link->name, name, (uint32_t)name_length * sizeof(uint16_t));
    if (ntfs_mft_read(mnt, parent->mft_no, parent_record) < 0 || ntfs_mft_read(mnt, target->mft_no, target_record) < 0) {
        status = -EIO;
        goto out;
    }
    links = load_le16(target_record + 0x12);
    if (!links || links == UINT16_MAX || !(load_le16(target_record + 0x16) & 1)) {
        status = -EIO;
        goto out;
    }
    memcpy(old_record, target_record, mnt->mft_size);
    parent_reference = parent->mft_no | ((uint64_t)load_le16(parent_record + 0x10) << 48);
    target_reference = target->mft_no | ((uint64_t)load_le16(target_record + 0x10) << 48);
    status           = ntfs_file_name_add(mnt, target_record, parent_reference, name, name_length, target->file_attr, target->file_size);
    if (status < 0) goto out;
    store_le16(target_record + 0x12, links + 1);
    status = ntfs_mft_write(mnt, target->mft_no, target_record);
    if (status < 0) goto out;
    status = ntfs_directory_index_insert(mnt, parent_record, target_reference, parent_reference, name, name_length, target->file_attr, target->file_size, target->file_size);
    if (status < 0) {
        ntfs_mft_write(mnt, target->mft_no, old_record);
        goto out;
    }
    status = ntfs_mft_write(mnt, parent->mft_no, parent_record);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, target_reference, name, name_length);
        ntfs_index_reclaim_abort(mnt);
        ntfs_mft_write(mnt, target->mft_no, old_record);
        goto out;
    }
    link->mnt           = mnt;
    link->mft_no        = target->mft_no;
    link->parent_mft_no = parent->mft_no;
    link->file_size     = target->file_size;
    link->file_attr     = target->file_attr;
    link->name_length   = name_length;
    link->is_resident   = target->is_resident;
    node->handle        = link;
    node->inode         = target->mft_no;
    node->size          = target->file_size;
    node->blksz         = mnt->cluster_size;
    node->type          = file_none;
    node->flags |= VFS_NODE_DELETE_SYNC;
    link   = NULL;
    status = 0;
out:
    if (link) free(link->name);
    free(link);
    free(old_record);
    free(target_record);
    free(parent_record);
    free(name);
    return status;
}

/* Create a hard link inside a transaction. */
static int ntfs_namespace_hardlink(ntfs_handle_t *parent, ntfs_handle_t *target, const char *name, vfs_node_t node)
{
    if (!parent || !parent->mnt) return -EINVAL;
    int      status;
    fs_txn_t transaction;
    spin_lock(&parent->mnt->write_lock);
    if (!parent->mnt->dirty_owned) {
        status = ntfs_set_volume_dirty(parent->mnt, 1);
        if (status < 0) {
            spin_unlock(&parent->mnt->write_lock);
            return status;
        }
        parent->mnt->dirty_owned = 1;
    }
    status = ntfs_transaction_begin(parent->mnt, &transaction, 65536);
    if (status != EOK) {
        spin_unlock(&parent->mnt->write_lock);
        return status;
    }
    status = ntfs_namespace_hardlink_locked(parent, target, name, node);
    status = ntfs_transaction_finish(parent->mnt, &transaction, status);
    spin_unlock(&parent->mnt->write_lock);
    return status;
}

/* Create a symlink record and register its reparse-point index entry. */
static int ntfs_namespace_symlink_locked(ntfs_handle_t *parent, const char *source_name, const char *source_target, vfs_node_t node)
{
    ntfs_mount_t  *mnt;
    ntfs_handle_t *link          = NULL;
    uint8_t       *parent_record = NULL;
    uint8_t       *link_record   = NULL;
    uint16_t      *name          = NULL;
    uint16_t      *target        = NULL;
    uint8_t        name_length;
    uint8_t        target_length;
    uint64_t       record_number;
    uint64_t       parent_reference;
    uint64_t       link_reference;
    int            relative;
    int            status;

    if (!parent || !parent->mnt || !parent->is_dir || !source_name || !source_target || !node) return -EINVAL;
    mnt = parent->mnt;
    if (!mnt->write_enabled || mnt->dev.read_only) return -EROFS;
    status = ntfs_name_from_utf8(source_name, &name, &name_length);
    if (status < 0) return status;
    status = ntfs_utf16_from_utf8(source_target, &target, &target_length, 0);
    if (status < 0) goto out;
    relative = source_target[0] != '/' && source_target[0] != '\\'
               && !(((source_target[0] >= 'A' && source_target[0] <= 'Z') || (source_target[0] >= 'a' && source_target[0] <= 'z')) && source_target[1] == ':');
    parent_record = malloc(mnt->mft_size);
    link_record   = malloc(mnt->mft_size);
    link          = calloc(1, sizeof(*link));
    if (!parent_record || !link_record || !link) {
        status = -ENOMEM;
        goto out;
    }
    link->name = malloc((uint32_t)name_length * sizeof(uint16_t));
    if (!link->name) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(link->name, name, (uint32_t)name_length * sizeof(uint16_t));
    if (ntfs_mft_read(mnt, parent->mft_no, parent_record) < 0 || !(load_le16(parent_record + 0x16) & 2)) {
        status = -EIO;
        goto out;
    }
    status = ntfs_mft_bitmap_find_free(mnt, &record_number);
    if (status < 0) goto out;
    parent_reference = parent->mft_no | ((uint64_t)load_le16(parent_record + 0x10) << 48);
    link_reference   = record_number | (1ULL << 48);
    status           = ntfs_build_symlink_record(mnt, link_record, record_number, 1, parent_reference, name, name_length, target, target_length, relative);
    if (status < 0) goto out;
    status = ntfs_mft_write(mnt, record_number, link_record);
    if (status < 0) goto out;
    status = ntfs_mft_bitmap_set(mnt, record_number, 1);
    if (status < 0) goto out;
    status = ntfs_directory_index_insert(mnt, parent_record, link_reference, parent_reference, name, name_length, 0x20 | FILE_ATTRIBUTE_REPARSE_POINT, 0, 0);
    if (status < 0) {
        ntfs_mft_bitmap_set(mnt, record_number, 0);
        goto out;
    }
    status = ntfs_directory_index_set_reparse_tag(mnt, parent_record, record_number, name, name_length);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, record_number, name, name_length);
        ntfs_index_reclaim_abort(mnt);
        ntfs_mft_bitmap_set(mnt, record_number, 0);
        goto out;
    }
    status = ntfs_mft_write(mnt, parent->mft_no, parent_record);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, record_number, name, name_length);
        ntfs_index_reclaim_abort(mnt);
        ntfs_mft_bitmap_set(mnt, record_number, 0);
        goto out;
    }
    link->mnt           = mnt;
    link->mft_no        = record_number;
    link->parent_mft_no = parent->mft_no;
    link->file_attr     = 0x20 | FILE_ATTRIBUTE_REPARSE_POINT;
    link->name_length   = name_length;
    link->is_resident   = 1;
    node->handle        = link;
    node->inode         = record_number;
    node->blksz         = mnt->cluster_size;
    node->type          = file_symlink;
    node->flags |= VFS_NODE_DELETE_SYNC;
    link   = NULL;
    status = 0;
out:
    if (link) free(link->name);
    free(link);
    free(link_record);
    free(parent_record);
    free(target);
    free(name);
    return status;
}

/* Create a symlink inside a transaction. */
static int ntfs_namespace_symlink(ntfs_handle_t *parent, const char *name, const char *target, vfs_node_t node)
{
    int      status;
    fs_txn_t transaction;
    if (!parent || !parent->mnt) return -EINVAL;
    spin_lock(&parent->mnt->write_lock);
    if (!parent->mnt->dirty_owned) {
        status = ntfs_set_volume_dirty(parent->mnt, 1);
        if (status < 0) {
            spin_unlock(&parent->mnt->write_lock);
            return status;
        }
        parent->mnt->dirty_owned = 1;
    }
    status = ntfs_transaction_begin(parent->mnt, &transaction, 65536);
    if (status != EOK) {
        spin_unlock(&parent->mnt->write_lock);
        return status;
    }
    status = ntfs_namespace_symlink_locked(parent, name, target, node);
    status = ntfs_transaction_finish(parent->mnt, &transaction, status);
    spin_unlock(&parent->mnt->write_lock);
    return status;
}

/* Create a directory inside a transaction. */
static int ntfs_vfs_mkdir(void *p, const char *n, vfs_node_t nd)
{
    return ntfs_namespace_create(p, n, nd, 1);
}

/* Create a file inside a transaction. */
static int ntfs_vfs_mkfile(void *p, const char *n, vfs_node_t nd)
{
    return ntfs_namespace_create(p, n, nd, 0);
}

/* Create a hard link through the VFS. */
static int ntfs_vfs_link(void *p, const char *target_name, vfs_node_t node)
{
    vfs_node_t target;
    int        status;

    if (!p || !target_name || !node || !node->name) return -EINVAL;
    target = vfs_open_nofollow(target_name);
    if (!target || !target->handle) {
        if (target) vfs_close(target);
        return -ENOENT;
    }
    status = ntfs_namespace_hardlink(p, target->handle, node->name, node);
    vfs_close(target);
    return status;
}

/* Create a symlink through the VFS. */
static int ntfs_vfs_symlink(void *p, const char *target_name, vfs_node_t node)
{
    if (!node || !node->name) return -EINVAL;
    return ntfs_namespace_symlink(p, node->name, target_name, node);
}

/* Refresh a node's size and lazily load its children or runlist. */
static int ntfs_vfs_stat(void *file, vfs_node_t nd)
{
    ntfs_handle_t *h = file;
    if (!h || !nd) return -EINVAL;

    /*
     * VFS nodes materialized from a directory index already have a handle, so
     * traversal invokes stat rather than open.  Populate nested directories
     * here as well as regular-file data, otherwise only the mount root gets a
     * child cache and existing nested files disappear from pathname lookup.
     */
    if (h->is_dir && !h->dir_loaded) {
        int status = ntfs_load_directory(h, nd);
        if (status < 0) return status;
    } else if (!h->is_dir && !h->runlist_buf) {
        int status = ntfs_load_file_runlist(h, nd);
        if (status < 0) return status;
    }

    nd->size  = h->file_size;
    nd->inode = h->mft_no;
    nd->blksz = h->mnt->cluster_size;
    return 0;
}

/* Free a handle; the shared mount is released on unmount. */
static int ntfs_vfs_free(void *handle)
{
    ntfs_handle_t *h = handle;
    if (!h) return -EINVAL;
    free(h->runlist_buf);
    free(h->name);

    /* note: mnt is shared, freed by unmount */
    free(h);
    return 0;
}

/* Unlink a file or directory, freeing its extents and index blocks. */
static int ntfs_vfs_delete(void *p, vfs_node_t n)
{
    ntfs_handle_t *parent = p;
    ntfs_handle_t *child;
    ntfs_mount_t  *mnt;
    uint8_t       *parent_record;
    uint8_t       *child_record;
    uint8_t       *old_child_record;
    int64_t        data_lcn[256];
    int64_t        data_length[256];
    int64_t        index_lcn[256];
    int64_t        index_length[256];
    int            data_extents        = 0;
    int            index_extents       = 0;
    int            namespace_committed = 0;
    int            status;
    fs_txn_t       transaction;
    int            transaction_started = 0;

    if (!parent || !n || !n->handle) return -EINVAL;
    child = n->handle;
    mnt   = parent->mnt;
    if (!mnt || child->mnt != mnt || child->parent_mft_no != parent->mft_no || !child->name || !child->name_length) return -EINVAL;
    if (!mnt->write_enabled || mnt->dev.read_only) return -EROFS;
    parent_record    = malloc(mnt->mft_size);
    child_record     = malloc(mnt->mft_size);
    old_child_record = malloc(mnt->mft_size);
    if (!parent_record || !child_record || !old_child_record) {
        free(parent_record);
        free(child_record);
        free(old_child_record);
        return -ENOMEM;
    }

    spin_lock(&mnt->write_lock);
    if (!mnt->dirty_owned) {
        status = ntfs_set_volume_dirty(mnt, 1);
        if (status < 0) goto unlock;
        mnt->dirty_owned = 1;
    }
    status = ntfs_transaction_begin(mnt, &transaction, 65536);
    if (status < 0) goto unlock;
    transaction_started = 1;
    if (ntfs_mft_read(mnt, parent->mft_no, parent_record) < 0 || ntfs_mft_read(mnt, child->mft_no, child_record) < 0) {
        status = -EIO;
        goto unlock;
    }
    uint16_t link_count = load_le16(child_record + 0x12);
    if (!(load_le16(child_record + 0x16) & 1) || !link_count) {
        status = -EIO;
        goto unlock;
    }
    if (link_count > 1) {
        uint64_t parent_reference = parent->mft_no | ((uint64_t)load_le16(parent_record + 0x10) << 48);
        memcpy(old_child_record, child_record, mnt->mft_size);
        status = ntfs_directory_index_remove(mnt, parent_record, child->mft_no, child->name, child->name_length);
        if (status < 0) goto unlock;
        status = ntfs_file_name_remove(mnt, child_record, parent_reference, child->name, child->name_length);
        if (status < 0) goto unlock;
        store_le16(child_record + 0x12, link_count - 1);
        status = ntfs_mft_write(mnt, child->mft_no, child_record);
        if (status < 0) goto unlock;
        status = ntfs_mft_write(mnt, parent->mft_no, parent_record);
        if (status < 0) {
            ntfs_mft_write(mnt, child->mft_no, old_child_record);
        } else {
            namespace_committed = 1;
            status              = ntfs_index_reclaim_commit(mnt);
        }
        goto unlock;
    }
    if (child->is_dir) {
        ntfs_index_attributes_t attributes;
        ntfs_index_item_t      *items      = NULL;
        uint32_t                item_count = 0;
        status                             = ntfs_directory_index_collect(mnt, child_record, &items, &item_count, &attributes);
        ntfs_index_items_free(items, item_count);
        if (status < 0) goto unlock;
        if (item_count) {
            status = -ENOTEMPTY;
            goto unlock;
        }
        if (attributes.allocation) {
            uint32_t allocation_length = load_le32((uint8_t *)&attributes.allocation->length);
            uint16_t mapping_offset    = load_le16((uint8_t *)&attributes.allocation->d.nres.mapping_pairs_off);
            int64_t  vcn[256];
            if (!attributes.allocation->non_resident || mapping_offset < 64 || mapping_offset >= allocation_length) {
                status = -EIO;
                goto unlock;
            }
            index_extents = ntfs_runlist_parse((uint8_t *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), vcn, index_lcn, index_length, 256);
            if (index_extents <= 0) {
                status = -EIO;
                goto unlock;
            }
        }
    }
    uint32_t bytes_in_use = load_le32(child_record + 0x18);
    uint32_t offset       = load_le16(child_record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(child_record + offset);
        uint32_t    type      = load_le32((uint8_t *)&attribute->type);
        uint32_t    length    = load_le32((uint8_t *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) {
            status = -EIO;
            goto unlock;
        }
        if (type == AT_DATA) {
            if (attribute->name_length) {
                status = -EOPNOTSUPP;
                goto unlock;
            }
            if (attribute->non_resident) {
                uint16_t flags          = load_le16((uint8_t *)&attribute->flags);
                uint16_t mapping_offset = load_le16((uint8_t *)&attribute->d.nres.mapping_pairs_off);
                int64_t  vcn[256];
                if (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE) || length < 64 || mapping_offset < 64 || mapping_offset >= length || data_extents) {
                    status = -EOPNOTSUPP;
                    goto unlock;
                }
                data_extents = ntfs_runlist_parse((uint8_t *)attribute + mapping_offset, (int)(length - mapping_offset), vcn, data_lcn, data_length, 256);
                if (data_extents <= 0) {
                    status = -EIO;
                    goto unlock;
                }
                for (int i = 0; i < data_extents; i++) {
                    if (data_lcn[i] < 0 || data_length[i] <= 0 || data_lcn[i] >= mnt->nr_clusters || data_length[i] > mnt->nr_clusters - data_lcn[i]) {
                        status = -EIO;
                        goto unlock;
                    }
                }
            }
        }
        offset += length;
    }
    status = ntfs_directory_index_remove(mnt, parent_record, child->mft_no, child->name, child->name_length);
    if (status < 0) goto unlock;
    status = ntfs_mft_write(mnt, parent->mft_no, parent_record);
    if (status < 0) goto unlock;
    namespace_committed = 1;
    status              = ntfs_index_reclaim_commit(mnt);
    if (status < 0) goto unlock;
    if (data_extents) {
        status = ntfs_bitmap_change_extents(mnt, data_lcn, data_length, data_extents, 0);
        if (status < 0) goto unlock;
    }
    if (index_extents) {
        status = ntfs_bitmap_change_extents(mnt, index_lcn, index_length, index_extents, 0);
        if (status < 0) {
            if (data_extents) ntfs_bitmap_change_extents(mnt, data_lcn, data_length, data_extents, 1);
            goto unlock;
        }
    }
    store_le16(child_record + 0x12, 0);
    store_le16(child_record + 0x16, load_le16(child_record + 0x16) & (uint16_t)~1U);
    status = ntfs_mft_write(mnt, child->mft_no, child_record);
    if (status < 0) goto unlock;
    status = ntfs_mft_bitmap_set(mnt, child->mft_no, 0);
unlock:
    if (status < 0 && !namespace_committed) ntfs_index_reclaim_abort(mnt);
    if (transaction_started) status = ntfs_transaction_finish(mnt, &transaction, status);
    spin_unlock(&mnt->write_lock);
    free(child_record);
    free(old_child_record);
    free(parent_record);
    return status;
}

/* Rename a file's directory entry and file-name attribute. */
static int ntfs_vfs_rename(const vfs_rename_context_t *context)
{
    ntfs_handle_t *child;
    const char    *nn;
    ntfs_mount_t  *mnt;
    uint8_t       *parent_record     = NULL;
    uint8_t       *old_parent_record = NULL;
    uint8_t       *child_record      = NULL;
    uint8_t       *old_record        = NULL;
    uint16_t      *new_name          = NULL;
    uint8_t        new_length;
    uint64_t       parent_reference;
    uint64_t       child_reference;
    int            namespace_committed = 0;
    int            status;
    fs_txn_t       transaction;
    int            transaction_started = 0;

    if (!context || !context->source || !context->new_name) return -EINVAL;
    if (context->old_parent != context->new_parent || context->target) return -EOPNOTSUPP;
    child = context->source->handle;
    nn    = context->new_name;
    if (!child || !child->mnt || !child->name || !child->name_length) return -EINVAL;
    mnt = child->mnt;
    if (!mnt->write_enabled || mnt->dev.read_only) return -EROFS;
    status = ntfs_name_from_utf8(nn, &new_name, &new_length);
    if (status < 0) return status;
    if (new_length == child->name_length) {
        int identical = 1;
        for (uint32_t index = 0; index < new_length; index++)
            if (new_name[index] != child->name[index]) identical = 0;
        if (identical) {
            free(new_name);
            return 0;
        }
    }
    parent_record     = malloc(mnt->mft_size);
    old_parent_record = malloc(mnt->mft_size);
    child_record      = malloc(mnt->mft_size);
    old_record        = malloc(mnt->mft_size);
    if (!parent_record || !old_parent_record || !child_record || !old_record) {
        status = -ENOMEM;
        goto out;
    }
    spin_lock(&mnt->write_lock);
    if (!mnt->dirty_owned) {
        status = ntfs_set_volume_dirty(mnt, 1);
        if (status < 0) goto unlock_rename;
        mnt->dirty_owned = 1;
    }
    status = ntfs_transaction_begin(mnt, &transaction, 65536);
    if (status < 0) goto unlock_rename;
    transaction_started = 1;
    if (ntfs_mft_read(mnt, child->parent_mft_no, parent_record) < 0 || ntfs_mft_read(mnt, child->mft_no, child_record) < 0) {
        status = -EIO;
        goto unlock_rename;
    }
    memcpy(old_record, child_record, mnt->mft_size);
    memcpy(old_parent_record, parent_record, mnt->mft_size);
    parent_reference = child->parent_mft_no | ((uint64_t)load_le16(parent_record + 0x10) << 48);
    child_reference  = child->mft_no | ((uint64_t)load_le16(child_record + 0x10) << 48);
    status           = ntfs_directory_index_remove(mnt, parent_record, child->mft_no, child->name, child->name_length);
    if (status < 0) goto unlock_rename;
    status = ntfs_directory_index_insert(mnt, parent_record, child_reference, parent_reference, new_name, new_length, child->file_attr, child->file_size, child->file_size);
    if (status < 0) {
        ntfs_directory_index_insert(mnt, old_parent_record, child_reference, parent_reference, child->name, child->name_length, child->file_attr, child->file_size, child->file_size);
        ntfs_index_reclaim_abort(mnt);
        goto unlock_rename;
    }
    status = ntfs_file_name_replace(mnt, child_record, parent_reference, child->name, child->name_length, new_name, new_length);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, child->mft_no, new_name, new_length);
        ntfs_directory_index_insert(mnt, old_parent_record, child_reference, parent_reference, child->name, child->name_length, child->file_attr, child->file_size, child->file_size);
        ntfs_index_reclaim_abort(mnt);
        goto unlock_rename;
    }
    status = ntfs_mft_write(mnt, child->mft_no, child_record);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, child->mft_no, new_name, new_length);
        ntfs_directory_index_insert(mnt, old_parent_record, child_reference, parent_reference, child->name, child->name_length, child->file_attr, child->file_size, child->file_size);
        ntfs_index_reclaim_abort(mnt);
        ntfs_mft_write(mnt, child->mft_no, old_record);
        goto unlock_rename;
    }
    status = ntfs_mft_write(mnt, child->parent_mft_no, parent_record);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, child->mft_no, new_name, new_length);
        ntfs_directory_index_insert(mnt, old_parent_record, child_reference, parent_reference, child->name, child->name_length, child->file_attr, child->file_size, child->file_size);
        ntfs_index_reclaim_abort(mnt);
        ntfs_mft_write(mnt, child->mft_no, old_record);
        goto unlock_rename;
    }
    namespace_committed = 1;
    status              = ntfs_index_reclaim_commit(mnt);
    if (status < 0) goto unlock_rename;
unlock_rename:
    if (status < 0 && !namespace_committed) ntfs_index_reclaim_abort(mnt);
    if (transaction_started) status = ntfs_transaction_finish(mnt, &transaction, status);
    spin_unlock(&mnt->write_lock);

    /* Publish the in-memory namespace only after the complete disk transaction is durable. */
    if (status == EOK) {
        free(child->name);
        child->name        = new_name;
        child->name_length = new_length;
        new_name           = NULL;
    }
out:
    free(new_name);
    free(old_record);
    free(child_record);
    free(old_parent_record);
    free(parent_record);
    return status;
}

static struct vfs_callback ntfs_cb = {
    .mount    = ntfs_vfs_mount,
    .unmount  = ntfs_vfs_unmount,
    .open     = ntfs_vfs_open,
    .close    = vfs_stub_close,
    .read     = ntfs_vfs_read,
    .write    = ntfs_vfs_write,
    .resize   = ntfs_vfs_resize,
    .readlink = ntfs_vfs_readlink,
    .mkdir    = ntfs_vfs_mkdir,
    .mkfile   = ntfs_vfs_mkfile,
    .link     = ntfs_vfs_link,
    .symlink  = ntfs_vfs_symlink,
    .stat     = ntfs_vfs_stat,
    .ioctl    = vfs_stub_ioctl_notty,
    .free     = ntfs_vfs_free,
    .delete   = ntfs_vfs_delete,
    .rename   = ntfs_vfs_rename,
};

/* Register the ntfs filesystem with the VFS layer. */
int ntfs_vfs_regist(void)
{
    int id = vfs_regist_fs("ntfs", &ntfs_cb);
    if (id & ERRNO_MASK) {
        plogk("ntfs: Failed to register filesystem (%d)\n", id);
        return -EINVAL;
    }
    plogk("ntfs: Filesystem registered (fsid=%d)\n", id);
    return 0;
}

#endif
