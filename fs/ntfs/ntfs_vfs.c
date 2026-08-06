/*
 *
 *      ntfs_vfs.c
 *      New Technology File System
 *
 *      2026/7/25 By MicroFish
 *      2026/7/26 By JiTianYu391: Added NTFS write support.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/blockdev.h>
#include <fs/core/fs_txn.h>
#include <fs/core/vfs.h>
#include <fs/ntfs/ntfs_vfs.h>
#include <fs/virtual/devtmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int64_t  s64;

#define magicNTFS 0x202020205346544eULL

#define AT_STANDARD_INFORMATION 0x10
#define AT_ATTRIBUTE_LIST       0x20
#define AT_FILE_NAME            0x30
#define AT_VOLUME_INFORMATION   0x70
#define AT_DATA                 0x80
#define AT_INDEX_ROOT           0x90
#define AT_INDEX_ALLOCATION     0xa0
#define AT_BITMAP               0xb0
#define AT_REPARSE_POINT        0xc0
#define AT_END                  0xffffffff
#define ATTR_COMPRESSED_MASK    0x00ff
#define ATTR_IS_COMPRESSED      0x0001
#define ATTR_IS_ENCRYPTED       0x4000
#define ATTR_IS_SPARSE          0x8000

#define MFT_MAGIC  0x454c4946
#define INDX_MAGIC 0x58444E49

#define INDEX_ENTRY_NODE 0x01
#define INDEX_ENTRY_END  0x02

#define IO_REPARSE_TAG_SYMLINK       0xa000000cU
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400U

#define LCN_HOLE ((s64) - 2)

/* on-disk layout structs —packed because NTFS has no natural alignment */
typedef struct ntfs_boot_sector {
        u8  jump[3];
        u64 oem_id;
        struct {
                u16 bytes_per_sector;
                u8  sectors_per_cluster;
                u16 reserved_sectors;
                u8  fats;
                u16 root_entries;
                u16 sectors;
                u8  media_type;
                u16 sectors_per_fat;
                u16 sectors_per_track;
                u16 heads;
                u32 hidden_sectors;
                u32 large_sectors;
        } __attribute__((packed)) bpb;
        u8                        unused[4];
        u64                       number_of_sectors;
        u64                       mft_lcn;
        u64                       mftmirr_lcn;
        s8                        clusters_per_mft_record;
        u8                        reserved0[3];
        s8                        clusters_per_index_record;
        u8                        reserved1[3];
        u64                       volume_serial_number;
        u32                       checksum;
        u8                        bootstrap[426];
        u16                       end_of_sector_marker;
} __attribute__((packed)) ntfs_boot_sector_t;

typedef struct mft_rec {
        u32 magic;
        u16 usa_ofs;
        u16 usa_count;
        u64 lsn;
        u16 seq;
        u16 link_count;
        u16 attrs;
        u16 flags;
        u32 bytes_in_use;
        u32 bytes_alloc;
        u64 base;
        u16 next_attr;
        u16 _resv;
        u32 mft_no;
} __attribute__((packed)) mft_rec_t;

typedef struct attr_rec {
        u32 type;
        u32 length;
        u8  non_resident;
        u8  name_length;
        u16 name_offset;
        u16 flags;
        u16 instance;
        union {
                struct {
                        u32 value_length;
                        u16 value_offset;
                        u8  _flags;
                        s8  _resv;
                } res;
                struct {
                        u64 lowest_vcn;
                        u64 highest_vcn;
                        u16 mapping_pairs_off;
                        u8  comp_unit;
                        u8  _r[5];
                        u64 alloc_size;
                        u64 data_size;
                        u64 init_size;
                        u64 compr_size;
                } nres;
        } d;
} __attribute__((packed)) attr_rec_t;

typedef struct fname_attr {
        u64 parent_dir;
        u64 crtime;
        u64 mtime_data;
        u64 mtime_mft;
        u64 atime;
        u64 alloc_size;
        u64 data_size;
        u32 fa;
        union {
                u32 ea_size;
                u32 rp_tag;
        };
        u8  name_len;
        u8  name_type;
        u16 name[];
} __attribute__((packed)) fname_attr_t;

typedef struct {
        blockdev_device_t dev;
        u32               cluster_size;
        u32               cluster_bits;
        u32               cluster_mask;
        u32               sector_size;
        u32               mft_size;
        u32               mft_bits;
        u32               indx_size;
        u32               indx_vcn_per_cluster;
        s64               mft_lcn;
        s64               mftmirr_lcn;
        s64               nr_clusters;
        u8               *mft_runlist;
        u32               mft_runlist_size;
        u64               mft_data_size;
        u8               *bitmap_runlist;
        u32               bitmap_runlist_size;
        u64               bitmap_data_size;
        u16              *upcase;
        u32               upcase_length;
        s64               index_reclaim_lcn[256];
        s64               index_reclaim_length[256];
        int               index_reclaim_count;
        spinlock_t        write_lock;
        int               write_enabled;
        int               dirty_owned;
        fs_txn_log_t      transaction_log;
        fs_txn_t         *active_transaction;
        int               transaction_log_initialized;
} ntfs_mount_t;

typedef struct {
        ntfs_mount_t *mnt;
        u64           mft_no;
        u64           parent_mft_no;
        u64           file_size;
        u32           file_attr;
        u16          *name;
        u8            name_length;
        int           is_dir;
        int           dir_loaded;
        u8           *runlist_buf;
        u32           runlist_sz;
        int           is_resident; /* 1 if runlist_buf holds inline resident data */
} ntfs_handle_t;

/* ---------- little-endian accessors ---------- */
static inline u64 le64(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24) | ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48)
           | ((u64)p[7] << 56);
}
static inline u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline u16 le16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

static inline void put_le16(u8 *p, u16 value)
{
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static inline void put_le32(u8 *p, u32 value)
{
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static inline void put_le64(u8 *p, u64 value)
{
    put_le32(p, (u32)value);
    put_le32(p + sizeof(u32), (u32)(value >> 32));
}

static u64 ntfs_current_filetime(void)
{
    return ((u64)timer_realtime_seconds32() + 11644473600ULL) * 10000000ULL;
}

static int ntfs_record_touch(u8 *record, u32 record_size, int data_changed)
{
    if (!record || record_size < 48 || le32(record) != MFT_MAGIC) return -EIO;
    u32 used   = le32(record + 0x18);
    u32 offset = le16(record + 0x14);
    u64 now    = ntfs_current_filetime();
    if (used > record_size || offset > used) return -EIO;
    while (offset + 24 <= used) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || length > used - offset) return -EIO;
        if (!attribute->non_resident && !attribute->name_length && (type == AT_STANDARD_INFORMATION || type == AT_FILE_NAME)) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            if (value_offset > length || value_length > length - value_offset) return -EIO;
            u8 *value = (u8 *)attribute + value_offset;
            if (type == AT_STANDARD_INFORMATION) {
                if (value_length < 32) return -EIO;
                if (data_changed) put_le64(value + 8, now);
                put_le64(value + 16, now);
            } else {
                if (value_length < sizeof(fname_attr_t)) return -EIO;
                fname_attr_t *file_name = (fname_attr_t *)value;
                if (data_changed) put_le64((u8 *)&file_name->mtime_data, now);
                put_le64((u8 *)&file_name->mtime_mft, now);
            }
        }
        offset += length;
    }
    return EOK;
}

static int ntfs_record_layout(ntfs_mount_t *mnt, u8 *record, u32 record_size, u16 *usa_offset, u16 *usa_count)
{
    u32 usa_end;

    if (!mnt || !record || !usa_offset || !usa_count || !mnt->sector_size || record_size < mnt->sector_size || record_size % mnt->sector_size)
        return -EINVAL;

    *usa_offset = le16(record + 4);
    *usa_count  = le16(record + 6);
    usa_end     = (u32)*usa_offset + (u32)*usa_count * sizeof(u16);
    if (*usa_offset < 8 || (*usa_offset & 1) || *usa_count != record_size / mnt->sector_size + 1 || usa_end > record_size
        || usa_end > mnt->sector_size - sizeof(u16))
        return -EIO;
    return 0;
}

static int ntfs_record_unpack(ntfs_mount_t *mnt, u8 *record, u32 record_size)
{
    u16 usa_offset;
    u16 usa_count;
    u16 sequence;
    int status;

    status = ntfs_record_layout(mnt, record, record_size, &usa_offset, &usa_count);
    if (status < 0) return status;
    sequence = le16(record + usa_offset);

    for (u16 i = 1; i < usa_count; i++) {
        u32 trailer = (size_t)i * mnt->sector_size - sizeof(u16);
        if (le16(record + trailer) != sequence) {
            plogk("ntfs: drive %u: record checksum mismatch (size %u)\n", mnt->dev.drive, record_size);
            return -EIO;
        }
    }
    for (u16 i = 1; i < usa_count; i++) {
        u32 trailer = (size_t)i * mnt->sector_size - sizeof(u16);
        put_le16(record + trailer, le16(record + usa_offset + (u32)i * sizeof(u16)));
    }
    return 0;
}

static int ntfs_record_pack(ntfs_mount_t *mnt, u8 *record, u32 record_size)
{
    u16 usa_offset;
    u16 usa_count;
    u16 sequence;
    int status;

    status = ntfs_record_layout(mnt, record, record_size, &usa_offset, &usa_count);
    if (status < 0) return status;

    sequence = (u16)(le16(record + usa_offset) + 1);
    if (!sequence) sequence = 1;
    put_le16(record + usa_offset, sequence);
    for (u16 i = 1; i < usa_count; i++) {
        u32 trailer = (size_t)i * mnt->sector_size - sizeof(u16);
        put_le16(record + usa_offset + (u32)i * sizeof(u16), le16(record + trailer));
        put_le16(record + trailer, sequence);
    }
    return 0;
}

/* ---------- utf16 →utf8 ---------- */
static u16 *utf16_from(const u8 *buf, int ofs, int len)
{
    if (len <= 0 || len > 255) return NULL;
    u16 *out = calloc(len + 1, sizeof(u16));
    if (!out) return NULL;
    for (int i = 0; i < len; i++) out[i] = le16(buf + ofs + (size_t)i * 2);
    return out;
}

static char *utf8_from_utf16(const u16 *u, int len)
{
    if (len <= 0 || !u) return strdup("");
    int out_len = 0;
    for (int i = 0; i < len && u[i]; i++) {
        u32 cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80)
            out_len++;
        else if (cp < 0x800)
            out_len += 2;
        else if (cp < 0x10000)
            out_len += 3;
        else
            out_len += 4;
    }
    char *out = calloc(out_len + 1, 1);
    if (!out) return NULL;
    int j = 0;
    for (int i = 0; i < len && u[i]; i++) {
        u32 cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            out[j++] = (char)cp;
        } else if (cp < 0x800) {
            out[j++] = (char)(0xC0 | (cp >> 6));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[j++] = (char)(0xE0 | (cp >> 12));
            out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[j++] = (char)(0xF0 | (cp >> 18));
            out[j++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

static int mft_read(ntfs_mount_t *mnt, u64 mft_no, u8 *buf);
static int mft_write(ntfs_mount_t *mnt, u64 mft_no, const u8 *buf);
static int dev_read(ntfs_mount_t *mnt, u64 byte_off, u8 *buf, size_t size);
static int dev_write(ntfs_mount_t *mnt, u64 byte_off, const u8 *buf, size_t size);
static int read_by_runlist(ntfs_mount_t *mnt, u8 *rl, int rl_len, u64 offset, u8 *buf, size_t size, u64 max_size);
static s64 write_by_runlist(ntfs_mount_t *mnt, u8 *rl, int rl_len, u64 offset, const u8 *buf, size_t size, u64 max_size);

static u32 align8(u32 value)
{
    return (value + 7) & ~7U;
}

static int ntfs_attr_name_is_i30(const attr_rec_t *attribute, u32 length)
{
    static const u16 i30[] = {'$', 'I', '3', '0'};
    u16              offset;

    if (!attribute || attribute->name_length != 4) return 0;
    offset = le16((const u8 *)&attribute->name_offset);
    if (offset > length || sizeof(i30) > length - offset) return 0;
    for (u32 i = 0; i < 4; i++)
        if (le16((const u8 *)attribute + offset + i * sizeof(u16)) != i30[i]) return 0;
    return 1;
}

static u16 ntfs_upcase_char(ntfs_mount_t *mnt, u16 value)
{
    if (mnt && mnt->upcase && value < mnt->upcase_length) return mnt->upcase[value];
    if (value >= 'a' && value <= 'z') return value - 'a' + 'A';
    return value;
}

static int ntfs_utf16_compare(ntfs_mount_t *mnt, const u16 *left, u8 left_length, const u16 *right, u8 right_length)
{
    u8 common = left_length < right_length ? left_length : right_length;

    for (u8 i = 0; i < common; i++) {
        u16 left_upcase  = ntfs_upcase_char(mnt, left[i]);
        u16 right_upcase = ntfs_upcase_char(mnt, right[i]);
        if (left_upcase < right_upcase) return -1;
        if (left_upcase > right_upcase) return 1;
    }
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
}

static int ntfs_utf16_from_utf8(const char *source, u16 **name, u8 *name_length, int filename)
{
    u16 *result;
    u32  count  = 0;
    u32  offset = 0;

    if (!source || !name || !name_length || !source[0]) return -EINVAL;
    result = calloc(255, sizeof(u16));
    if (!result) return -ENOMEM;
    while (source[offset]) {
        u32 codepoint;
        u8  first = (u8)source[offset++];
        u32 continuation;

        if (first < 0x80) {
            codepoint    = first;
            continuation = 0;
        } else if ((first & 0xe0) == 0xc0) {
            codepoint    = first & 0x1f;
            continuation = 1;
            if (codepoint < 2) goto invalid;
        } else if ((first & 0xf0) == 0xe0) {
            codepoint    = first & 0x0f;
            continuation = 2;
        } else if ((first & 0xf8) == 0xf0) {
            codepoint    = first & 0x07;
            continuation = 3;
        } else {
            goto invalid;
        }
        for (u32 i = 0; i < continuation; i++) {
            u8 byte = (u8)source[offset++];
            if (!byte || (byte & 0xc0) != 0x80) goto invalid;
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) || (continuation == 3 && codepoint < 0x10000) || codepoint > 0x10ffff
            || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            goto invalid;
        if (codepoint < 0x20
            || (filename
                && (codepoint == '"' || codepoint == '*' || codepoint == '/' || codepoint == ':' || codepoint == '<' || codepoint == '>'
                    || codepoint == '?' || codepoint == '\\' || codepoint == '|')))
            goto invalid;
        if (codepoint < 0x10000) {
            if (count == 255) goto too_long;
            result[count++] = (u16)codepoint;
        } else {
            if (count > 253) goto too_long;
            codepoint -= 0x10000;
            result[count++] = (u16)(0xd800 | (codepoint >> 10));
            result[count++] = (u16)(0xdc00 | (codepoint & 0x3ff));
        }
    }
    if (!count || (filename && (result[count - 1] == ' ' || result[count - 1] == '.'))) goto invalid;
    *name        = result;
    *name_length = (u8)count;
    return 0;

too_long:
    free(result);
    return -ENAMETOOLONG;
invalid:
    free(result);
    return -EINVAL;
}

static int ntfs_name_from_utf8(const char *source, u16 **name, u8 *name_length)
{
    return ntfs_utf16_from_utf8(source, name, name_length, 1);
}

static int ntfs_index_root_insert(ntfs_mount_t *mnt, u8 *record, u64 file_reference, u64 parent_reference, const u16 *name, u8 name_length,
                                  u32 file_attributes, u64 data_size, u64 allocated_size)
{
    attr_rec_t *attribute = NULL;
    u32         attribute_offset;
    u32         attribute_length = 0;
    u32         bytes_in_use;
    u16         value_offset;
    u32         value_length;
    u8         *value;
    u8         *header;
    u32         first_offset;
    u32         total_size;
    u32         allocation_size;
    u32         insert_offset = 0;
    u32         entry_size;
    u16         key_size;

    if (!mnt || !record || !name || !name_length || mnt->mft_size < sizeof(mft_rec_t)) return -EINVAL;
    if (le32(record) != MFT_MAGIC) return -EIO;
    bytes_in_use = le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size || bytes_in_use < sizeof(mft_rec_t)) return -EIO;

    attribute_offset = le16(record + 0x14);
    while (attribute_offset + 24 <= bytes_in_use) {
        attr_rec_t *current = (attr_rec_t *)(record + attribute_offset);
        u32         type    = le32((u8 *)&current->type);
        u32         length  = le32((u8 *)&current->length);

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

    value_offset = le16((u8 *)&attribute->d.res.value_offset);
    value_length = le32((u8 *)&attribute->d.res.value_length);
    if (value_offset > attribute_length || value_length < 32 || value_length > attribute_length - value_offset) return -EIO;
    value  = (u8 *)attribute + value_offset;
    header = value + 0x10;
    if (le32(value) != AT_FILE_NAME || le32(value + 4) != 1) return -EOPNOTSUPP;
    first_offset    = le32(header);
    total_size      = le32(header + 4);
    allocation_size = le32(header + 8);
    if (header[12] & INDEX_ENTRY_NODE) return -EOPNOTSUPP;
    if (first_offset < 16 || total_size < first_offset || allocation_size < total_size || total_size > value_length - 0x10
        || allocation_size > value_length - 0x10)
        return -EIO;

    for (u32 position = first_offset; position + 16 <= total_size;) {
        u8 *entry       = header + position;
        u16 length      = le16(entry + 8);
        u16 entry_key   = le16(entry + 10);
        u16 entry_flags = le16(entry + 12);

        if (length < 16 || length > total_size - position) return -EIO;
        if (entry_flags & INDEX_ENTRY_END) {
            if (entry_key) return -EIO;
            insert_offset = (u32)(entry - record);
            break;
        }
        if (entry_key < sizeof(fname_attr_t) || entry_key > length - 16) return -EIO;
        fname_attr_t *file_name  = (fname_attr_t *)(entry + 16);
        u32           name_bytes = (u32)file_name->name_len * sizeof(u16);
        if (name_bytes > entry_key - sizeof(fname_attr_t)) return -EIO;
        u16 existing[255];
        for (u32 i = 0; i < file_name->name_len; i++) existing[i] = le16((u8 *)file_name->name + i * sizeof(u16));
        int comparison = ntfs_utf16_compare(mnt, name, name_length, existing, file_name->name_len);
        if (!comparison) return -EEXIST;
        if (comparison < 0) {
            insert_offset = (u32)(entry - record);
            break;
        }
        position += length;
    }
    if (!insert_offset) return -EIO;

    key_size   = (u16)(sizeof(fname_attr_t) + (u32)name_length * sizeof(u16));
    entry_size = align8(16 + key_size);
    if (entry_size > mnt->mft_size - bytes_in_use || attribute_length > UINT32_MAX - entry_size || value_length > UINT32_MAX - entry_size
        || total_size > UINT32_MAX - entry_size || allocation_size > UINT32_MAX - entry_size)
        return -ENOSPC;

    memmove(record + insert_offset + entry_size, record + insert_offset, bytes_in_use - insert_offset);
    memset(record + insert_offset, 0, entry_size);
    put_le64(record + insert_offset, file_reference);
    put_le16(record + insert_offset + 8, (u16)entry_size);
    put_le16(record + insert_offset + 10, key_size);
    fname_attr_t *file_name = (fname_attr_t *)(record + insert_offset + 16);
    put_le64((u8 *)&file_name->parent_dir, parent_reference);
    put_le64((u8 *)&file_name->alloc_size, allocated_size);
    put_le64((u8 *)&file_name->data_size, data_size);
    put_le32((u8 *)&file_name->fa, file_attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (u32 i = 0; i < name_length; i++) put_le16((u8 *)file_name->name + i * sizeof(u16), name[i]);

    put_le32((u8 *)&attribute->length, attribute_length + entry_size);
    put_le32((u8 *)&attribute->d.res.value_length, value_length + entry_size);
    put_le32(header + 4, total_size + entry_size);
    put_le32(header + 8, allocation_size + entry_size);
    put_le32(record + 0x18, bytes_in_use + entry_size);
    return 0;
}

static int ntfs_index_root_remove(ntfs_mount_t *mnt, u8 *record, u64 file_reference, const u16 *name, u8 name_length)
{
    attr_rec_t *attribute = NULL;
    u32         offset;
    u32         attribute_length = 0;
    u32         bytes_in_use;
    u16         value_offset;
    u32         value_length;
    u8         *header;
    u32         first_offset;
    u32         total_size;
    u32         allocation_size;
    u32         remove_offset = 0;
    u16         remove_length = 0;

    if (!mnt || !record || !name || !name_length || le32(record) != MFT_MAGIC) return -EINVAL;
    bytes_in_use = le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size) return -EIO;
    offset = le16(record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *current = (attr_rec_t *)(record + offset);
        u32         type    = le32((u8 *)&current->type);
        u32         length  = le32((u8 *)&current->length);

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
    value_offset = le16((u8 *)&attribute->d.res.value_offset);
    value_length = le32((u8 *)&attribute->d.res.value_length);
    if (value_offset > attribute_length || value_length < 32 || value_length > attribute_length - value_offset) return -EIO;
    header          = (u8 *)attribute + value_offset + 0x10;
    first_offset    = le32(header);
    total_size      = le32(header + 4);
    allocation_size = le32(header + 8);
    if (header[12] & INDEX_ENTRY_NODE) return -EOPNOTSUPP;
    if (first_offset < 16 || total_size < first_offset || allocation_size < total_size || total_size > value_length - 0x10) return -EIO;

    for (u32 position = first_offset; position + 16 <= total_size;) {
        u8 *entry       = header + position;
        u16 length      = le16(entry + 8);
        u16 key_length  = le16(entry + 10);
        u16 entry_flags = le16(entry + 12);

        if (length < 16 || length > total_size - position) return -EIO;
        if (entry_flags & INDEX_ENTRY_END) break;
        if (key_length < sizeof(fname_attr_t) || key_length > length - 16) return -EIO;
        fname_attr_t *file_name  = (fname_attr_t *)(entry + 16);
        u32           name_bytes = (u32)file_name->name_len * sizeof(u16);
        if (name_bytes > key_length - sizeof(fname_attr_t)) return -EIO;
        if ((le64(entry) & 0x0000ffffffffffffULL) == (file_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length) {
            int equal = 1;
            for (u32 i = 0; i < name_length; i++) {
                if (le16((u8 *)file_name->name + i * sizeof(u16)) != name[i]) {
                    equal = 0;
                    break;
                }
            }
            if (equal) {
                remove_offset = (u32)(entry - record);
                remove_length = length;
            }
        }
        position += length;
    }
    if (!remove_offset) return -ENOENT;
    memmove(record + remove_offset, record + remove_offset + remove_length, bytes_in_use - remove_offset - remove_length);
    memset(record + bytes_in_use - remove_length, 0, remove_length);
    put_le32((u8 *)&attribute->length, attribute_length - remove_length);
    put_le32((u8 *)&attribute->d.res.value_length, value_length - remove_length);
    put_le32(header + 4, total_size - remove_length);
    put_le32(header + 8, allocation_size - remove_length);
    put_le32(record + 0x18, bytes_in_use - remove_length);
    return 0;
}

static int ntfs_file_name_replace(ntfs_mount_t *mnt, u8 *record, u64 parent_reference, const u16 *old_name, u8 old_length, const u16 *new_name,
                                  u8 new_length)
{
    u32 bytes_in_use;
    u32 offset;

    if (!mnt || !record || !old_name || !old_length || !new_name || !new_length || le32(record) != MFT_MAGIC) return -EINVAL;
    bytes_in_use = le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size) return -EIO;
    offset = le16(record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        if (type == AT_FILE_NAME && !attribute->non_resident && !attribute->name_length) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < sizeof(fname_attr_t) || value_length > length - value_offset) return -EIO;
            fname_attr_t *file_name = (fname_attr_t *)((u8 *)attribute + value_offset);
            if ((le64((u8 *)&file_name->parent_dir) & 0x0000ffffffffffffULL) == (parent_reference & 0x0000ffffffffffffULL)
                && file_name->name_len == old_length) {
                int equal = 1;
                for (u32 i = 0; i < old_length; i++) {
                    if (le16((u8 *)file_name->name + i * sizeof(u16)) != old_name[i]) {
                        equal = 0;
                        break;
                    }
                }
                if (equal) {
                    u32 new_value_length = sizeof(fname_attr_t) + (u32)new_length * sizeof(u16);
                    u32 new_attr_length  = align8(value_offset + new_value_length);
                    if (new_attr_length > length && new_attr_length - length > mnt->mft_size - bytes_in_use) return -ENOSPC;
                    if (new_attr_length != length) {
                        u32 tail_offset = offset + length;
                        memmove(record + offset + new_attr_length, record + tail_offset, bytes_in_use - tail_offset);
                        if (new_attr_length < length) memset(record + bytes_in_use - (length - new_attr_length), 0, length - new_attr_length);
                        bytes_in_use = bytes_in_use - length + new_attr_length;
                        put_le32(record + 0x18, bytes_in_use);
                        put_le32((u8 *)&attribute->length, new_attr_length);
                    }
                    file_name = (fname_attr_t *)((u8 *)attribute + value_offset);
                    memset((u8 *)file_name + sizeof(fname_attr_t), 0, new_attr_length - value_offset - sizeof(fname_attr_t));
                    file_name->name_len = new_length;
                    for (u32 i = 0; i < new_length; i++) put_le16((u8 *)file_name->name + i * sizeof(u16), new_name[i]);
                    put_le32((u8 *)&attribute->d.res.value_length, new_value_length);
                    return 0;
                }
            }
        }
        offset += length;
    }
    return -ENOENT;
}

static u32 ntfs_resident_attribute(u8 *record, u32 offset, u32 type, u16 instance, const u16 *attribute_name, u8 attribute_name_length,
                                   const u8 *value, u32 value_length);

static int ntfs_file_name_add(ntfs_mount_t *mnt, u8 *record, u64 parent_reference, const u16 *name, u8 name_length, u32 file_attributes,
                              u64 data_size)
{
    u8  value[sizeof(fname_attr_t) + 255 * sizeof(u16)];
    u32 bytes_in_use;
    u32 offset;
    u32 attr_length;

    if (!mnt || !record || !name || !name_length || le32(record) != MFT_MAGIC) return -EINVAL;
    bytes_in_use = le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size || bytes_in_use < 8) return -EIO;
    offset = le16(record + 0x14);
    while (offset + 8 <= bytes_in_use) {
        u32 type   = le32(record + offset);
        u32 length = le32(record + offset + 4);
        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        offset += length;
    }
    if (offset + 8 > bytes_in_use || le32(record + offset) != AT_END) return -EIO;
    attr_length = align8(24 + sizeof(fname_attr_t) + (u32)name_length * sizeof(u16));
    if (attr_length > mnt->mft_size - bytes_in_use) return -ENOSPC;
    memset(value, 0, sizeof(fname_attr_t) + (u32)name_length * sizeof(u16));
    fname_attr_t *file_name = (fname_attr_t *)value;
    put_le64((u8 *)&file_name->parent_dir, parent_reference);
    put_le64((u8 *)&file_name->data_size, data_size);
    put_le64((u8 *)&file_name->alloc_size, data_size);
    put_le32((u8 *)&file_name->fa, file_attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (u32 i = 0; i < name_length; i++) put_le16((u8 *)file_name->name + i * sizeof(u16), name[i]);
    u16 instance = le16(record + 0x28);
    u32 end
        = ntfs_resident_attribute(record, offset, AT_FILE_NAME, instance, NULL, 0, value, sizeof(fname_attr_t) + (u32)name_length * sizeof(u16));
    put_le32(record + end, AT_END);
    put_le32(record + 0x18, end + 8);
    put_le16(record + 0x28, instance + 1);
    return 0;
}

static int ntfs_file_name_remove(ntfs_mount_t *mnt, u8 *record, u64 parent_reference, const u16 *name, u8 name_length)
{
    u32 bytes_in_use;
    u32 offset;

    if (!mnt || !record || !name || !name_length || le32(record) != MFT_MAGIC) return -EINVAL;
    bytes_in_use = le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size) return -EIO;
    offset = le16(record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length || type == AT_ATTRIBUTE_LIST) return -EIO;
        if (type == AT_FILE_NAME && !attribute->non_resident && !attribute->name_length) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < sizeof(fname_attr_t) || value_length > length - value_offset) return -EIO;
            fname_attr_t *file_name = (fname_attr_t *)((u8 *)attribute + value_offset);
            if ((le64((u8 *)&file_name->parent_dir) & 0x0000ffffffffffffULL) == (parent_reference & 0x0000ffffffffffffULL)
                && file_name->name_len == name_length) {
                int equal = 1;
                for (u32 i = 0; i < name_length; i++)
                    if (le16((u8 *)file_name->name + i * sizeof(u16)) != name[i]) equal = 0;
                if (equal) {
                    memmove(record + offset, record + offset + length, bytes_in_use - offset - length);
                    memset(record + bytes_in_use - length, 0, length);
                    put_le32(record + 0x18, bytes_in_use - length);
                    return 0;
                }
            }
        }
        offset += length;
    }
    return -ENOENT;
}

static int ntfs_mft_bitmap_attribute(ntfs_mount_t *mnt, u8 *record, attr_rec_t **result)
{
    u32 offset;
    u32 bytes_in_use;

    if (mft_read(mnt, 0, record) < 0 || le32(record) != MFT_MAGIC) return -EIO;
    bytes_in_use = le32(record + 0x18);
    if (bytes_in_use > mnt->mft_size) return -EIO;
    offset = le16(record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);

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

static int ntfs_mft_bitmap_read_byte(ntfs_mount_t *mnt, attr_rec_t *attribute, u64 byte_offset, u8 *value)
{
    u32 length = le32((u8 *)&attribute->length);

    if (attribute->non_resident) {
        u16 mapping_offset = le16((u8 *)&attribute->d.nres.mapping_pairs_off);
        u64 data_size      = le64((u8 *)&attribute->d.nres.data_size);
        if (length < 64 || mapping_offset < 64 || mapping_offset >= length || byte_offset >= data_size) return -EIO;
        return read_by_runlist(mnt, (u8 *)attribute + mapping_offset, (int)(length - mapping_offset), byte_offset, value, 1, data_size) == 1 ?
                   0 :
                   -EIO;
    }
    u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
    u32 value_length = le32((u8 *)&attribute->d.res.value_length);
    if (value_offset > length || value_length > length - value_offset || byte_offset >= value_length) return -EIO;
    *value = *((u8 *)attribute + value_offset + byte_offset);
    return 0;
}

static int ntfs_mft_bitmap_find_free(ntfs_mount_t *mnt, u64 *record_number)
{
    u8         *record;
    attr_rec_t *attribute;
    u64         records;
    int         status;

    if (!mnt || !record_number || !mnt->mft_size) return -EINVAL;
    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    status = ntfs_mft_bitmap_attribute(mnt, record, &attribute);
    if (status < 0) goto out;
    records = mnt->mft_data_size / mnt->mft_size;
    for (u64 number = 16; number < records; number += 8) {
        u8 bitmap;
        status = ntfs_mft_bitmap_read_byte(mnt, attribute, number / 8, &bitmap);
        if (status < 0) goto out;
        for (u32 bit = 0; bit < 8 && number + bit < records; bit++) {
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

static int ntfs_mft_bitmap_set(ntfs_mount_t *mnt, u64 record_number, int allocated)
{
    u8         *record;
    attr_rec_t *attribute;
    u8          bitmap;
    int         status;

    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    status = ntfs_mft_bitmap_attribute(mnt, record, &attribute);
    if (status < 0) goto out;
    status = ntfs_mft_bitmap_read_byte(mnt, attribute, record_number / 8, &bitmap);
    if (status < 0) goto out;
    if (allocated)
        bitmap |= (u8)(1U << (record_number & 7));
    else
        bitmap &= (u8) ~(1U << (record_number & 7));
    if (attribute->non_resident) {
        u32 length         = le32((u8 *)&attribute->length);
        u16 mapping_offset = le16((u8 *)&attribute->d.nres.mapping_pairs_off);
        u64 data_size      = le64((u8 *)&attribute->d.nres.data_size);
        status
            = write_by_runlist(mnt, (u8 *)attribute + mapping_offset, (int)(length - mapping_offset), record_number / 8, &bitmap, 1, data_size)
                      == 1 ?
                  0 :
                  -EIO;
    } else {
        u16 value_offset                                      = le16((u8 *)&attribute->d.res.value_offset);
        *((u8 *)attribute + value_offset + record_number / 8) = bitmap;
        status                                                = mft_write(mnt, 0, record);
    }
out:
    free(record);
    return status;
}

static u32 ntfs_resident_attribute(u8 *record, u32 offset, u32 type, u16 instance, const u16 *attribute_name, u8 attribute_name_length,
                                   const u8 *value, u32 value_length)
{
    u16 value_offset = (u16)align8(24 + (u32)attribute_name_length * sizeof(u16));
    u32 length       = align8(value_offset + value_length);

    memset(record + offset, 0, length);
    put_le32(record + offset, type);
    put_le32(record + offset + 4, length);
    record[offset + 9] = attribute_name_length;
    put_le16(record + offset + 10, 24);
    put_le16(record + offset + 14, instance);
    put_le32(record + offset + 16, value_length);
    put_le16(record + offset + 20, value_offset);
    for (u32 i = 0; i < attribute_name_length; i++) put_le16(record + offset + 24 + i * sizeof(u16), attribute_name[i]);
    if (value_length) memcpy(record + offset + value_offset, value, value_length);
    return offset + length;
}

static int ntfs_build_file_record(ntfs_mount_t *mnt, u8 *record, u64 record_number, u16 sequence, u64 parent_reference, const u16 *name,
                                  u8 name_length, int directory)
{
    static const u16 i30[]        = {'$', 'I', '3', '0'};
    u8               standard[48] = {0};
    u8               file_name_value[sizeof(fname_attr_t) + 255 * sizeof(u16)];
    u8               index_root[48] = {0};
    u32              offset;
    u32              attributes = directory ? 0x10 : 0x20;

    if (!mnt || !record || !name || !name_length) return -EINVAL;
    memset(record, 0, mnt->mft_size);
    put_le32(record, MFT_MAGIC);
    put_le16(record + 4, 0x30);
    put_le16(record + 6, (u16)(mnt->mft_size / mnt->sector_size + 1));
    put_le16(record + 0x10, sequence ? sequence : 1);
    put_le16(record + 0x12, 1);
    put_le16(record + 0x14, 0x38);
    put_le16(record + 0x16, directory ? 3 : 1);
    put_le32(record + 0x1c, mnt->mft_size);
    put_le16(record + 0x28, directory ? 3 : 4);
    put_le32(record + 0x2c, (u32)record_number);
    u64 now = ntfs_current_filetime();
    put_le64(standard, now);
    put_le64(standard + 8, now);
    put_le64(standard + 16, now);
    put_le64(standard + 24, now);
    put_le32(standard + 32, attributes);

    offset = ntfs_resident_attribute(record, 0x38, AT_STANDARD_INFORMATION, 0, NULL, 0, standard, sizeof(standard));
    memset(file_name_value, 0, sizeof(fname_attr_t) + (u32)name_length * sizeof(u16));
    fname_attr_t *file_name = (fname_attr_t *)file_name_value;
    put_le64((u8 *)&file_name->parent_dir, parent_reference);
    put_le64((u8 *)&file_name->crtime, now);
    put_le64((u8 *)&file_name->mtime_data, now);
    put_le64((u8 *)&file_name->mtime_mft, now);
    put_le64((u8 *)&file_name->atime, now);
    put_le32((u8 *)&file_name->fa, attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (u32 i = 0; i < name_length; i++) put_le16((u8 *)file_name->name + i * sizeof(u16), name[i]);
    offset = ntfs_resident_attribute(record, offset, AT_FILE_NAME, 1, NULL, 0, file_name_value,
                                     sizeof(fname_attr_t) + (u32)name_length * sizeof(u16));
    if (directory) {
        put_le32(index_root, AT_FILE_NAME);
        put_le32(index_root + 4, 1);
        put_le32(index_root + 8, mnt->indx_size);
        index_root[12] = 1;
        put_le32(index_root + 16, 0x10);
        put_le32(index_root + 20, 0x20);
        put_le32(index_root + 24, 0x20);
        put_le16(index_root + 32 + 8, 16);
        put_le16(index_root + 32 + 12, INDEX_ENTRY_END);
        offset = ntfs_resident_attribute(record, offset, AT_INDEX_ROOT, 2, i30, 4, index_root, sizeof(index_root));
    } else {
        offset = ntfs_resident_attribute(record, offset, AT_DATA, 2, NULL, 0, NULL, 0);
    }
    if (offset > mnt->mft_size - 8) return -ENOSPC;
    put_le32(record + offset, AT_END);
    put_le32(record + 0x18, offset + 8);
    return 0;
}

static int ntfs_build_symlink_record(ntfs_mount_t *mnt, u8 *record, u64 record_number, u16 sequence, u64 parent_reference, const u16 *name,
                                     u8 name_length, const u16 *target, u8 target_length, int relative)
{
    u8  reparse[20 + 255 * 4];
    u32 reparse_size = 20 + (u32)target_length * 4;
    u32 offset;
    int status;

    status = ntfs_build_file_record(mnt, record, record_number, sequence, parent_reference, name, name_length, 0);
    if (status < 0) return status;
    offset = le16(record + 0x14);
    while (offset + 24 <= le32(record + 0x18)) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > le32(record + 0x18) - length) return -EIO;
        if (!attribute->non_resident && !attribute->name_length && (type == AT_STANDARD_INFORMATION || type == AT_FILE_NAME)) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            if (value_offset > length || value_length > length - value_offset) return -EIO;
            if (type == AT_STANDARD_INFORMATION) {
                if (value_length < 36) return -EIO;
                put_le32((u8 *)attribute + value_offset + 32, le32((u8 *)attribute + value_offset + 32) | FILE_ATTRIBUTE_REPARSE_POINT);
            } else {
                if (value_length < sizeof(fname_attr_t)) return -EIO;
                fname_attr_t *file_name = (fname_attr_t *)((u8 *)attribute + value_offset);
                put_le32((u8 *)&file_name->fa, le32((u8 *)&file_name->fa) | FILE_ATTRIBUTE_REPARSE_POINT);
                put_le32((u8 *)&file_name->rp_tag, IO_REPARSE_TAG_SYMLINK);
            }
        }
        offset += length;
    }
    if (offset + 8 > le32(record + 0x18) || le32(record + offset) != AT_END) return -EIO;
    memset(reparse, 0, reparse_size);
    put_le32(reparse, IO_REPARSE_TAG_SYMLINK);
    put_le16(reparse + 4, (u16)(12 + (u32)target_length * 4));
    put_le16(reparse + 10, (u16)((u32)target_length * 2));
    put_le16(reparse + 12, (u16)((u32)target_length * 2));
    put_le16(reparse + 14, (u16)((u32)target_length * 2));
    put_le32(reparse + 16, relative ? 1 : 0);
    for (u32 i = 0; i < target_length; i++) {
        put_le16(reparse + 20 + (size_t)i * 2, target[i]);
        put_le16(reparse + 20 + (size_t)((u32)target_length + i) * 2, target[i]);
    }
    u32 attribute_length = align8(24 + reparse_size);
    if (attribute_length > mnt->mft_size - offset - 8) return -ENOSPC;
    u16 instance = le16(record + 0x28);
    u32 end      = ntfs_resident_attribute(record, offset, AT_REPARSE_POINT, instance, NULL, 0, reparse, reparse_size);
    if (end > mnt->mft_size - 8) return -ENOSPC;
    put_le32(record + end, AT_END);
    put_le32(record + 0x18, end + 8);
    put_le16(record + 0x28, instance + 1);
    return 0;
}

static int ntfs_index_root_set_reparse_tag(ntfs_mount_t *mnt, u8 *record, u64 file_reference, const u16 *name, u8 name_length)
{
    u32 bytes_in_use = le32(record + 0x18);
    u32 offset       = le16(record + 0x14);
    if (!mnt || !record || bytes_in_use > mnt->mft_size) return -EINVAL;
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > bytes_in_use - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length) && !attribute->non_resident) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < 32 || value_length > length - value_offset) return -EIO;
            u8 *header = (u8 *)attribute + value_offset + 0x10;
            u32 pos    = le32(header);
            u32 total  = le32(header + 4);
            while (pos + 16 <= total) {
                u8 *entry  = header + pos;
                u16 elen   = le16(entry + 8);
                u16 keylen = le16(entry + 10);
                u16 flags  = le16(entry + 12);
                if (elen < 16 || elen > total - pos) return -EIO;
                if (flags & INDEX_ENTRY_END) break;
                if (keylen < sizeof(fname_attr_t) || keylen > elen - 16) return -EIO;
                fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
                if ((le64(entry) & 0x0000ffffffffffffULL) == (file_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length) {
                    int equal = 1;
                    for (u32 i = 0; i < name_length; i++)
                        if (le16((u8 *)file_name->name + (size_t)i * 2) != name[i]) equal = 0;
                    if (equal) {
                        put_le32((u8 *)&file_name->rp_tag, IO_REPARSE_TAG_SYMLINK);
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

static int read_by_runlist(ntfs_mount_t *mnt, u8 *rl, int rl_len, u64 offset, u8 *buf, size_t size, u64 max_size);
static s64 write_by_runlist(ntfs_mount_t *mnt, u8 *rl, int rl_len, u64 offset, const u8 *buf, size_t size, u64 max_size);
static int runlist_parse(u8 *rl, int len, s64 *vcn, s64 *lcn, s64 *run, int max);

static int mft_bootstrap_runlist(ntfs_mount_t *mnt)
{
    u8 *record;
    u32 offset;
    int status = -EIO;

    if (!mnt || mnt->mft_runlist || !mnt->mft_size || mnt->mft_lcn < 0) return mnt && mnt->mft_runlist ? 0 : -EINVAL;
    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (dev_read(mnt, (u64)mnt->mft_lcn << mnt->cluster_bits, record, mnt->mft_size) < 0 || ntfs_record_unpack(mnt, record, mnt->mft_size) < 0
        || le32(record) != MFT_MAGIC)
        goto out;

    offset = le16(record + 0x14);
    while (offset + 16 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);

        if (type == AT_END) break;
        if (length < 16 || offset + length > mnt->mft_size) goto out;
        if (type == AT_ATTRIBUTE_LIST) goto out;
        if (type == AT_DATA && !attribute->name_length) {
            u16 mapping_offset;
            u16 flags;
            u32 runlist_size;
            u8 *runlist;
            s64 vcn[256], lcn[256], run[256];
            int run_count;
            u64 data_size;

            if (!attribute->non_resident || length < 64 || le64((u8 *)&attribute->d.nres.lowest_vcn) != 0) goto out;
            flags = le16((u8 *)&attribute->flags);
            if (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) goto out;
            mapping_offset = le16((u8 *)&attribute->d.nres.mapping_pairs_off);
            if (mapping_offset < 64 || mapping_offset >= length) goto out;
            runlist_size = length - mapping_offset;
            runlist      = malloc(runlist_size);
            if (!runlist) {
                status = -ENOMEM;
                goto out;
            }
            memcpy(runlist, (u8 *)attribute + mapping_offset, runlist_size);
            run_count = runlist_parse(runlist, (int)runlist_size, vcn, lcn, run, 256);
            data_size = le64((u8 *)&attribute->d.nres.data_size);
            if (run_count <= 0 || data_size < mnt->mft_size
                || (u64)run[run_count - 1] + (u64)vcn[run_count - 1] < (data_size + mnt->cluster_size - 1) / mnt->cluster_size) {
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
    if (status != EOK && status != -ENOMEM) plogk("ntfs: drive %u: MFT data attribute parse failed\n", mnt->dev.drive);
    free(record);
    return status;
}

/* ---------- disk I/O helpers ---------- */
static int mft_read(ntfs_mount_t *mnt, u64 mft_no, u8 *buf)
{
    u64 logical_offset;

    if (!mnt || !buf || !mnt->cluster_size || mft_no > UINT64_MAX / mnt->mft_size) return -EIO;
    if (mft_no && !mnt->mft_runlist && mft_bootstrap_runlist(mnt) < 0) return -EIO;
    logical_offset = mft_no * mnt->mft_size;
    if (mnt->mft_runlist) {
        if (read_by_runlist(mnt, mnt->mft_runlist, (int)mnt->mft_runlist_size, logical_offset, buf, mnt->mft_size, mnt->mft_data_size)
            != (int)mnt->mft_size)
            return -EIO;
    } else {
        u64 byte_offset = ((u64)mnt->mft_lcn << mnt->cluster_bits) + logical_offset;
        if (dev_read(mnt, byte_offset, buf, mnt->mft_size) < 0) {
            plogk("ntfs: drive %u: failed to read MFT record %llu at byte %llu\n", mnt->dev.drive, (unsigned long long)mft_no,
                  (unsigned long long)byte_offset);
            return -EIO;
        }
    }
    return ntfs_record_unpack(mnt, buf, mnt->mft_size);
}

static int mft_write(ntfs_mount_t *mnt, u64 mft_no, const u8 *buf)
{
    u64 byte_off;
    u8 *record;
    int status;

    if (!mnt || !buf || !mnt->cluster_size || mnt->dev.read_only) return -EROFS;
    if (!mnt->mft_runlist && mft_bootstrap_runlist(mnt) < 0) return -EIO;
    if (mft_no > UINT64_MAX / mnt->mft_size) return -EOVERFLOW;
    byte_off = mft_no * mnt->mft_size;

    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    memcpy(record, buf, mnt->mft_size);
    status = ntfs_record_pack(mnt, record, mnt->mft_size);
    if (status == 0) {
        if (mnt->mft_runlist) {
            if (write_by_runlist(mnt, mnt->mft_runlist, (int)mnt->mft_runlist_size, byte_off, record, mnt->mft_size, mnt->mft_data_size)
                != (s64)mnt->mft_size)
                status = -EIO;
        } else {
            byte_off += (u64)mnt->mft_lcn << mnt->cluster_bits;
            if (dev_write(mnt, byte_off, record, mnt->mft_size) < 0) status = -EIO;
        }
        if (status == 0 && mnt->mftmirr_lcn > 0) {
            u64 mirror_records = mnt->cluster_size / mnt->mft_size;
            if (mirror_records < 4) mirror_records = 4;
            if (mft_no < mirror_records
                && dev_write(mnt, ((u64)mnt->mftmirr_lcn << mnt->cluster_bits) + mft_no * mnt->mft_size, record, mnt->mft_size) < 0)
                status = -EIO;
        }
    }
    if (status != EOK) plogk("ntfs: drive %u: MFT record %llu write failed: %d\n", mnt->dev.drive, (unsigned long long)mft_no, status);
    free(record);
    return status;
}

static int ntfs_load_bitmap_runlist(ntfs_mount_t *mnt)
{
    u8 *record;
    u32 offset;
    int status = -EIO;

    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (mft_read(mnt, 6, record) < 0 || le32(record) != MFT_MAGIC) goto out;

    offset = le16(record + 0x14);
    while (offset + 16 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);

        if (type == AT_END) break;
        if (length < 16 || offset + length > mnt->mft_size || type == AT_ATTRIBUTE_LIST) goto out;
        if (type == AT_DATA && !attribute->name_length) {
            u16 mapping_offset;
            u16 flags;
            u64 data_size;
            u8 *runlist;
            s64 vcn[256], lcn[256], run[256];

            if (!attribute->non_resident || length < 64) goto out;
            flags = le16((u8 *)&attribute->flags);
            if (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) goto out;
            mapping_offset = le16((u8 *)&attribute->d.nres.mapping_pairs_off);
            data_size      = le64((u8 *)&attribute->d.nres.data_size);
            if (mapping_offset < 64 || mapping_offset >= length || data_size < ((u64)mnt->nr_clusters + 7) / 8) goto out;
            runlist = malloc(length - mapping_offset);
            if (!runlist) {
                status = -ENOMEM;
                goto out;
            }
            memcpy(runlist, (u8 *)attribute + mapping_offset, length - mapping_offset);
            if (runlist_parse(runlist, (int)(length - mapping_offset), vcn, lcn, run, 256) <= 0) {
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

static int ntfs_load_upcase(ntfs_mount_t *mnt)
{
    u8 *record;
    u8 *raw = NULL;
    u32 offset;
    int status = -EIO;

    if (!mnt) return -EINVAL;
    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (mft_read(mnt, 10, record) < 0 || le32(record) != MFT_MAGIC) goto out;
    offset = le16(record + 0x14);
    while (offset + 24 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > mnt->mft_size - length || type == AT_ATTRIBUTE_LIST) goto out;
        if (type == AT_DATA && !attribute->name_length) {
            const u32 table_size = 65536 * sizeof(u16);
            raw                  = malloc(table_size);
            if (!raw) {
                status = -ENOMEM;
                goto out;
            }
            if (attribute->non_resident) {
                u16 mapping_offset = le16((u8 *)&attribute->d.nres.mapping_pairs_off);
                u64 data_size      = le64((u8 *)&attribute->d.nres.data_size);
                u16 flags          = le16((u8 *)&attribute->flags);
                if (length < 64 || mapping_offset < 64 || mapping_offset >= length || data_size != table_size
                    || (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE))
                    || read_by_runlist(mnt, (u8 *)attribute + mapping_offset, (int)(length - mapping_offset), 0, raw, table_size, data_size)
                           != (int)table_size)
                    goto out;
            } else {
                u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
                u32 value_length = le32((u8 *)&attribute->d.res.value_length);
                if (value_offset > length || value_length != table_size || value_length > length - value_offset) goto out;
                memcpy(raw, (u8 *)attribute + value_offset, table_size);
            }
            mnt->upcase = malloc(table_size);
            if (!mnt->upcase) {
                status = -ENOMEM;
                goto out;
            }
            for (u32 i = 0; i < 65536; i++) mnt->upcase[i] = le16(raw + i * sizeof(u16));
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

static int ntfs_prepare_write(ntfs_mount_t *mnt)
{
    u8 *record;
    u32 offset;
    int status = -EROFS;

    if (!mnt || mnt->dev.read_only || mft_bootstrap_runlist(mnt) < 0) return -EROFS;
    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (mft_read(mnt, 3, record) < 0 || le32(record) != MFT_MAGIC) goto out;

    offset = le16(record + 0x14);
    while (offset + 24 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || offset + length > mnt->mft_size || type == AT_ATTRIBUTE_LIST) goto out;
        if (type == AT_VOLUME_INFORMATION && !attribute->name_length && !attribute->non_resident) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            u8 *value;

            if (value_offset > length || value_length < 12 || value_length > length - value_offset) goto out;
            value = (u8 *)attribute + value_offset;
            if (value[8] != 3 || le16(value + 10) != 0) goto out;
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

static int ntfs_set_volume_dirty(ntfs_mount_t *mnt, int dirty)
{
    u8 *record;
    u32 offset;
    int status = -EIO;

    record = malloc(mnt->mft_size);
    if (!record) return -ENOMEM;
    if (mft_read(mnt, 3, record) < 0 || le32(record) != MFT_MAGIC) goto out;
    offset = le16(record + 0x14);
    while (offset + 24 <= mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || offset + length > mnt->mft_size) goto out;
        if (type == AT_VOLUME_INFORMATION && !attribute->name_length && !attribute->non_resident) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            u16 volume_flags;

            if (value_offset > length || value_length < 12 || value_length > length - value_offset) goto out;
            volume_flags = le16((u8 *)attribute + value_offset + 10);
            if (dirty)
                volume_flags |= 1;
            else
                volume_flags &= (u16)~1U;
            put_le16((u8 *)attribute + value_offset + 10, volume_flags);
            status = mft_write(mnt, 3, record);
            if (status == EOK) status = blockdev_flush(&mnt->dev);
            goto out;
        }
        offset += length;
    }

out:
    free(record);
    return status;
}

static int ntfs_clear_owned_dirty(ntfs_mount_t *mnt)
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

static int ntfs_transaction_begin(ntfs_mount_t *mnt, fs_txn_t *transaction, u32 credits)
{
    int status;
    if (!mnt || !transaction || !credits || mnt->active_transaction || !mnt->transaction_log_initialized) return -EINVAL;
    status = fs_txn_begin(&mnt->transaction_log, credits, transaction);
    if (status == EOK) {
        mnt->active_transaction = transaction;
    } else if (mnt->dirty_owned) {
        (void)ntfs_clear_owned_dirty(mnt);
    }
    return status;
}

static int ntfs_transaction_finish(ntfs_mount_t *mnt, fs_txn_t *transaction, int status)
{
    if (!mnt || !transaction || mnt->active_transaction != transaction) return -EINVAL;
    mnt->active_transaction = NULL;
    if (status != EOK) {
        fs_txn_abort(transaction, status);
        (void)ntfs_clear_owned_dirty(mnt);
        return status;
    }
    status = fs_txn_commit(transaction);
    if (status == EOK) {
        status = ntfs_clear_owned_dirty(mnt);
    } else {
        /* A failed home-write may have left partially updated metadata. */
        mnt->write_enabled = 0;
    }
    return status;
}

static int dev_read(ntfs_mount_t *mnt, u64 byte_off, u8 *buf, size_t size)
{
    if (!mnt || !buf || byte_off > UINT64_MAX - size) return -EINVAL;
    if (!mnt->active_transaction) return blockdev_read_bytes(&mnt->dev, byte_off, buf, size);
    return fs_txn_read_bytes(mnt->active_transaction, byte_off, buf, size);
}

static int dev_write(ntfs_mount_t *mnt, u64 byte_off, const u8 *buf, size_t size)
{
    if (!mnt || !buf || mnt->dev.read_only || byte_off > UINT64_MAX - size) return mnt && mnt->dev.read_only ? -EROFS : -EINVAL;
    if (!mnt->active_transaction) return blockdev_write_bytes(&mnt->dev, byte_off, buf, size);
    return fs_txn_stage_bytes(mnt->active_transaction, byte_off, buf, size, FS_TXN_METADATA);
}

/* ---------- runlist parser ---------- */
static int runlist_parse(u8 *rl, int len, s64 *vcn, s64 *lcn, s64 *run, int max)
{
    int       off = 0, cnt = 0;
    s64       cur_vcn = 0, cur_lcn = 0;
    const s64 s64_max = (s64)(UINT64_MAX >> 1);
    const s64 s64_min = -s64_max - 1;

    if (!rl || len <= 0 || !vcn || !lcn || !run || max <= 0) return -EINVAL;
    while (off < len && cnt < max) {
        if (rl[off] == 0) return cnt;
        u8  h  = rl[off++];
        int lb = h & 0x0F, ob = (h >> 4) & 0x0F;
        if (lb == 0 || lb > 8 || ob > 8 || off + lb + ob > len) return -EIO;

        u64 raw_length = 0;
        for (int i = 0; i < lb; i++) raw_length |= (u64)rl[off++] << (i * 8);
        if (!raw_length || raw_length > (u64)s64_max || cur_vcn > s64_max - (s64)raw_length) return -EIO;
        s64 rlen = (s64)raw_length;

        if (ob == 0) {
            /* sparse / hole run: no LCN delta */
            vcn[cnt] = cur_vcn;
            lcn[cnt] = LCN_HOLE;
            run[cnt] = rlen;
        } else {
            u64 raw_offset = 0;
            for (int i = 0; i < ob; i++) raw_offset |= (u64)rl[off++] << (i * 8);
            s64 roff = (s64)raw_offset;
            if (ob < 8 && (rl[off - 1] & 0x80)) {
                u64 mask = (1ULL << (ob * 8)) - 1;
                roff     = (s64)(raw_offset | ~mask);
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

static int runlist_signed_bytes(s64 value)
{
    for (int bytes = 1; bytes < 8; bytes++) {
        s64 limit = (s64)1 << (bytes * 8 - 1);
        if (value >= -limit && value < limit) return bytes;
    }
    return 8;
}

static int runlist_unsigned_bytes(u64 value)
{
    int bytes = 1;
    while (bytes < 8 && value >= (1ULL << (bytes * 8))) bytes++;
    return bytes;
}

static int runlist_encode(const s64 *lcn, const s64 *run, int count, u8 *out, u32 capacity)
{
    s64 previous_lcn = 0;
    u32 offset       = 0;

    if (!lcn || !run || count <= 0 || !out) return -EINVAL;
    for (int i = 0; i < count; i++) {
        s64 delta;
        int length_bytes;
        int offset_bytes;

        if (lcn[i] < 0 || run[i] <= 0) return -EINVAL;
        delta        = lcn[i] - previous_lcn;
        length_bytes = runlist_unsigned_bytes((u64)run[i]);
        offset_bytes = runlist_signed_bytes(delta);
        if (capacity - offset < (u32)(1 + length_bytes + offset_bytes)) return -ENOSPC;
        out[offset++] = (u8)((offset_bytes << 4) | length_bytes);
        for (int byte = 0; byte < length_bytes; byte++) out[offset++] = (u8)((u64)run[i] >> (byte * 8));
        for (int byte = 0; byte < offset_bytes; byte++) out[offset++] = (u8)((u64)delta >> (byte * 8));
        previous_lcn = lcn[i];
    }
    if (offset >= capacity) return -ENOSPC;
    out[offset++] = 0;
    return (int)offset;
}

static int read_by_runlist(ntfs_mount_t *mnt, u8 *rl, int rl_len, u64 offset, u8 *buf, size_t size, u64 max_size)
{
    s64 v[256], l[256], r[256];
    int n = runlist_parse(rl, rl_len, v, l, r, 256);
    if (n <= 0) {
        plogk("ntfs: drive %u: corrupt runlist in read path\n", mnt->dev.drive);
        return 0;
    }
    if (!size || offset >= max_size) return 0;

    size_t done = 0;
    while (done < size && offset + done < max_size) {
        s64 pos       = (s64)(offset + done);
        s64 clu       = pos >> mnt->cluster_bits;
        s64 found_lcn = -1, hole_end = 0;
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (clu >= v[i] && clu < v[i] + r[i]) {
                if (l[i] >= 0)
                    found_lcn = l[i] + (clu - v[i]);
                else
                    hole_end = (v[i] + r[i]) << mnt->cluster_bits;
                break;
            }
            if (clu < v[i] && !found) {
                hole_end = v[i] << mnt->cluster_bits;
                found    = 1;
            }
        }
        if (found_lcn < 0) {
            s64 h_end   = hole_end ? hole_end : (s64)max_size;
            s64 remain  = (s64)(size - done);
            s64 to_fill = h_end - pos;
            if (to_fill > remain) to_fill = remain;
            if (to_fill <= 0) break;
            if (pos + to_fill > (s64)max_size) to_fill = (s64)max_size - pos;
            size_t fill = (size_t)to_fill;
            memset(buf + done, 0, fill);
            done += fill;
        } else {
            s64 cl_off  = pos & mnt->cluster_mask;
            s64 to_read = (s64)(size - done);
            s64 max_cl  = (s64)mnt->cluster_size - cl_off;
            if (to_read > max_cl) to_read = max_cl;
            s64 byte_off = (found_lcn << mnt->cluster_bits) + cl_off;
            if (dev_read(mnt, (u64)byte_off, buf + done, (size_t)to_read) < 0) {
                plogk("ntfs: drive %u: block read failed at byte %llu (%llu bytes)\n", mnt->dev.drive, (unsigned long long)byte_off,
                      (unsigned long long)(size - done));
                return done > 0 ? (int)done : -EIO;
            }
            done += (size_t)to_read;
        }
    }
    return (int)done;
}

static s64 write_by_runlist(ntfs_mount_t *mnt, u8 *rl, int rl_len, u64 offset, const u8 *buf, size_t size, u64 max_size)
{
    s64    v[256], l[256], r[256];
    int    n;
    size_t done = 0;

    if (!mnt || !buf || !size || offset > max_size || size > max_size - offset) return -EINVAL;
    n = runlist_parse(rl, rl_len, v, l, r, 256);
    if (n <= 0) {
        plogk("ntfs: drive %u: corrupt runlist in write path\n", mnt->dev.drive);
        return -EIO;
    }

    while (done < size) {
        u64 position = offset + done;
        s64 cluster  = (s64)(position >> mnt->cluster_bits);
        s64 run_lcn  = -1;
        s64 run_end  = -1;

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
            plogk("ntfs: drive %u: write target LCN %lld out of range (nr_clusters=%llu)\n", mnt->dev.drive, (long long)run_lcn,
                  (unsigned long long)mnt->nr_clusters);
            return done ? (s64)done : -EIO;
        }

        size_t cluster_offset = (size_t)(position & mnt->cluster_mask);
        size_t chunk          = (size_t)(run_end - (s64)position);
        if (chunk > size - done) chunk = size - done;
        if ((u64)run_lcn + (cluster_offset + chunk + mnt->cluster_size - 1) / mnt->cluster_size > (u64)mnt->nr_clusters) {
            plogk("ntfs: drive %u: write overruns volume end at LCN %lld\n", mnt->dev.drive, (long long)run_lcn);
            return done ? (s64)done : -EIO;
        }
        if (dev_write(mnt, ((u64)run_lcn << mnt->cluster_bits) + cluster_offset, buf + done, chunk) < 0) {
            plogk("ntfs: drive %u: block write failed at LCN %lld\n", mnt->dev.drive, (long long)run_lcn);
            return done ? (s64)done : -EIO;
        }
        done += chunk;
    }
    return (s64)done;
}

static int zero_by_runlist(ntfs_mount_t *mnt, u8 *rl, int rl_len, u64 offset, u64 size, u64 max_size)
{
    u8 *zeros;

    if (!size) return 0;
    zeros = calloc(1, mnt->cluster_size);
    if (!zeros) return -ENOMEM;
    while (size) {
        size_t chunk = size > mnt->cluster_size ? mnt->cluster_size : (size_t)size;
        s64    done  = write_by_runlist(mnt, rl, rl_len, offset, zeros, chunk, max_size);
        if (done != (s64)chunk) {
            free(zeros);
            return -EIO;
        }
        offset += chunk;
        size -= chunk;
    }
    free(zeros);
    return 0;
}

static int bitmap_find_free(ntfs_mount_t *mnt, u64 count, s64 *extent_lcn, s64 *extent_length, int *extent_count)
{
    u8  byte      = 0;
    u64 remaining = count;
    int extents   = 0;

    if (!mnt || !mnt->bitmap_runlist || !count || !extent_lcn || !extent_length || !extent_count) return -EINVAL;
    for (u64 cluster = 0; cluster < (u64)mnt->nr_clusters && remaining; cluster++) {
        if (!(cluster & 7)
            && read_by_runlist(mnt, mnt->bitmap_runlist, (int)mnt->bitmap_runlist_size, cluster >> 3, &byte, 1, mnt->bitmap_data_size) != 1)
            return -EIO;
        if (byte & (1U << (cluster & 7))) continue;
        if (extents && extent_lcn[extents - 1] + extent_length[extents - 1] == (s64)cluster) {
            extent_length[extents - 1]++;
        } else {
            if (extents == 256) return -ENOSPC;
            extent_lcn[extents]    = (s64)cluster;
            extent_length[extents] = 1;
            extents++;
        }
        remaining--;
    }
    if (remaining) return -ENOSPC;
    *extent_count = extents;
    return 0;
}

static int bitmap_change_extents(ntfs_mount_t *mnt, const s64 *extent_lcn, const s64 *extent_length, int extent_count, int allocated)
{
    typedef struct bitmap_byte {
            u64 offset;
            u8  before;
            u8  after;
    } bitmap_byte_t;
    bitmap_byte_t *bytes;
    u64            cluster_count = 0;
    u32            byte_count    = 0;
    u32            committed     = 0;
    int            status        = -EIO;

    if (!mnt || !extent_lcn || !extent_length || extent_count < 0) return -EINVAL;
    for (int extent = 0; extent < extent_count; extent++) {
        if (extent_lcn[extent] < 0 || extent_length[extent] <= 0 || extent_lcn[extent] >= mnt->nr_clusters
            || extent_length[extent] > mnt->nr_clusters - extent_lcn[extent])
            return -EIO;
        if ((u64)extent_length[extent] > UINT64_MAX - cluster_count) return -EOVERFLOW;
        cluster_count += (u64)extent_length[extent];
    }
    if (!cluster_count) return 0;
    if (cluster_count > UINT32_MAX / sizeof(*bytes)) return -EOVERFLOW;
    bytes = calloc((size_t)cluster_count, sizeof(*bytes));
    if (!bytes) return -ENOMEM;

    for (int extent = 0; extent < extent_count; extent++) {
        for (s64 index = 0; index < extent_length[extent]; index++) {
            u64 cluster = (u64)(extent_lcn[extent] + index);
            u64 offset  = cluster >> 3;
            u32 slot    = 0;
            while (slot < byte_count && bytes[slot].offset != offset) slot++;
            if (slot == byte_count) {
                if (read_by_runlist(mnt, mnt->bitmap_runlist, (int)mnt->bitmap_runlist_size, offset, &bytes[slot].before, 1,
                                    mnt->bitmap_data_size)
                    != 1)
                    goto out;
                bytes[slot].offset = offset;
                bytes[slot].after  = bytes[slot].before;
                byte_count++;
            }
            if (allocated)
                bytes[slot].after |= (u8)(1U << (cluster & 7));
            else
                bytes[slot].after &= (u8) ~(1U << (cluster & 7));
        }
    }
    for (; committed < byte_count; committed++) {
        if (write_by_runlist(mnt, mnt->bitmap_runlist, (int)mnt->bitmap_runlist_size, bytes[committed].offset, &bytes[committed].after, 1,
                             mnt->bitmap_data_size)
            != 1)
            goto rollback;
    }
    status = 0;
    goto out;

rollback:
    while (committed) {
        committed--;
        write_by_runlist(mnt, mnt->bitmap_runlist, (int)mnt->bitmap_runlist_size, bytes[committed].offset, &bytes[committed].before, 1,
                         mnt->bitmap_data_size);
    }
out:
    free(bytes);
    return status;
}

static int zero_extents(ntfs_mount_t *mnt, const s64 *extent_lcn, const s64 *extent_length, int extent_count)
{
    u8 *zeros = calloc(1, mnt->cluster_size);
    if (!zeros) return -ENOMEM;
    for (int extent = 0; extent < extent_count; extent++) {
        for (s64 index = 0; index < extent_length[extent]; index++) {
            if (dev_write(mnt, (u64)(extent_lcn[extent] + index) << mnt->cluster_bits, zeros, mnt->cluster_size) < 0) {
                free(zeros);
                return -EIO;
            }
        }
    }
    free(zeros);
    return 0;
}

static u32 ntfs_index_vcn_size(const ntfs_mount_t *mnt)
{
    return mnt->indx_size < mnt->cluster_size ? mnt->indx_size : mnt->cluster_size;
}

static int ntfs_index_reclaim_stage(ntfs_mount_t *mnt, const s64 *lcn, const s64 *length, int count)
{
    if (!mnt || !lcn || !length || count <= 0 || count > 256) return -EINVAL;
    if (mnt->index_reclaim_count) return -EBUSY;
    for (int extent = 0; extent < count; extent++) {
        if (lcn[extent] < 0 || length[extent] <= 0 || lcn[extent] >= mnt->nr_clusters || length[extent] > mnt->nr_clusters - lcn[extent])
            return -EIO;
        mnt->index_reclaim_lcn[extent]    = lcn[extent];
        mnt->index_reclaim_length[extent] = length[extent];
    }
    mnt->index_reclaim_count = count;
    return 0;
}

static int ntfs_index_reclaim_commit(ntfs_mount_t *mnt)
{
    int status;
    if (!mnt) return -EINVAL;
    if (!mnt->index_reclaim_count) return 0;
    status = bitmap_change_extents(mnt, mnt->index_reclaim_lcn, mnt->index_reclaim_length, mnt->index_reclaim_count, 0);
    if (status == 0) mnt->index_reclaim_count = 0;
    return status;
}

static void ntfs_index_reclaim_abort(ntfs_mount_t *mnt)
{
    if (mnt) mnt->index_reclaim_count = 0;
}

static int ntfs_index_root_promote(ntfs_mount_t *mnt, u8 *record, u64 file_reference, u64 parent_reference, const u16 *name, u8 name_length,
                                   u32 file_attributes, u64 data_size, u64 allocated_size)
{
    static const u16 i30[] = {'$', 'I', '3', '0'};
    ntfs_mount_t     temporary_mount;
    u8              *temporary   = NULL;
    u8              *indx        = NULL;
    attr_rec_t      *root        = NULL;
    u32              root_offset = 0, root_length = 0, used = le32(record + 0x18);
    s64              lcn[256], run[256];
    int              run_count = 0;
    u8               mapping[2048];
    int              mapping_size;
    int              status = -EIO;

    if (!mnt || !record || !name || !name_length || !mnt->indx_size || used > mnt->mft_size) return -EINVAL;
    for (u32 offset = le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
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
    u16 value_offset = le16((u8 *)&root->d.res.value_offset);
    u32 value_length = le32((u8 *)&root->d.res.value_length);
    if (value_offset > root_length || value_length < 48 || value_length > root_length - value_offset) return -EIO;
    u8 *header = (u8 *)root + value_offset + 0x10;
    u32 first = le32(header), total = le32(header + 4);
    if (first < 16 || total < first + 16 || total > value_length - 0x10 || (header[12] & INDEX_ENTRY_NODE)) return -EIO;

    u32 capacity = mnt->mft_size + 1024;
    temporary    = calloc(1, capacity);
    if (!temporary) return -ENOMEM;
    memcpy(temporary, record, mnt->mft_size);
    memcpy(&temporary_mount, mnt, sizeof(temporary_mount));
    temporary_mount.mft_size = capacity;
    status = ntfs_index_root_insert(&temporary_mount, temporary, file_reference, parent_reference, name, name_length, file_attributes, data_size,
                                    allocated_size);
    if (status < 0) goto out;
    attr_rec_t *temp_root   = (attr_rec_t *)(temporary + root_offset);
    u8         *temp_header = (u8 *)temp_root + le16((u8 *)&temp_root->d.res.value_offset) + 0x10;
    u32         temp_first = le32(temp_header), entries_size = le32(temp_header + 4) - temp_first;
    if (0x40 + entries_size > mnt->indx_size - 2) {
        status = -ENOSPC;
        goto out;
    }

    u64 clusters = (mnt->indx_size + mnt->cluster_size - 1) / mnt->cluster_size;
    status       = bitmap_find_free(mnt, clusters, lcn, run, &run_count);
    if (status < 0) goto out;
    mapping_size = runlist_encode(lcn, run, run_count, mapping, sizeof(mapping));
    if (mapping_size < 0) {
        status = mapping_size;
        goto out;
    }
    u32 new_root_length = root_length - (total - first) + 24;
    u32 ia_length = align8(72 + (u32)mapping_size), bitmap_length = 40;
    if (used - root_length + new_root_length + ia_length + bitmap_length > mnt->mft_size) {
        status = -ENOSPC;
        goto out;
    }

    indx = calloc(1, mnt->indx_size);
    if (!indx) {
        status = -ENOMEM;
        goto out;
    }
    put_le32(indx, INDX_MAGIC);
    put_le16(indx + 4, 0x28);
    put_le16(indx + 6, (u16)(mnt->indx_size / mnt->sector_size + 1));
    put_le32(indx + 0x18, 0x28);
    put_le32(indx + 0x1c, 0x28 + entries_size);
    put_le32(indx + 0x20, mnt->indx_size - 0x18);
    memcpy(indx + 0x40, temp_header + temp_first, entries_size);
    status = ntfs_record_pack(mnt, indx, mnt->indx_size);
    if (status < 0) goto out;
    if (write_by_runlist(mnt, mapping, mapping_size, 0, indx, mnt->indx_size, mnt->indx_size) != (s64)mnt->indx_size) {
        status = -EIO;
        goto out;
    }
    status = bitmap_change_extents(mnt, lcn, run, run_count, 1);
    if (status < 0) goto out;

    u32 old_after = root_offset + root_length, new_after = root_offset + new_root_length;
    memmove(record + new_after, record + old_after, used - old_after);
    if (new_after < old_after) memset(record + used - (old_after - new_after), 0, old_after - new_after);
    used   = used - root_length + new_root_length;
    root   = (attr_rec_t *)(record + root_offset);
    header = (u8 *)root + value_offset + 0x10;
    memset(header + first, 0, 24);
    put_le16(header + first + 8, 24);
    put_le16(header + first + 12, INDEX_ENTRY_NODE | INDEX_ENTRY_END);
    put_le32(header + 4, first + 24);
    put_le32(header + 8, first + 24);
    header[12] = INDEX_ENTRY_NODE;
    put_le32((u8 *)&root->length, new_root_length);
    put_le32((u8 *)&root->d.res.value_length, value_length - (total - first) + 24);

    u32 end = used - 8;
    memmove(record + end + ia_length, record + end, 8);
    memset(record + end, 0, ia_length);
    attr_rec_t *allocation = (attr_rec_t *)(record + end);
    put_le32((u8 *)&allocation->type, AT_INDEX_ALLOCATION);
    put_le32((u8 *)&allocation->length, ia_length);
    allocation->non_resident = 1;
    allocation->name_length  = 4;
    put_le16((u8 *)&allocation->name_offset, 64);
    put_le16((u8 *)&allocation->instance, le16(record + 0x28));
    put_le64((u8 *)&allocation->d.nres.highest_vcn, clusters - 1);
    put_le16((u8 *)&allocation->d.nres.mapping_pairs_off, 72);
    put_le64((u8 *)&allocation->d.nres.alloc_size, clusters * mnt->cluster_size);
    put_le64((u8 *)&allocation->d.nres.data_size, mnt->indx_size);
    put_le64((u8 *)&allocation->d.nres.init_size, mnt->indx_size);
    for (u32 i = 0; i < 4; i++) put_le16((u8 *)allocation + 64 + (size_t)i * 2, i30[i]);
    memcpy((u8 *)allocation + 72, mapping, mapping_size);
    used += ia_length;
    u8  bitmap     = 1;
    u32 bitmap_end = ntfs_resident_attribute(record, used - 8, AT_BITMAP, le16(record + 0x28) + 1, i30, 4, &bitmap, 1);
    put_le32(record + bitmap_end, AT_END);
    put_le32(record + 0x18, bitmap_end + 8);
    put_le16(record + 0x28, le16(record + 0x28) + 2);
    status = 0;
out:
    free(indx);
    free(temporary);
    return status;
}

static u32 ntfs_index_entry_build(u8 *entry, u32 capacity, u64 file_reference, u64 parent_reference, const u16 *name, u8 name_length,
                                  u32 file_attributes, u64 data_size, u64 allocated_size)
{
    u16 key_length   = sizeof(fname_attr_t) + (size_t)name_length * 2;
    u32 entry_length = align8(16 + key_length);
    if (!entry || !name || !name_length || entry_length > capacity) return 0;
    memset(entry, 0, entry_length);
    put_le64(entry, file_reference);
    put_le16(entry + 8, entry_length);
    put_le16(entry + 10, key_length);
    fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
    put_le64((u8 *)&file_name->parent_dir, parent_reference);
    put_le64((u8 *)&file_name->alloc_size, allocated_size);
    put_le64((u8 *)&file_name->data_size, data_size);
    put_le32((u8 *)&file_name->fa, file_attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (u32 i = 0; i < name_length; i++) put_le16((u8 *)file_name->name + (size_t)i * 2, name[i]);
    return entry_length;
}

static int ntfs_index_root_insert_child(ntfs_mount_t *mnt, u8 *record, u64 old_child_vcn, u64 new_child_vcn, const u8 *key)
{
    attr_rec_t *root = NULL;
    u32         used = le32(record + 0x18), root_offset = 0, root_length = 0;
    u32         key_entry_length = le16(key + 8), key_length = le16(key + 10);
    if (!mnt || !record || !key || key_entry_length < 16 || key_length > key_entry_length - 16 || used > mnt->mft_size) return -EINVAL;
    for (u32 offset = le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
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
    u16 value_offset = le16((u8 *)&root->d.res.value_offset);
    u32 value_length = le32((u8 *)&root->d.res.value_length);
    u8 *header       = (u8 *)root + value_offset + 0x10;
    u32 first = le32(header), total = le32(header + 4), pointer_offset = 0;
    if (value_offset > root_length || value_length > root_length - value_offset || first < 16 || total > value_length - 0x10) return -EIO;
    for (u32 position = first; position + 24 <= total;) {
        u8 *entry  = header + position;
        u16 length = le16(entry + 8), flags = le16(entry + 12);
        if (length < 24 || length > total - position || !(flags & INDEX_ENTRY_NODE)) return -EIO;
        if (le64(entry + length - 8) == old_child_vcn) {
            pointer_offset = (u32)(entry - record);
            break;
        }
        position += length;
    }
    if (!pointer_offset) return -EIO;
    u32 promoted_length = align8(16 + key_length + 8);
    if (promoted_length > mnt->mft_size - used) return -ENOSPC;
    memmove(record + pointer_offset + promoted_length, record + pointer_offset, used - pointer_offset);
    memset(record + pointer_offset, 0, promoted_length);
    memcpy(record + pointer_offset, key, 16 + key_length);
    put_le16(record + pointer_offset + 8, promoted_length);
    put_le16(record + pointer_offset + 12, INDEX_ENTRY_NODE);
    put_le64(record + pointer_offset + promoted_length - 8, old_child_vcn);
    u8 *moved_pointer = record + pointer_offset + promoted_length;
    put_le64(moved_pointer + le16(moved_pointer + 8) - 8, new_child_vcn);
    root   = (attr_rec_t *)(record + root_offset);
    header = (u8 *)root + value_offset + 0x10;
    put_le32((u8 *)&root->length, root_length + promoted_length);
    put_le32((u8 *)&root->d.res.value_length, value_length + promoted_length);
    put_le32(header + 4, total + promoted_length);
    put_le32(header + 8, le32(header + 8) + promoted_length);
    put_le32(record + 0x18, used + promoted_length);
    return 0;
}

static int ntfs_index_allocation_split(ntfs_mount_t *mnt, u8 *record, u8 *block, u64 child_vcn, u32 insert_offset, u64 file_reference,
                                       u64 parent_reference, const u16 *name, u8 name_length, u32 file_attributes, u64 data_size,
                                       u64 allocated_size);

static int ntfs_index_allocation_insert(ntfs_mount_t *mnt, u8 *record, u64 file_reference, u64 parent_reference, const u16 *name, u8 name_length,
                                        u32 file_attributes, u64 data_size, u64 allocated_size)
{
    attr_rec_t *allocation = NULL;
    attr_rec_t *root       = NULL;
    u32         used = le32(record + 0x18), offset = le16(record + 0x14);
    u8         *block  = NULL;
    int         status = -EIO;

    if (!mnt || !record || !name || !name_length || used > mnt->mft_size) return -EINVAL;
    while (offset + 24 <= used) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length)) root = attribute;
        if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
        offset += length;
    }
    if (!root || root->non_resident || !allocation || !allocation->non_resident) return -EIO;
    u32 allocation_length = le32((u8 *)&allocation->length);
    u16 mapping_offset    = le16((u8 *)&allocation->d.nres.mapping_pairs_off);
    u64 stream_size       = le64((u8 *)&allocation->d.nres.data_size);
    if (mapping_offset < 64 || mapping_offset >= allocation_length || stream_size < mnt->indx_size) return -EIO;
    u16 root_value_offset = le16((u8 *)&root->d.res.value_offset);
    u32 root_value_length = le32((u8 *)&root->d.res.value_length);
    if (root_value_offset > le32((u8 *)&root->length) || root_value_length < 40
        || root_value_length > le32((u8 *)&root->length) - root_value_offset)
        return -EIO;
    u8 *root_header = (u8 *)root + root_value_offset + 0x10;
    u32 root_first = le32(root_header), root_total = le32(root_header + 4);
    u64 child_vcn = UINT64_MAX;
    if (!(root_header[12] & INDEX_ENTRY_NODE) || root_first < 16 || root_total < root_first + 24 || root_total > root_value_length - 0x10)
        return -EIO;
    for (u32 position = root_first; position + 24 <= root_total;) {
        u8 *entry      = root_header + position;
        u16 length     = le16(entry + 8);
        u16 key_length = le16(entry + 10);
        u16 flags      = le16(entry + 12);
        if (length < 24 || length > root_total - position || !(flags & INDEX_ENTRY_NODE)) return -EIO;
        if (flags & INDEX_ENTRY_END) {
            child_vcn = le64(entry + length - 8);
            break;
        }
        if (key_length < sizeof(fname_attr_t) || key_length > length - 24) return -EIO;
        fname_attr_t *root_name = (fname_attr_t *)(entry + 16);
        if ((size_t)root_name->name_len * 2 > key_length - sizeof(fname_attr_t)) return -EIO;
        u16 existing[255];
        for (u32 i = 0; i < root_name->name_len; i++) existing[i] = le16((u8 *)root_name->name + (size_t)i * 2);
        int comparison = ntfs_utf16_compare(mnt, name, name_length, existing, root_name->name_len);
        if (!comparison) return -EEXIST;
        if (comparison < 0) {
            child_vcn = le64(entry + length - 8);
            break;
        }
        position += length;
    }
    u32 vcn_size = ntfs_index_vcn_size(mnt);
    if (!vcn_size || child_vcn == UINT64_MAX || child_vcn > UINT64_MAX / vcn_size) return -EIO;
    u64 block_offset = child_vcn * vcn_size;
    if (block_offset > stream_size || mnt->indx_size > stream_size - block_offset) return -EIO;
    block = malloc(mnt->indx_size);
    if (!block) return -ENOMEM;
    if (read_by_runlist(mnt, (u8 *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block, mnt->indx_size,
                        stream_size)
            != (int)mnt->indx_size
        || ntfs_record_unpack(mnt, block, mnt->indx_size) < 0 || le32(block) != INDX_MAGIC)
        goto out;
    u8 *header = block + 0x18;
    u32 first = le32(header), total = le32(header + 4), capacity = le32(header + 8), insert = 0;
    if (first < 16 || total < first + 16 || capacity < total || capacity > mnt->indx_size - 0x18 || header[12] & INDEX_ENTRY_NODE) goto out;
    for (u32 position = first; position + 16 <= total;) {
        u8 *entry  = header + position;
        u16 length = le16(entry + 8), key_length = le16(entry + 10), flags = le16(entry + 12);
        if (length < 16 || length > total - position) goto out;
        if (flags & INDEX_ENTRY_END) {
            insert = (u32)(entry - block);
            break;
        }
        if (key_length < sizeof(fname_attr_t) || key_length > length - 16) goto out;
        fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
        u16           existing[255];
        if ((size_t)file_name->name_len * 2 > key_length - sizeof(fname_attr_t)) goto out;
        for (u32 i = 0; i < file_name->name_len; i++) existing[i] = le16((u8 *)file_name->name + (size_t)i * 2);
        int comparison = ntfs_utf16_compare(mnt, name, name_length, existing, file_name->name_len);
        if (!comparison) {
            status = -EEXIST;
            goto out;
        }
        if (comparison < 0) {
            insert = (u32)(entry - block);
            break;
        }
        position += length;
    }
    if (!insert) goto out;
    u16 key_length   = sizeof(fname_attr_t) + (size_t)name_length * 2;
    u32 entry_length = align8(16 + key_length);
    if (entry_length > capacity - total || insert > mnt->indx_size - entry_length) {
        status = ntfs_index_allocation_split(mnt, record, block, child_vcn, insert, file_reference, parent_reference, name, name_length,
                                             file_attributes, data_size, allocated_size);
        goto out;
    }
    memmove(block + insert + entry_length, block + insert, 0x18 + total - insert);
    memset(block + insert, 0, entry_length);
    put_le64(block + insert, file_reference);
    put_le16(block + insert + 8, entry_length);
    put_le16(block + insert + 10, key_length);
    fname_attr_t *file_name = (fname_attr_t *)(block + insert + 16);
    put_le64((u8 *)&file_name->parent_dir, parent_reference);
    put_le64((u8 *)&file_name->alloc_size, allocated_size);
    put_le64((u8 *)&file_name->data_size, data_size);
    put_le32((u8 *)&file_name->fa, file_attributes);
    file_name->name_len  = name_length;
    file_name->name_type = 1;
    for (u32 i = 0; i < name_length; i++) put_le16((u8 *)file_name->name + (size_t)i * 2, name[i]);
    put_le32(header + 4, total + entry_length);
    status = ntfs_record_pack(mnt, block, mnt->indx_size);
    if (status < 0) goto out;
    status = write_by_runlist(mnt, (u8 *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block, mnt->indx_size,
                              stream_size)
                     == (s64)mnt->indx_size ?
                 0 :
                 -EIO;
out:
    free(block);
    return status;
}

static int ntfs_index_allocation_split(ntfs_mount_t *mnt, u8 *record, u8 *block, u64 child_vcn, u32 insert_offset, u64 file_reference,
                                       u64 parent_reference, const u16 *name, u8 name_length, u32 file_attributes, u64 data_size,
                                       u64 allocated_size)
{
    u8 *header = block + 0x18;
    u32 first = le32(header), total = le32(header + 4);
    u32 area_offset = 0x18 + first, old_size = total - first;
    u8  new_entry[16 + sizeof(fname_attr_t) + (size_t)255 * 2 + 8];
    u32 new_length = ntfs_index_entry_build(new_entry, sizeof(new_entry), file_reference, parent_reference, name, name_length, file_attributes,
                                            data_size, allocated_size);
    u8 *all = NULL, *left = NULL, *right = NULL, *original = NULL, *record_copy = NULL;
    s64 old_vcn[256], old_lcn[256], old_run[256], new_lcn[256], new_run[256];
    int old_count, new_count, status = -EIO;
    int bitmap_committed           = 0;
    int left_committed             = 0;
    u32 rollback_allocation_offset = 0;
    u32 rollback_allocation_length = 0;
    u16 rollback_mapping_offset    = 0;
    u64 rollback_stream_size       = 0;

    if (!new_length || insert_offset < area_offset || insert_offset > area_offset + old_size - 16) return -EINVAL;
    u32 relative_insert = insert_offset - area_offset;
    all                 = malloc(old_size + new_length);
    left                = calloc(1, mnt->indx_size);
    right               = calloc(1, mnt->indx_size);
    original            = malloc(mnt->indx_size);
    record_copy         = malloc(mnt->mft_size);
    if (!all || !left || !right || !original || !record_copy) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(original, block, mnt->indx_size);
    if (ntfs_record_pack(mnt, original, mnt->indx_size) < 0) goto out;
    memcpy(all, block + area_offset, relative_insert);
    memcpy(all + relative_insert, new_entry, new_length);
    memcpy(all + relative_insert + new_length, block + insert_offset, old_size - relative_insert);
    u32 all_size = old_size + new_length, position = 0, entries = 0;
    while (position + 16 <= all_size) {
        u16 length = le16(all + position + 8), flags = le16(all + position + 12);
        if (length < 16 || length > all_size - position) goto out;
        if (flags & INDEX_ENTRY_END) break;
        entries++;
        position += length;
    }
    if (entries < 2 || position + 16 > all_size) goto out;
    u32 median_index = entries / 2, median_offset = 0;
    for (u32 index = 0; index < median_index; index++) median_offset += le16(all + median_offset + 8);
    u16 median_length = le16(all + median_offset + 8);
    u32 right_offset  = median_offset + median_length;
    u32 right_size    = all_size - right_offset;
    u32 left_size     = median_offset + 16;
    if (0x40 + left_size > mnt->indx_size - 2 || 0x40 + right_size > mnt->indx_size - 2) {
        status = -ENOSPC;
        goto out;
    }

    memcpy(record_copy, record, mnt->mft_size);
    attr_rec_t *allocation = NULL, *index_bitmap = NULL;
    u32         used = le32(record_copy + 0x18);
    for (u32 attr_offset = le16(record_copy + 0x14); attr_offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record_copy + attr_offset);
        u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || attr_offset > used - length) goto out;
        if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
        if (type == AT_BITMAP && ntfs_attr_name_is_i30(attribute, length)) index_bitmap = attribute;
        attr_offset += length;
    }
    if (!allocation || !allocation->non_resident || !index_bitmap || index_bitmap->non_resident) goto out;
    u32 allocation_length  = le32((u8 *)&allocation->length);
    u16 mapping_offset     = le16((u8 *)&allocation->d.nres.mapping_pairs_off);
    u64 stream_size        = le64((u8 *)&allocation->d.nres.data_size);
    u64 allocated_clusters = le64((u8 *)&allocation->d.nres.alloc_size) / mnt->cluster_size;
    if (!stream_size || stream_size % mnt->indx_size || mapping_offset < 64 || mapping_offset >= allocation_length) goto out;
    old_count = runlist_parse((u8 *)allocation + mapping_offset, allocation_length - mapping_offset, old_vcn, old_lcn, old_run, 256);
    if (old_count <= 0 || (u64)(old_vcn[old_count - 1] + old_run[old_count - 1]) != allocated_clusters) goto out;
    rollback_allocation_offset = (u32)((u8 *)allocation - record_copy);
    rollback_allocation_length = allocation_length;
    rollback_mapping_offset    = mapping_offset;
    rollback_stream_size       = stream_size;
    u64 required_clusters      = (stream_size + mnt->indx_size + mnt->cluster_size - 1) / mnt->cluster_size;
    u64 new_clusters           = required_clusters > allocated_clusters ? required_clusters - allocated_clusters : 0;
    u64 final_clusters         = allocated_clusters + new_clusters;
    new_count                  = 0;
    if (new_clusters) {
        status = bitmap_find_free(mnt, new_clusters, new_lcn, new_run, &new_count);
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
    u8  mapping[2048];
    int mapping_size = runlist_encode(old_lcn, old_run, old_count, mapping, sizeof(mapping));
    if (mapping_size < 0) {
        status = mapping_size;
        goto out;
    }
    if ((u32)mapping_size > allocation_length - mapping_offset) {
        u32 new_allocation_length = align8(mapping_offset + (u32)mapping_size);
        u32 allocation_offset     = (u32)((u8 *)allocation - record_copy);
        used                      = le32(record_copy + 0x18);
        if (new_allocation_length - allocation_length > mnt->mft_size - used) {
            status = -ENOSPC;
            goto out;
        }
        memmove(record_copy + allocation_offset + new_allocation_length, record_copy + allocation_offset + allocation_length,
                used - allocation_offset - allocation_length);
        memset(record_copy + allocation_offset + allocation_length, 0, new_allocation_length - allocation_length);
        put_le32(record_copy + 0x18, used + new_allocation_length - allocation_length);
        allocation = (attr_rec_t *)(record_copy + allocation_offset);
        put_le32((u8 *)&allocation->length, new_allocation_length);
    }
    u64 new_child_vcn = stream_size / ntfs_index_vcn_size(mnt);
    status            = ntfs_index_root_insert_child(mnt, record_copy, child_vcn, new_child_vcn, all + median_offset);
    if (status < 0) goto out;
    allocation   = NULL;
    index_bitmap = NULL;
    used         = le32(record_copy + 0x18);
    for (u32 attr_offset = le16(record_copy + 0x14); attr_offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record_copy + attr_offset);
        u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
        if (type == AT_BITMAP && ntfs_attr_name_is_i30(attribute, length)) index_bitmap = attribute;
        attr_offset += length;
    }
    if (!allocation || !index_bitmap) goto out;
    u16 bitmap_value_offset    = le16((u8 *)&index_bitmap->d.res.value_offset);
    u32 bitmap_value_length    = le32((u8 *)&index_bitmap->d.res.value_length);
    u64 block_index            = stream_size / mnt->indx_size;
    u32 required_bitmap_length = (u32)(block_index / 8 + 1);
    u32 index_bitmap_length    = le32((u8 *)&index_bitmap->length);
    if (bitmap_value_offset > index_bitmap_length || bitmap_value_length > index_bitmap_length - bitmap_value_offset) goto out;
    if (required_bitmap_length > bitmap_value_length) {
        u32 new_bitmap_length = align8(bitmap_value_offset + required_bitmap_length);
        u32 bitmap_offset     = (u32)((u8 *)index_bitmap - record_copy);
        used                  = le32(record_copy + 0x18);
        if (new_bitmap_length > index_bitmap_length && new_bitmap_length - index_bitmap_length > mnt->mft_size - used) {
            status = -ENOSPC;
            goto out;
        }
        memmove(record_copy + bitmap_offset + new_bitmap_length, record_copy + bitmap_offset + index_bitmap_length,
                used - bitmap_offset - index_bitmap_length);
        memset(record_copy + bitmap_offset + bitmap_value_offset + bitmap_value_length, 0,
               new_bitmap_length - bitmap_value_offset - bitmap_value_length);
        put_le32(record_copy + 0x18, used - index_bitmap_length + new_bitmap_length);
        index_bitmap = (attr_rec_t *)(record_copy + bitmap_offset);
        put_le32((u8 *)&index_bitmap->length, new_bitmap_length);
        put_le32((u8 *)&index_bitmap->d.res.value_length, required_bitmap_length);
        allocation = NULL;
        used       = le32(record_copy + 0x18);
        for (u32 attr_offset = le16(record_copy + 0x14); attr_offset + 24 <= used;) {
            attr_rec_t *attribute = (attr_rec_t *)(record_copy + attr_offset);
            u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
            if (type == AT_END) break;
            if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
            attr_offset += length;
        }
        if (!allocation) goto out;
    }
    *((u8 *)index_bitmap + bitmap_value_offset + block_index / 8) |= (u8)(1U << (block_index & 7));
    allocation_length = le32((u8 *)&allocation->length);
    mapping_offset    = le16((u8 *)&allocation->d.nres.mapping_pairs_off);
    memset((u8 *)allocation + mapping_offset, 0, allocation_length - mapping_offset);
    memcpy((u8 *)allocation + mapping_offset, mapping, mapping_size);
    put_le64((u8 *)&allocation->d.nres.highest_vcn, final_clusters - 1);
    put_le64((u8 *)&allocation->d.nres.alloc_size, final_clusters * mnt->cluster_size);
    put_le64((u8 *)&allocation->d.nres.data_size, stream_size + mnt->indx_size);
    put_le64((u8 *)&allocation->d.nres.init_size, stream_size + mnt->indx_size);

    u8 *old_terminal = all + all_size - 16;
    put_le32(left, INDX_MAGIC);
    put_le16(left + 4, 0x28);
    put_le16(left + 6, mnt->indx_size / mnt->sector_size + 1);
    put_le64(left + 16, child_vcn);
    put_le32(left + 0x18, 0x28);
    put_le32(left + 0x1c, 0x28 + left_size);
    put_le32(left + 0x20, mnt->indx_size - 0x18);
    memcpy(left + 0x40, all, median_offset);
    memcpy(left + 0x40 + median_offset, old_terminal, 16);
    put_le32(right, INDX_MAGIC);
    put_le16(right + 4, 0x28);
    put_le16(right + 6, mnt->indx_size / mnt->sector_size + 1);
    put_le64(right + 16, new_child_vcn);
    put_le32(right + 0x18, 0x28);
    put_le32(right + 0x1c, 0x28 + right_size);
    put_le32(right + 0x20, mnt->indx_size - 0x18);
    memcpy(right + 0x40, all + right_offset, right_size);
    if (ntfs_record_pack(mnt, left, mnt->indx_size) < 0 || ntfs_record_pack(mnt, right, mnt->indx_size) < 0) goto out;
    if (write_by_runlist(mnt, mapping, mapping_size, stream_size, right, mnt->indx_size, stream_size + mnt->indx_size) != (s64)mnt->indx_size)
        goto out;
    status = bitmap_change_extents(mnt, new_lcn, new_run, new_count, 1);
    if (status < 0) goto out;
    bitmap_committed = 1;
    left_committed   = 1;
    if (write_by_runlist(mnt, mapping, mapping_size, child_vcn * ntfs_index_vcn_size(mnt), left, mnt->indx_size, stream_size + mnt->indx_size)
        != (s64)mnt->indx_size) {
        status = -EIO;
        goto out;
    }
    memcpy(record, record_copy, mnt->mft_size);
    status = 0;
out:
    if (status < 0 && left_committed) {
        attr_rec_t *rollback_allocation = (attr_rec_t *)(record + rollback_allocation_offset);
        if (rollback_mapping_offset >= 64 && rollback_mapping_offset < rollback_allocation_length)
            write_by_runlist(mnt, (u8 *)rollback_allocation + rollback_mapping_offset,
                             (int)(rollback_allocation_length - rollback_mapping_offset), child_vcn * ntfs_index_vcn_size(mnt), original,
                             mnt->indx_size, rollback_stream_size);
    }
    if (status < 0 && bitmap_committed) bitmap_change_extents(mnt, new_lcn, new_run, new_count, 0);
    free(record_copy);
    free(original);
    free(right);
    free(left);
    free(all);
    return status;
}

static int ntfs_directory_index_insert(ntfs_mount_t *mnt, u8 *record, u64 file_reference, u64 parent_reference, const u16 *name, u8 name_length,
                                       u32 file_attributes, u64 data_size, u64 allocated_size)
{
    u32 used = le32(record + 0x18);
    if (!mnt || !record || used > mnt->mft_size) return -EINVAL;
    for (u32 offset = le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length) && !attribute->non_resident) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < 32 || value_length > length - value_offset) return -EIO;
            u8 *header = (u8 *)attribute + value_offset + 0x10;
            if (header[12] & INDEX_ENTRY_NODE)
                return ntfs_index_allocation_insert(mnt, record, file_reference, parent_reference, name, name_length, file_attributes, data_size,
                                                    allocated_size);
            int status = ntfs_index_root_insert(mnt, record, file_reference, parent_reference, name, name_length, file_attributes, data_size,
                                                allocated_size);
            if (status != -ENOSPC) return status;
            return ntfs_index_root_promote(mnt, record, file_reference, parent_reference, name, name_length, file_attributes, data_size,
                                           allocated_size);
        }
        offset += length;
    }
    return -EIO;
}

static int ntfs_index_root_replace_key(ntfs_mount_t *mnt, u8 *record, u32 entry_offset, const u8 *replacement)
{
    attr_rec_t *root = NULL;
    u32         used = le32(record + 0x18), root_offset = 0, root_length = 0;
    if (!mnt || !record || !replacement || used > mnt->mft_size || entry_offset >= used) return -EINVAL;
    for (u32 offset = le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
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
    u16 old_length = le16(record + entry_offset + 8), key_length = le16(replacement + 10);
    if (old_length < 24 || key_length < sizeof(fname_attr_t) || key_length > le16(replacement + 8) - 16) return -EIO;
    u32 new_length = align8(16 + key_length + 8);
    if (new_length > old_length && new_length - old_length > mnt->mft_size - used) return -ENOSPC;
    u64 child_vcn = le64(record + entry_offset + old_length - 8);
    memmove(record + entry_offset + new_length, record + entry_offset + old_length, used - entry_offset - old_length);
    if (new_length < old_length) memset(record + used - (old_length - new_length), 0, old_length - new_length);
    memset(record + entry_offset, 0, new_length);
    memcpy(record + entry_offset, replacement, 16 + key_length);
    put_le16(record + entry_offset + 8, new_length);
    put_le16(record + entry_offset + 12, INDEX_ENTRY_NODE);
    put_le64(record + entry_offset + new_length - 8, child_vcn);
    root             = (attr_rec_t *)(record + root_offset);
    u16 value_offset = le16((u8 *)&root->d.res.value_offset);
    u8 *header       = (u8 *)root + value_offset + 0x10;
    put_le32((u8 *)&root->length, root_length - old_length + new_length);
    put_le32((u8 *)&root->d.res.value_length, le32((u8 *)&root->d.res.value_length) - old_length + new_length);
    put_le32(header + 4, le32(header + 4) - old_length + new_length);
    put_le32(header + 8, le32(header + 8) - old_length + new_length);
    put_le32(record + 0x18, used - old_length + new_length);
    return 0;
}

typedef struct ntfs_index_item {
        u8 *entry;
        u16 length;
} ntfs_index_item_t;

typedef struct ntfs_index_attributes {
        attr_rec_t *root;
        attr_rec_t *allocation;
        attr_rec_t *bitmap;
        u32         root_offset;
        u32         allocation_offset;
        u32         bitmap_offset;
} ntfs_index_attributes_t;

static void ntfs_index_items_free(ntfs_index_item_t *items, u32 count)
{
    if (!items) return;
    for (u32 index = 0; index < count; index++) free(items[index].entry);
    free(items);
}

static int ntfs_index_attributes_find(ntfs_mount_t *mnt, u8 *record, ntfs_index_attributes_t *attributes)
{
    u32 used;
    u32 offset;

    if (!mnt || !record || !attributes) return -EINVAL;
    memset(attributes, 0, sizeof(*attributes));
    used = le32(record + 0x18);
    if (used > mnt->mft_size) return -EIO;
    offset = le16(record + 0x14);
    while (offset + 24 <= used) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
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

static int ntfs_index_bitmap_test(ntfs_mount_t *mnt, attr_rec_t *bitmap, u64 bit, int *set)
{
    u32 length;
    u8  value;

    if (!mnt || !bitmap || !set) return -EINVAL;
    length = le32((u8 *)&bitmap->length);
    if (bitmap->non_resident) {
        u16 mapping_offset = le16((u8 *)&bitmap->d.nres.mapping_pairs_off);
        u64 data_size      = le64((u8 *)&bitmap->d.nres.data_size);
        if (length < 64 || mapping_offset < 64 || mapping_offset >= length || bit / 8 >= data_size
            || read_by_runlist(mnt, (u8 *)bitmap + mapping_offset, (int)(length - mapping_offset), bit / 8, &value, 1, data_size) != 1)
            return -EIO;
    } else {
        u16 value_offset = le16((u8 *)&bitmap->d.res.value_offset);
        u32 value_length = le32((u8 *)&bitmap->d.res.value_length);
        if (value_offset > length || value_length > length - value_offset || bit / 8 >= value_length) return -EIO;
        value = *((u8 *)bitmap + value_offset + bit / 8);
    }
    *set = !!(value & (1U << (bit & 7)));
    return 0;
}

static int ntfs_index_item_append(ntfs_index_item_t **items, u32 *count, u32 *capacity, const u8 *entry, int node)
{
    u16 key_length;
    u16 entry_length;
    u32 stored_length;
    u8 *copy;

    if (!items || !count || !capacity || !entry) return -EINVAL;
    entry_length = le16(entry + 8);
    key_length   = le16(entry + 10);
    if (entry_length < (node ? 24 : 16) || key_length < sizeof(fname_attr_t) || key_length > entry_length - (node ? 24 : 16)) return -EIO;
    fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
    if ((size_t)file_name->name_len * 2 > key_length - sizeof(fname_attr_t)) return -EIO;
    stored_length = align8(16 + key_length);
    copy          = calloc(1, stored_length);
    if (!copy) return -ENOMEM;
    memcpy(copy, entry, 16 + key_length);
    put_le16(copy + 8, (u16)stored_length);
    put_le16(copy + 12, 0);
    if (*count == *capacity) {
        u32 new_capacity = *capacity ? *capacity * 2 : 16;
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
    (*items)[*count].length = (u16)stored_length;
    (*count)++;
    return 0;
}

static int ntfs_index_collect_header(ntfs_index_item_t **items, u32 *count, u32 *capacity, u8 *buffer, u32 buffer_size, u32 header_offset)
{
    u8 *header;
    u32 first;
    u32 total;
    int node;

    if (!buffer || header_offset > buffer_size || buffer_size - header_offset < 16) return -EIO;
    header = buffer + header_offset;
    first  = le32(header);
    total  = le32(header + 4);
    node   = !!(header[12] & INDEX_ENTRY_NODE);
    if (first < 16 || total < first + (node ? 24U : 16U) || total > buffer_size - header_offset) return -EIO;
    for (u32 position = first; position + (node ? 24U : 16U) <= total;) {
        u8 *entry  = header + position;
        u16 length = le16(entry + 8);
        u16 flags  = le16(entry + 12);
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

static int ntfs_directory_index_collect(ntfs_mount_t *mnt, u8 *record, ntfs_index_item_t **result, u32 *result_count,
                                        ntfs_index_attributes_t *result_attributes)
{
    ntfs_index_attributes_t attributes;
    ntfs_index_item_t      *items    = NULL;
    u32                     count    = 0;
    u32                     capacity = 0;
    u8                     *block    = NULL;
    int                     status;

    if (!mnt || !record || !result || !result_count) return -EINVAL;
    status = ntfs_index_attributes_find(mnt, record, &attributes);
    if (status < 0) return status;
    if (attributes.root->non_resident) return -EIO;
    u32 root_length       = le32((u8 *)&attributes.root->length);
    u16 root_value_offset = le16((u8 *)&attributes.root->d.res.value_offset);
    u32 root_value_length = le32((u8 *)&attributes.root->d.res.value_length);
    if (root_value_offset > root_length || root_value_length < 32 || root_value_length > root_length - root_value_offset) return -EIO;
    status = ntfs_index_collect_header(&items, &count, &capacity, (u8 *)attributes.root + root_value_offset, root_value_length, 0x10);
    if (status < 0) goto out;

    if (attributes.allocation || attributes.bitmap) {
        if (!attributes.allocation || !attributes.bitmap || !attributes.allocation->non_resident) {
            status = -EIO;
            goto out;
        }
        u32 allocation_length = le32((u8 *)&attributes.allocation->length);
        u16 mapping_offset    = le16((u8 *)&attributes.allocation->d.nres.mapping_pairs_off);
        u64 stream_size       = le64((u8 *)&attributes.allocation->d.nres.data_size);
        if (allocation_length < 64 || mapping_offset < 64 || mapping_offset >= allocation_length || !mnt->indx_size
            || stream_size % mnt->indx_size)
            goto corrupt;
        block = malloc(mnt->indx_size);
        if (!block) {
            status = -ENOMEM;
            goto out;
        }
        u64 block_count = stream_size / mnt->indx_size;
        for (u64 index = 0; index < block_count; index++) {
            int allocated;
            status = ntfs_index_bitmap_test(mnt, attributes.bitmap, index, &allocated);
            if (status < 0) goto out;
            if (!allocated) continue;
            u64 logical_offset = index * (u64)mnt->indx_size;
            if (read_by_runlist(mnt, (u8 *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), logical_offset,
                                block, mnt->indx_size, stream_size)
                    != (int)mnt->indx_size
                || ntfs_record_unpack(mnt, block, mnt->indx_size) < 0 || le32(block) != INDX_MAGIC) {
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

static int ntfs_directory_index_set_reparse_tag(ntfs_mount_t *mnt, u8 *record, u64 file_reference, const u16 *name, u8 name_length)
{
    ntfs_index_attributes_t attributes;
    u8                     *block = NULL;
    int                     status;

    status = ntfs_index_root_set_reparse_tag(mnt, record, file_reference, name, name_length);
    if (status != -ENOENT) return status;
    status = ntfs_index_attributes_find(mnt, record, &attributes);
    if (status < 0) return status;
    if (!attributes.allocation || !attributes.bitmap || !attributes.allocation->non_resident) return -ENOENT;
    u32 allocation_length = le32((u8 *)&attributes.allocation->length);
    u16 mapping_offset    = le16((u8 *)&attributes.allocation->d.nres.mapping_pairs_off);
    u64 stream_size       = le64((u8 *)&attributes.allocation->d.nres.data_size);
    if (allocation_length < 64 || mapping_offset < 64 || mapping_offset >= allocation_length || !mnt->indx_size || stream_size % mnt->indx_size)
        return -EIO;
    block = malloc(mnt->indx_size);
    if (!block) return -ENOMEM;
    for (u64 index = 0; index < stream_size / mnt->indx_size; index++) {
        int allocated;
        status = ntfs_index_bitmap_test(mnt, attributes.bitmap, index, &allocated);
        if (status < 0) goto out;
        if (!allocated) continue;
        u64 logical_offset = index * (u64)mnt->indx_size;
        if (read_by_runlist(mnt, (u8 *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), logical_offset, block,
                            mnt->indx_size, stream_size)
                != (int)mnt->indx_size
            || ntfs_record_unpack(mnt, block, mnt->indx_size) < 0 || le32(block) != INDX_MAGIC) {
            status = -EIO;
            goto out;
        }
        u8 *header = block + 0x18;
        u32 first  = le32(header);
        u32 total  = le32(header + 4);
        int node   = !!(header[12] & INDEX_ENTRY_NODE);
        if (first < 16 || total < first + (node ? 24U : 16U) || total > mnt->indx_size - 0x18) {
            status = -EIO;
            goto out;
        }
        for (u32 position = first; position + (node ? 24U : 16U) <= total;) {
            u8 *entry      = header + position;
            u16 length     = le16(entry + 8);
            u16 key_length = le16(entry + 10);
            u16 flags      = le16(entry + 12);
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
            int equal = (le64(entry) & 0x0000ffffffffffffULL) == (file_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length;
            for (u32 character = 0; equal && character < name_length; character++)
                if (le16((u8 *)file_name->name + (size_t)character * 2) != name[character]) equal = 0;
            if (equal) {
                put_le32((u8 *)&file_name->rp_tag, IO_REPARSE_TAG_SYMLINK);
                status = ntfs_record_pack(mnt, block, mnt->indx_size);
                if (status < 0) goto out;
                status = write_by_runlist(mnt, (u8 *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset),
                                          logical_offset, block, mnt->indx_size, stream_size)
                                 == (s64)mnt->indx_size ?
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

static int ntfs_index_item_compare(ntfs_mount_t *mnt, const ntfs_index_item_t *left, const ntfs_index_item_t *right)
{
    fname_attr_t *left_name  = (fname_attr_t *)(left->entry + 16);
    fname_attr_t *right_name = (fname_attr_t *)(right->entry + 16);
    u16           left_utf16[255];
    u16           right_utf16[255];
    for (u32 index = 0; index < left_name->name_len; index++) left_utf16[index] = le16((u8 *)left_name->name + (size_t)index * 2);
    for (u32 index = 0; index < right_name->name_len; index++) right_utf16[index] = le16((u8 *)right_name->name + (size_t)index * 2);
    return ntfs_utf16_compare(mnt, left_utf16, left_name->name_len, right_utf16, right_name->name_len);
}

static int ntfs_record_remove_attribute(u8 *record, u32 record_size, u32 offset)
{
    u32 used;
    u32 length;

    if (!record || offset + 8 > record_size) return -EINVAL;
    used   = le32(record + 0x18);
    length = le32(record + offset + 4);
    if (used > record_size || length < 24 || offset > used - length) return -EIO;
    memmove(record + offset, record + offset + length, used - offset - length);
    memset(record + used - length, 0, length);
    put_le32(record + 0x18, used - length);
    return 0;
}

static int ntfs_index_try_collapse(ntfs_mount_t *mnt, u8 *record)
{
    ntfs_index_attributes_t attributes;
    ntfs_index_item_t      *items = NULL;
    u8                     *copy  = NULL;
    u32                     count = 0;
    s64                     vcn[256], lcn[256], run[256];
    int                     run_count;
    int                     status;

    status = ntfs_directory_index_collect(mnt, record, &items, &count, &attributes);
    if (status < 0) return status;
    if (!attributes.allocation) {
        ntfs_index_items_free(items, count);
        return 0;
    }
    for (u32 index = 1; index < count; index++) {
        ntfs_index_item_t item     = items[index];
        u32               position = index;
        while (position && ntfs_index_item_compare(mnt, &item, &items[position - 1]) < 0) {
            items[position] = items[position - 1];
            position--;
        }
        items[position] = item;
    }
    for (u32 index = 1; index < count; index++) {
        if (!ntfs_index_item_compare(mnt, &items[index - 1], &items[index])) {
            status = -EIO;
            goto out;
        }
    }

    u64 entries_length = 16;
    for (u32 index = 0; index < count; index++) entries_length += items[index].length;
    u32 root_length       = le32((u8 *)&attributes.root->length);
    u16 root_value_offset = le16((u8 *)&attributes.root->d.res.value_offset);
    u32 new_value_length  = 0x10 + 0x10 + (u32)entries_length;
    u32 new_root_length   = align8(root_value_offset + new_value_length);
    u32 allocation_length = le32((u8 *)&attributes.allocation->length);
    u32 bitmap_length     = le32((u8 *)&attributes.bitmap->length);
    u32 used              = le32(record + 0x18);
    if (root_length > used || allocation_length > used - root_length || bitmap_length > used - root_length - allocation_length) {
        status = -EIO;
        goto out;
    }
    u32 fixed_length = used - root_length - allocation_length - bitmap_length;
    if (entries_length > UINT32_MAX - 0x20 || new_root_length > mnt->mft_size || fixed_length > mnt->mft_size - new_root_length) {
        status = 0;
        goto out;
    }
    u16 mapping_offset = le16((u8 *)&attributes.allocation->d.nres.mapping_pairs_off);
    if (mapping_offset < 64 || mapping_offset >= allocation_length) {
        status = -EIO;
        goto out;
    }
    run_count = runlist_parse((u8 *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), vcn, lcn, run, 256);
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
    u32 first_remove  = attributes.allocation_offset > attributes.bitmap_offset ? attributes.allocation_offset : attributes.bitmap_offset;
    u32 second_remove = attributes.allocation_offset > attributes.bitmap_offset ? attributes.bitmap_offset : attributes.allocation_offset;
    status            = ntfs_record_remove_attribute(copy, mnt->mft_size, first_remove);
    if (status < 0) goto out;
    status = ntfs_record_remove_attribute(copy, mnt->mft_size, second_remove);
    if (status < 0) goto out;
    status = ntfs_index_attributes_find(mnt, copy, &attributes);
    if (status < 0 || attributes.allocation || attributes.bitmap) goto corrupt;
    root_length       = le32((u8 *)&attributes.root->length);
    root_value_offset = le16((u8 *)&attributes.root->d.res.value_offset);
    used              = le32(copy + 0x18);
    if (new_root_length > root_length && new_root_length - root_length > mnt->mft_size - used) goto corrupt;
    memmove(copy + attributes.root_offset + new_root_length, copy + attributes.root_offset + root_length,
            used - attributes.root_offset - root_length);
    if (new_root_length < root_length) memset(copy + used - (root_length - new_root_length), 0, root_length - new_root_length);
    put_le32(copy + 0x18, used - root_length + new_root_length);
    attributes.root = (attr_rec_t *)(copy + attributes.root_offset);
    put_le32((u8 *)&attributes.root->length, new_root_length);
    put_le32((u8 *)&attributes.root->d.res.value_length, new_value_length);
    u8 *value  = (u8 *)attributes.root + root_value_offset;
    u8 *header = value + 0x10;
    memset(header, 0, new_value_length - 0x10);
    put_le32(header, 0x10);
    put_le32(header + 4, 0x10 + (u32)entries_length);
    put_le32(header + 8, 0x10 + (u32)entries_length);
    u32 position = 0x10;
    for (u32 index = 0; index < count; index++) {
        memcpy(header + position, items[index].entry, items[index].length);
        position += items[index].length;
    }
    put_le16(header + position + 8, 16);
    put_le16(header + position + 12, INDEX_ENTRY_END);
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

static int ntfs_directory_index_remove(ntfs_mount_t *mnt, u8 *record, u64 file_reference, const u16 *name, u8 name_length)
{
    attr_rec_t *root = NULL, *allocation = NULL;
    u32         used = le32(record + 0x18);
    if (!mnt || !record || !name || !name_length || used > mnt->mft_size) return -EINVAL;
    for (u32 offset = le16(record + 0x14); offset + 24 <= used;) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type = le32((u8 *)&attribute->type), length = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) return -EIO;
        if (type == AT_INDEX_ROOT && ntfs_attr_name_is_i30(attribute, length)) root = attribute;
        if (type == AT_INDEX_ALLOCATION && ntfs_attr_name_is_i30(attribute, length)) allocation = attribute;
        offset += length;
    }
    if (!root || root->non_resident) return -EIO;
    u16 value_offset = le16((u8 *)&root->d.res.value_offset);
    u32 value_length = le32((u8 *)&root->d.res.value_length);
    if (value_offset > le32((u8 *)&root->length) || value_length < 32 || value_length > le32((u8 *)&root->length) - value_offset) return -EIO;
    u8 *root_header = (u8 *)root + value_offset + 0x10;
    if (!(root_header[12] & INDEX_ENTRY_NODE)) return ntfs_index_root_remove(mnt, record, file_reference, name, name_length);
    if (!allocation || !allocation->non_resident) return -EIO;
    u32 first = le32(root_header), total = le32(root_header + 4);
    u64 child_vcn         = UINT64_MAX;
    u32 root_match_offset = 0;
    for (u32 position = first; position + 24 <= total;) {
        u8 *entry  = root_header + position;
        u16 length = le16(entry + 8), key_length = le16(entry + 10), flags = le16(entry + 12);
        if (length < 24 || length > total - position || !(flags & INDEX_ENTRY_NODE)) return -EIO;
        if (flags & INDEX_ENTRY_END) {
            child_vcn = le64(entry + length - 8);
            break;
        }
        if (key_length < sizeof(fname_attr_t) || key_length > length - 24) return -EIO;
        fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
        u16           existing[255];
        if ((size_t)file_name->name_len * 2 > key_length - sizeof(fname_attr_t)) return -EIO;
        for (u32 i = 0; i < file_name->name_len; i++) existing[i] = le16((u8 *)file_name->name + (size_t)i * 2);
        int comparison = ntfs_utf16_compare(mnt, name, name_length, existing, file_name->name_len);
        if (!comparison) {
            if ((le64(entry) & 0x0000ffffffffffffULL) != (file_reference & 0x0000ffffffffffffULL)) return -ENOENT;
            u32 next_position = position + length;
            if (next_position + 24 > total) return -EIO;
            u8 *next        = root_header + next_position;
            u16 next_length = le16(next + 8), next_flags = le16(next + 12);
            if (next_length < 24 || next_length > total - next_position || !(next_flags & INDEX_ENTRY_NODE)) return -EIO;
            root_match_offset = (u32)(entry - record);
            child_vcn         = le64(next + next_length - 8);
            break;
        }
        if (comparison < 0) {
            child_vcn = le64(entry + length - 8);
            break;
        }
        position += length;
    }
    if (child_vcn == UINT64_MAX) return -EIO;
    u32 allocation_length = le32((u8 *)&allocation->length);
    u16 mapping_offset    = le16((u8 *)&allocation->d.nres.mapping_pairs_off);
    u64 stream_size       = le64((u8 *)&allocation->d.nres.data_size);
    u32 vcn_size          = ntfs_index_vcn_size(mnt);
    if (!vcn_size || child_vcn > UINT64_MAX / vcn_size) return -EIO;
    u64 block_offset = child_vcn * vcn_size;
    if (mapping_offset < 64 || mapping_offset >= allocation_length || block_offset > stream_size || mnt->indx_size > stream_size - block_offset)
        return -EIO;
    u8 *block = malloc(mnt->indx_size);
    if (!block) return -ENOMEM;
    int status = -EIO;
    if (read_by_runlist(mnt, (u8 *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block, mnt->indx_size,
                        stream_size)
            != (int)mnt->indx_size
        || ntfs_record_unpack(mnt, block, mnt->indx_size) < 0 || le32(block) != INDX_MAGIC)
        goto out;
    u8 *header = block + 0x18;
    first      = le32(header);
    total      = le32(header + 4);
    if (root_match_offset) {
        u8 *successor        = header + first;
        u16 successor_length = le16(successor + 8), successor_flags = le16(successor + 12);
        if (successor_length < 16 || successor_length > total - first || (successor_flags & INDEX_ENTRY_END)) goto out;
        u8 saved[16 + sizeof(fname_attr_t) + (size_t)255 * 2];
        if (successor_length > sizeof(saved)) {
            status = -EIO;
            goto out;
        }
        memcpy(saved, successor, successor_length);
        memmove(successor, successor + successor_length, 0x18 + total - (u32)(successor + successor_length - block));
        memset(block + 0x18 + total - successor_length, 0, successor_length);
        put_le32(header + 4, total - successor_length);
        status = ntfs_record_pack(mnt, block, mnt->indx_size);
        if (status < 0) goto out;
        status = write_by_runlist(mnt, (u8 *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block,
                                  mnt->indx_size, stream_size)
                         == (s64)mnt->indx_size ?
                     0 :
                     -EIO;
        if (status < 0) goto out;
        status = ntfs_index_root_replace_key(mnt, record, root_match_offset, saved);
        goto out;
    }
    for (u32 position = first; position + 16 <= total;) {
        u8 *entry  = header + position;
        u16 length = le16(entry + 8), key_length = le16(entry + 10), flags = le16(entry + 12);
        if (length < 16 || length > total - position) goto out;
        if (flags & INDEX_ENTRY_END) {
            status = -ENOENT;
            goto out;
        }
        if (key_length < sizeof(fname_attr_t) || key_length > length - 16) goto out;
        fname_attr_t *file_name = (fname_attr_t *)(entry + 16);
        int equal = (le64(entry) & 0x0000ffffffffffffULL) == (file_reference & 0x0000ffffffffffffULL) && file_name->name_len == name_length;
        for (u32 i = 0; equal && i < name_length; i++)
            if (le16((u8 *)file_name->name + (size_t)i * 2) != name[i]) equal = 0;
        if (equal) {
            memmove(entry, entry + length, 0x18 + total - (u32)(entry + length - block));
            memset(block + 0x18 + total - length, 0, length);
            put_le32(header + 4, total - length);
            status = ntfs_record_pack(mnt, block, mnt->indx_size);
            if (status < 0) goto out;
            status = write_by_runlist(mnt, (u8 *)allocation + mapping_offset, allocation_length - mapping_offset, block_offset, block,
                                      mnt->indx_size, stream_size)
                             == (s64)mnt->indx_size ?
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

static size_t ntfs_nonresident_grow(ntfs_handle_t *h, u8 *mft, attr_rec_t *attribute, u32 length, u16 mapping_offset, u64 offset,
                                    const u8 *buffer, size_t size, u64 data_size, u64 initialized_size)
{
    s64 vcn[256], lcn[256], run[256];
    s64 new_lcn[256], new_length[256];
    int run_count;
    int extent_count;
    u64 write_end       = offset + size;
    u64 allocated_size  = le64((u8 *)&attribute->d.nres.alloc_size);
    u64 allocated_count = (allocated_size + h->mnt->cluster_size - 1) >> h->mnt->cluster_bits;
    u64 required_count  = (write_end + h->mnt->cluster_size - 1) >> h->mnt->cluster_bits;
    u64 new_allocated_size;
    u8 *mapping;
    u8 *new_cache;
    int encoded_size;

    if (!h->mnt->bitmap_runlist || required_count > (u64)h->mnt->nr_clusters) return 0;
    run_count = runlist_parse((u8 *)attribute + mapping_offset, (int)(length - mapping_offset), vcn, lcn, run, 256);
    if (run_count <= 0 || (u64)(vcn[run_count - 1] + run[run_count - 1]) != allocated_count) return 0;

    /* Extending inside the already allocated last cluster needs no new run. */
    if (required_count <= allocated_count) {
        if ((offset > initialized_size
             && zero_by_runlist(h->mnt, (u8 *)attribute + mapping_offset, (int)(length - mapping_offset), initialized_size,
                                offset - initialized_size, allocated_size)
                    < 0)
            || write_by_runlist(h->mnt, (u8 *)attribute + mapping_offset, (int)(length - mapping_offset), offset, buffer, size, allocated_size)
                   != (s64)size)
            return 0;
        put_le64((u8 *)&attribute->d.nres.data_size, write_end);
        put_le64((u8 *)&attribute->d.nres.init_size, write_end);
        if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0 || mft_write(h->mnt, h->mft_no, mft) < 0) return 0;
        h->file_size = write_end;
        return size;
    }
    if (required_count - allocated_count > (u64)h->mnt->nr_clusters) return 0;
    if (bitmap_find_free(h->mnt, required_count - allocated_count, new_lcn, new_length, &extent_count) < 0) return 0;

    for (int extent = 0; extent < extent_count; extent++) {
        if (run_count && lcn[run_count - 1] + run[run_count - 1] == new_lcn[extent]) {
            run[run_count - 1] += new_length[extent];
        } else {
            if (run_count == 256) return 0;
            vcn[run_count] = (s64)allocated_count;
            for (int previous = 0; previous < extent; previous++) vcn[run_count] += new_length[previous];
            lcn[run_count] = new_lcn[extent];
            run[run_count] = new_length[extent];
            run_count++;
        }
    }

    mapping = calloc(1, length - mapping_offset);
    if (!mapping) return 0;
    encoded_size = runlist_encode(lcn, run, run_count, mapping, length - mapping_offset);
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
        || (offset > initialized_size
            && zero_by_runlist(h->mnt, mapping, encoded_size, initialized_size, offset - initialized_size,
                               required_count << h->mnt->cluster_bits)
                   < 0)
        || write_by_runlist(h->mnt, mapping, encoded_size, offset, buffer, size, required_count << h->mnt->cluster_bits) != (s64)size) {
        free(new_cache);
        free(mapping);
        return 0;
    }
    if (bitmap_change_extents(h->mnt, new_lcn, new_length, extent_count, 1) < 0) {
        bitmap_change_extents(h->mnt, new_lcn, new_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }

    memset((u8 *)attribute + mapping_offset, 0, length - mapping_offset);
    memcpy((u8 *)attribute + mapping_offset, mapping, (size_t)encoded_size);
    new_allocated_size = required_count << h->mnt->cluster_bits;
    put_le64((u8 *)&attribute->d.nres.highest_vcn, required_count - 1);
    put_le64((u8 *)&attribute->d.nres.alloc_size, new_allocated_size);
    put_le64((u8 *)&attribute->d.nres.data_size, write_end);
    put_le64((u8 *)&attribute->d.nres.init_size, write_end);
    if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0) {
        bitmap_change_extents(h->mnt, new_lcn, new_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }
    if (mft_write(h->mnt, h->mft_no, mft) < 0) {
        bitmap_change_extents(h->mnt, new_lcn, new_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }

    free(h->runlist_buf);
    h->runlist_buf = new_cache;
    h->runlist_sz  = (u32)encoded_size;
    h->file_size   = write_end;
    free(mapping);
    (void)data_size;
    return size;
}

static size_t ntfs_resident_convert(ntfs_handle_t *h, u8 *mft, attr_rec_t *attribute, u32 old_length, u16 value_offset, u32 value_length,
                                    u64 offset, const u8 *buffer, size_t size)
{
    s64 extent_lcn[256], extent_length[256];
    s64 runs[256];
    u64 write_end       = offset + size;
    u64 cluster_count   = (write_end + h->mnt->cluster_size - 1) >> h->mnt->cluster_bits;
    u32 attribute_off   = (u32)((u8 *)attribute - mft);
    u32 bytes_in_use    = le32(mft + 0x18);
    u32 bytes_allocated = le32(mft + 0x1c);
    u16 flags           = le16((u8 *)&attribute->flags);
    u16 instance        = le16((u8 *)&attribute->instance);
    int extent_count;
    int encoded_size;
    u32 new_length;
    u8 *mapping;
    u8 *new_cache;

    if (!h->mnt->bitmap_runlist || !cluster_count || bytes_in_use > bytes_allocated || bytes_allocated > h->mnt->mft_size
        || attribute_off + old_length > bytes_in_use)
        return 0;
    if (bitmap_find_free(h->mnt, cluster_count, extent_lcn, extent_length, &extent_count) < 0) return 0;
    for (int extent = 0; extent < extent_count; extent++) runs[extent] = extent_length[extent];

    mapping = calloc(1, h->mnt->mft_size);
    if (!mapping) return 0;
    encoded_size = runlist_encode(extent_lcn, runs, extent_count, mapping, h->mnt->mft_size);
    if (encoded_size < 0) {
        free(mapping);
        return 0;
    }
    new_length = (u32)ALIGN_UP(64 + encoded_size, 8);
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
        || (value_length
            && write_by_runlist(h->mnt, mapping, encoded_size, 0, (u8 *)attribute + value_offset, value_length,
                                cluster_count << h->mnt->cluster_bits)
                   != value_length)
        || write_by_runlist(h->mnt, mapping, encoded_size, offset, buffer, size, cluster_count << h->mnt->cluster_bits) != (s64)size) {
        free(new_cache);
        free(mapping);
        return 0;
    }
    if (bitmap_change_extents(h->mnt, extent_lcn, extent_length, extent_count, 1) < 0) {
        bitmap_change_extents(h->mnt, extent_lcn, extent_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }

    memmove(mft + attribute_off + new_length, mft + attribute_off + old_length, bytes_in_use - attribute_off - old_length);
    memset(attribute, 0, new_length);
    put_le32((u8 *)&attribute->type, AT_DATA);
    put_le32((u8 *)&attribute->length, new_length);
    attribute->non_resident = 1;
    put_le16((u8 *)&attribute->flags, flags);
    put_le16((u8 *)&attribute->instance, instance);
    put_le64((u8 *)&attribute->d.nres.lowest_vcn, 0);
    put_le64((u8 *)&attribute->d.nres.highest_vcn, cluster_count - 1);
    put_le16((u8 *)&attribute->d.nres.mapping_pairs_off, 64);
    put_le64((u8 *)&attribute->d.nres.alloc_size, cluster_count << h->mnt->cluster_bits);
    put_le64((u8 *)&attribute->d.nres.data_size, write_end);
    put_le64((u8 *)&attribute->d.nres.init_size, write_end);
    memcpy((u8 *)attribute + 64, mapping, (size_t)encoded_size);
    put_le32(mft + 0x18, bytes_in_use + new_length - old_length);
    if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0) {
        bitmap_change_extents(h->mnt, extent_lcn, extent_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }
    if (mft_write(h->mnt, h->mft_no, mft) < 0) {
        bitmap_change_extents(h->mnt, extent_lcn, extent_length, extent_count, 0);
        free(new_cache);
        free(mapping);
        return 0;
    }

    free(h->runlist_buf);
    h->runlist_buf = new_cache;
    h->runlist_sz  = (u32)encoded_size;
    h->file_size   = write_end;
    h->is_resident = 0;
    free(mapping);
    return size;
}

/* ---------- directory entry helpers ---------- */
static void add_dir_entry(vfs_node_t parent, ntfs_mount_t *mnt, u8 *entry, u32 entry_len)
{
    u64 mft_ref = le64(entry);

    /* validate FILE_NAME attr fits inside the entry */
    if (entry_len < 0x52) return; /* need at least: entry_hdr(0x10) + fname_hdr(0x42) */
    fname_attr_t *fna      = (fname_attr_t *)(entry + 0x10);
    u32           fn_bytes = (u32)fna->name_len * 2;
    if (fn_bytes > 510 || (u32)0x52 + fn_bytes > entry_len) return; /* filename overflow */

    u16 *fname16 = utf16_from((u8 *)entry, 0x52, fna->name_len);
    if (!fname16) return;
    char *fname8 = utf8_from_utf16(fname16, fna->name_len);
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
    ch->parent_mft_no = le64((u8 *)&fna->parent_dir) & 0x0000FFFFFFFFFFFFULL;
    ch->name          = fname16;
    ch->name_length   = fna->name_len;
    ch->is_dir        = (le32((u8 *)&fna->fa) & 0x10) ? 1 : 0;
    ch->file_size     = le64((u8 *)&fna->data_size);
    ch->file_attr     = le32((u8 *)&fna->fa);

    vfs_node_t child = vfs_node_alloc(parent, fname8);
    free(fname8);
    if (child) {
        child->handle = ch;
        child->type   = ch->is_dir ? file_dir : (le32((u8 *)&fna->rp_tag) == IO_REPARSE_TAG_SYMLINK ? file_symlink : file_none);
        child->size   = ch->file_size;
        /* Loaded nodes must use the same synchronous unlink path as newly
         * created nodes so namespace/metadata failures reach the caller. */
        child->flags |= VFS_NODE_DELETE_SYNC;
    } else {
        free(ch->name);
        free(ch);
    }
}

static int ntfs_load_directory(ntfs_handle_t *h, vfs_node_t node)
{
    ntfs_mount_t      *mnt        = h->mnt;
    ntfs_index_item_t *items      = NULL;
    u32                item_count = 0;
    u8                *mft        = malloc(mnt->mft_size);
    int                status;
    if (!mft) return -ENOMEM;
    if (mft_read(mnt, h->mft_no, mft) < 0) {
        free(mft);
        return -EIO;
    }

    mft_rec_t *mr = (mft_rec_t *)mft;
    if (mr->magic != MFT_MAGIC) {
        free(mft);
        return -EIO;
    }

    u32 off = le16((u8 *)&mr->attrs);

    while (off + 16 <= mnt->mft_size) {
        attr_rec_t *a  = (attr_rec_t *)(mft + off);
        u32         at = le32((u8 *)&a->type);
        u32         al = le32((u8 *)&a->length);
        if (at == AT_END || al < 16 || off + al > mnt->mft_size) break;

        if (at == AT_STANDARD_INFORMATION && !a->non_resident) {
            u32 voff = le16((u8 *)&a->d.res.value_offset);
            if (off + voff + 36 <= mnt->mft_size) h->file_attr = le32(mft + off + voff + 32);
        } else if (at == AT_DATA && a->name_length == 0) {
            u16 aflags = le16((u8 *)&a->flags);
            if (aflags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)) {
                h->file_size   = 0;
                h->is_resident = 0;
            } else if (a->non_resident) {
                h->file_size = le64((u8 *)&a->d.nres.data_size);
                u32 mp       = le16((u8 *)&a->d.nres.mapping_pairs_off);
                if (mp < al) {
                    h->runlist_sz = al - mp;
                    free(h->runlist_buf);
                    h->runlist_buf = malloc(h->runlist_sz);
                    if (h->runlist_buf) memcpy(h->runlist_buf, mft + off + mp, h->runlist_sz);
                }
                h->is_resident = 0;
            } else {
                h->file_size = le32((u8 *)&a->d.res.value_length);
                u16 voff     = le16((u8 *)&a->d.res.value_offset);
                if (voff + h->file_size <= al && h->file_size > 0) {
                    h->runlist_sz = (u32)h->file_size;
                    free(h->runlist_buf);
                    h->runlist_buf = malloc(h->runlist_sz);
                    if (h->runlist_buf) memcpy(h->runlist_buf, mft + off + voff, h->runlist_sz);
                }
                h->is_resident = 1;
            }
        }
        off += al;
    }

    /* Use the same validated collector as namespace mutation.  The previous
     * ad-hoc root/allocation parser could silently omit valid leaf entries,
     * leaving files present on disk but absent from the VFS child cache. */
    status = ntfs_directory_index_collect(mnt, mft, &items, &item_count, NULL);
    if (status < 0) {
        free(mft);
        return status;
    }
    for (u32 index = 0; index < item_count; index++) add_dir_entry(node, mnt, items[index].entry, items[index].length);
    ntfs_index_items_free(items, item_count);

    free(mft);
    h->dir_loaded = 1;
    node->visited = 1;
    return 0;
}

static int ntfs_load_file_runlist(ntfs_handle_t *h, vfs_node_t node)
{
    ntfs_mount_t *mnt = h->mnt;
    u8           *mft = malloc(mnt->mft_size);
    if (!mft) return -ENOMEM;
    if (mft_read(mnt, h->mft_no, mft) < 0) {
        free(mft);
        return -EIO;
    }

    mft_rec_t *mr = (mft_rec_t *)mft;
    if (mr->magic != MFT_MAGIC) {
        free(mft);
        return -EIO;
    }

    u32 off = le16((u8 *)&mr->attrs);
    while (off + 16 <= mnt->mft_size) {
        attr_rec_t *a  = (attr_rec_t *)(mft + off);
        u32         at = le32((u8 *)&a->type);
        u32         al = le32((u8 *)&a->length);
        if (at == AT_END || al < 16 || off + al > mnt->mft_size) break;
        if (at == AT_DATA && a->name_length == 0) {
            u16 aflags = le16((u8 *)&a->flags);
            if (aflags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)) {
                /* compressed/encrypted: refuse to return garbage */
                h->file_size = 0;
                node->size   = 0;
                break;
            }
            if (a->non_resident) {
                h->file_size = le64((u8 *)&a->d.nres.data_size);
                node->size   = h->file_size;
                u32 mp       = le16((u8 *)&a->d.nres.mapping_pairs_off);
                if (mp < al) {
                    h->runlist_sz  = al - mp;
                    h->runlist_buf = malloc(h->runlist_sz);
                    if (h->runlist_buf) memcpy(h->runlist_buf, mft + off + mp, h->runlist_sz);
                }
                h->is_resident = 0;
            } else {
                h->file_size = le32((u8 *)&a->d.res.value_length);
                node->size   = h->file_size;
                u16 voff     = le16((u8 *)&a->d.res.value_offset);
                if (voff + h->file_size <= al && h->file_size > 0) {
                    h->runlist_sz  = (u32)h->file_size;
                    h->runlist_buf = malloc(h->runlist_sz);
                    if (h->runlist_buf) memcpy(h->runlist_buf, mft + off + voff, h->runlist_sz);
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

/* ---------- VFS callbacks ---------- */
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

    u8 boot[512];
    if (blockdev_read_bytes(&dev, 0, boot, 512) < 0) {
        if (device_retained) blockdev_release(&dev);
        return -EIO;
    }

    ntfs_boot_sector_t *bs = (ntfs_boot_sector_t *)boot;
    if (le64((u8 *)&bs->oem_id) != magicNTFS) {
        if (device_retained) blockdev_release(&dev);
        return -EINVAL;
    }

    u16 bps = le16((u8 *)&bs->bpb.bytes_per_sector);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1))) {
        if (device_retained) blockdev_release(&dev);
        return -EINVAL;
    }

    s8  spc = bs->bpb.sectors_per_cluster;
    u32 sec_per_cluster;
    if ((u8)spc >= 0xf4)
        sec_per_cluster = 1U << -(s8)spc;
    else
        sec_per_cluster = (u32)(u8)spc;
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
    mnt->cluster_bits = __builtin_ctz(mnt->cluster_size);
    mnt->cluster_mask = mnt->cluster_size - 1;

    s8 cmr        = bs->clusters_per_mft_record;
    mnt->mft_size = cmr > 0 ? mnt->cluster_size << (__builtin_ffs(cmr) - 1) : (1U << -cmr);
    if (mnt->mft_size < 256 || mnt->mft_size > 65536) {
        if (device_retained) blockdev_release(&dev);
        free(mnt);
        return -EINVAL;
    }
    mnt->mft_bits = __builtin_ctz(mnt->mft_size);

    s8 cir         = bs->clusters_per_index_record;
    mnt->indx_size = cir > 0 ? mnt->cluster_size << (__builtin_ffs(cir) - 1) : (1U << -cir);
    if (mnt->indx_size < 512 || mnt->indx_size > 65536) {
        if (device_retained) blockdev_release(&dev);
        free(mnt);
        return -EINVAL;
    }

    mnt->mft_lcn     = le64((u8 *)&bs->mft_lcn);
    mnt->mftmirr_lcn = le64((u8 *)&bs->mftmirr_lcn);
    mnt->nr_clusters = le64((u8 *)&bs->number_of_sectors) >> (__builtin_ctz(sec_per_cluster));
    if (mnt->mft_lcn >= mnt->nr_clusters || mnt->mftmirr_lcn >= mnt->nr_clusters) {
        if (device_retained) blockdev_release(&dev);
        free(mnt);
        return -EINVAL;
    }

    /* blocks per cluster for index allocation VCN→LCN mapping */
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

    if (ntfs_prepare_write(mnt) < 0) plogk("ntfs: volume mounted with write path disabled.\n");

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

static void ntfs_vfs_unmount(void *root)
{
    ntfs_handle_t *h = root;
    if (!h) return;
    if (h->mnt->dirty_owned) {
        int status = ntfs_clear_owned_dirty(h->mnt);
        if (status != EOK) plogk("ntfs: sync failed; volume dirty flag was preserved.\n");
    }
    free(h->runlist_buf);
    free(h->mnt->upcase);
    free(h->mnt->bitmap_runlist);
    free(h->mnt->mft_runlist);
    if (h->mnt->transaction_log_initialized) fs_txn_log_destroy(&h->mnt->transaction_log);
    free(h->mnt);
    free(h);
}

static void ntfs_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    if (!node || !node->handle) return;
    ntfs_handle_t *h = node->handle;

    if (!h->is_dir && !h->runlist_buf) ntfs_load_file_runlist(h, node);

    if (h->is_dir && !h->dir_loaded) ntfs_load_directory(h, node);
}

static void ntfs_vfs_close(void *current)
{
    (void)current;
}

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
    int r = read_by_runlist(h->mnt, h->runlist_buf, h->runlist_sz, offset, addr, size, h->file_size);
    return r > 0 ? (size_t)r : 0;
}

static size_t ntfs_vfs_write_locked(ntfs_handle_t *h, const void *addr, size_t offset, size_t size)
{
    u8 *mft;
    u32 attr_offset;

    if (!size) return 0;
    if (!h || !h->mnt || !h->mnt->write_enabled || !addr || h->is_dir || h->mnt->dev.read_only || offset > UINT64_MAX - size) return 0;

    mft = malloc(h->mnt->mft_size);
    if (!mft) return 0;
    if (mft_read(h->mnt, h->mft_no, mft) < 0 || le32(mft) != MFT_MAGIC) {
        plogk("ntfs: drive %u: MFT read failed or bad magic for record %llu during write.\n", h->mnt->dev.drive, (unsigned long long)h->mft_no);
        free(mft);
        return 0;
    }

    attr_offset = le16(mft + 0x14);
    while (attr_offset + 24 <= h->mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(mft + attr_offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);

        if (type == AT_END) break;
        if (length < 24 || attr_offset + length > h->mnt->mft_size) break;
        if (type == AT_ATTRIBUTE_LIST) {
            plogk("ntfs: drive %u: write of record %llu with unsupported ATTRIBUTE_LIST.\n", h->mnt->dev.drive, (unsigned long long)h->mft_no);
            free(mft);
            return 0;
        }
        if (type == AT_DATA && !attribute->name_length) {
            u16 flags     = le16((u8 *)&attribute->flags);
            u64 write_end = (u64)offset + size;

            if (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) {
                plogk("ntfs: drive %u: write to record %llu with unsupported attribute flags 0x%04x\n", h->mnt->dev.drive,
                      (unsigned long long)h->mft_no, flags);
                free(mft);
                return 0;
            }

            if (attribute->non_resident) {
                u16 mapping_offset;
                u64 data_size;
                u64 initialized_size;
                s64 written;

                if (length < 64 || le64((u8 *)&attribute->d.nres.lowest_vcn) != 0) {
                    plogk("ntfs: drive %u: corrupt non-resident attribute in record %llu (corrupt runlist)\n", h->mnt->dev.drive,
                          (unsigned long long)h->mft_no);
                    free(mft);
                    return 0;
                }
                mapping_offset   = le16((u8 *)&attribute->d.nres.mapping_pairs_off);
                data_size        = le64((u8 *)&attribute->d.nres.data_size);
                initialized_size = le64((u8 *)&attribute->d.nres.init_size);
                if (mapping_offset < 64 || mapping_offset >= length || initialized_size > data_size) {
                    plogk("ntfs: drive %u: invalid attribute layout in record %llu\n", h->mnt->dev.drive, (unsigned long long)h->mft_no);
                    free(mft);
                    return 0;
                }
                if (write_end > data_size) {
                    size_t grown
                        = ntfs_nonresident_grow(h, mft, attribute, length, mapping_offset, offset, addr, size, data_size, initialized_size);
                    free(mft);
                    return grown;
                }

                if (write_end > initialized_size && offset > initialized_size
                    && zero_by_runlist(h->mnt, (u8 *)attribute + mapping_offset, (int)(length - mapping_offset), initialized_size,
                                       offset - initialized_size, data_size)
                           < 0) {
                    free(mft);
                    return 0;
                }
                written
                    = write_by_runlist(h->mnt, (u8 *)attribute + mapping_offset, (int)(length - mapping_offset), offset, addr, size, data_size);
                if (written != (s64)size) {
                    free(mft);
                    return 0;
                }
                if (write_end > initialized_size) put_le64((u8 *)&attribute->d.nres.init_size, write_end);
                if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0 || mft_write(h->mnt, h->mft_no, mft) < 0) {
                    free(mft);
                    return 0;
                }
                free(mft);
                return written > 0 ? (size_t)written : 0;
            }
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            u8 *new_cache;

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
                memset((u8 *)attribute + value_offset + value_length, 0, (size_t)write_end - value_length);
                value_length = (u32)write_end;
                put_le32((u8 *)&attribute->d.res.value_length, value_length);
            }
            memcpy((u8 *)attribute + value_offset + offset, addr, size);

            new_cache = malloc(value_length);
            if (!new_cache) {
                free(mft);
                return 0;
            }
            memcpy(new_cache, (u8 *)attribute + value_offset, value_length);
            if (ntfs_record_touch(mft, h->mnt->mft_size, 1) < 0 || mft_write(h->mnt, h->mft_no, mft) < 0) {
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

static int ntfs_vfs_resize_locked(ntfs_handle_t *h, u64 size)
{
    u8 *mft;
    u32 attr_offset;

    if (!h || !h->mnt || h->is_dir) return h && h->is_dir ? -EISDIR : -EINVAL;
    if (!h->mnt->write_enabled || h->mnt->dev.read_only) return -EROFS;
    if (size == h->file_size) return EOK;
    if (size > h->file_size) {
        u8 zero = 0;
        return ntfs_vfs_write_locked(h, &zero, size - 1, 1) == 1 ? EOK : -EIO;
    }

    mft = malloc(h->mnt->mft_size);
    if (!mft) return -ENOMEM;
    if (mft_read(h->mnt, h->mft_no, mft) < 0 || le32(mft) != MFT_MAGIC) {
        free(mft);
        return -EIO;
    }

    attr_offset = le16(mft + 0x14);
    while (attr_offset + 24 <= h->mnt->mft_size) {
        attr_rec_t *attribute = (attr_rec_t *)(mft + attr_offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
        int         status    = -EIO;

        if (type == AT_END) break;
        if (length < 24 || attr_offset + length > h->mnt->mft_size || type == AT_ATTRIBUTE_LIST) break;
        if (type != AT_DATA || attribute->name_length) {
            attr_offset += length;
            continue;
        }
        if (le16((u8 *)&attribute->flags) & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) {
            free(mft);
            return -EOPNOTSUPP;
        }

        if (!attribute->non_resident) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            u8 *new_cache    = NULL;

            if (value_offset > length || value_length > length - value_offset || size > value_length) break;
            if (size) {
                new_cache = malloc((size_t)size);
                if (!new_cache) {
                    free(mft);
                    return -ENOMEM;
                }
                memcpy(new_cache, (u8 *)attribute + value_offset, (size_t)size);
            }
            memset((u8 *)attribute + value_offset + size, 0, value_length - (u32)size);
            put_le32((u8 *)&attribute->d.res.value_length, (u32)size);
            status = ntfs_record_touch(mft, h->mnt->mft_size, 1);
            if (status == EOK) status = mft_write(h->mnt, h->mft_no, mft);
            if (status == EOK) {
                free(h->runlist_buf);
                h->runlist_buf = new_cache;
                h->runlist_sz  = (u32)size;
                h->file_size   = size;
                h->is_resident = 1;
                new_cache      = NULL;
            }
            free(new_cache);
            free(mft);
            return status;
        }

        if (length < 64 || le64((u8 *)&attribute->d.nres.lowest_vcn) != 0) break;
        u16 mapping_offset = le16((u8 *)&attribute->d.nres.mapping_pairs_off);
        u64 data_size      = le64((u8 *)&attribute->d.nres.data_size);
        u64 allocated_size = le64((u8 *)&attribute->d.nres.alloc_size);
        u64 initialized    = le64((u8 *)&attribute->d.nres.init_size);
        if (mapping_offset < 64 || mapping_offset >= length || size > data_size || initialized > data_size
            || allocated_size % h->mnt->cluster_size)
            break;

        s64 vcn[256], lcn[256], run[256];
        s64 kept_lcn[256], kept_run[256], free_lcn[256], free_run[256];
        int run_count       = runlist_parse((u8 *)attribute + mapping_offset, (int)(length - mapping_offset), vcn, lcn, run, 256);
        u64 allocated_count = allocated_size >> h->mnt->cluster_bits;
        u64 required_count  = (size + h->mnt->cluster_size - 1) >> h->mnt->cluster_bits;
        int kept_count = 0, free_count = 0;
        if (run_count <= 0 || (u64)(vcn[run_count - 1] + run[run_count - 1]) != allocated_count) break;
        for (int index = 0; index < run_count; index++) {
            if (lcn[index] < 0 || run[index] <= 0) break;
            u64 keep = 0;
            if ((u64)vcn[index] < required_count) {
                keep = required_count - (u64)vcn[index];
                if (keep > (u64)run[index]) keep = (u64)run[index];
            }
            if (keep) {
                kept_lcn[kept_count] = lcn[index];
                kept_run[kept_count] = (s64)keep;
                kept_count++;
            }
            if (keep < (u64)run[index]) {
                free_lcn[free_count] = lcn[index] + (s64)keep;
                free_run[free_count] = run[index] - (s64)keep;
                free_count++;
            }
        }
        if ((required_count && !kept_count) || free_count > 256) break;

        u8 *new_cache    = NULL;
        int encoded_size = 0;
        if (required_count) {
            new_cache = calloc(1, length - mapping_offset);
            if (!new_cache) {
                free(mft);
                return -ENOMEM;
            }
            encoded_size = runlist_encode(kept_lcn, kept_run, kept_count, new_cache, length - mapping_offset);
            if (encoded_size < 0) {
                free(new_cache);
                break;
            }
            u64 retained_size = required_count << h->mnt->cluster_bits;
            if (retained_size > size
                && zero_by_runlist(h->mnt, (u8 *)attribute + mapping_offset, (int)(length - mapping_offset), size, retained_size - size,
                                   allocated_size)
                       < 0) {
                free(new_cache);
                break;
            }
        }
        status = bitmap_change_extents(h->mnt, free_lcn, free_run, free_count, 0);
        if (status != EOK) {
            free(new_cache);
            free(mft);
            return status;
        }

        if (!required_count) {
            u32 bytes_in_use = le32(mft + 0x18);
            u16 flags        = le16((u8 *)&attribute->flags);
            u16 instance     = le16((u8 *)&attribute->instance);
            if (bytes_in_use > h->mnt->mft_size || attr_offset + length > bytes_in_use) {
                status = -EIO;
            } else {
                memmove(mft + attr_offset + 24, mft + attr_offset + length, bytes_in_use - attr_offset - length);
                memset(mft + bytes_in_use - (length - 24), 0, length - 24);
                attribute = (attr_rec_t *)(mft + attr_offset);
                memset(attribute, 0, 24);
                put_le32((u8 *)&attribute->type, AT_DATA);
                put_le32((u8 *)&attribute->length, 24);
                put_le16((u8 *)&attribute->flags, flags);
                put_le16((u8 *)&attribute->instance, instance);
                put_le16((u8 *)&attribute->d.res.value_offset, 24);
                put_le32(mft + 0x18, bytes_in_use - length + 24);
                status = ntfs_record_touch(mft, h->mnt->mft_size, 1);
            }
        } else {
            memset((u8 *)attribute + mapping_offset, 0, length - mapping_offset);
            memcpy((u8 *)attribute + mapping_offset, new_cache, (size_t)encoded_size);
            put_le64((u8 *)&attribute->d.nres.highest_vcn, required_count - 1);
            put_le64((u8 *)&attribute->d.nres.alloc_size, required_count << h->mnt->cluster_bits);
            put_le64((u8 *)&attribute->d.nres.data_size, size);
            put_le64((u8 *)&attribute->d.nres.init_size, initialized > size ? size : initialized);
            status = ntfs_record_touch(mft, h->mnt->mft_size, 1);
        }
        if (status == EOK) status = mft_write(h->mnt, h->mft_no, mft);
        if (status != EOK) {
            (void)bitmap_change_extents(h->mnt, free_lcn, free_run, free_count, 1);
        } else {
            free(h->runlist_buf);
            h->runlist_buf = new_cache;
            h->runlist_sz  = required_count ? (u32)encoded_size : 0;
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

static size_t ntfs_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    ntfs_handle_t *h = file;
    size_t         written;

    if (!h || !h->mnt) return 0;
#if NTFS_HOST_TEST
    written = ntfs_vfs_write_locked(h, addr, offset, size);
#else
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
    u64 credits = ((u64)size + h->mnt->dev.sector_size - 1) / h->mnt->dev.sector_size + 8192;
    if (credits > UINT32_MAX || ntfs_transaction_begin(h->mnt, &transaction, (u32)credits) != EOK) {
        spin_unlock(&h->mnt->write_lock);
        return 0;
    }
    written = ntfs_vfs_write_locked(h, addr, offset, size);
    status  = ntfs_transaction_finish(h->mnt, &transaction, written == size ? EOK : -EIO);
    if (status != EOK) written = 0;
    spin_unlock(&h->mnt->write_lock);
#endif
    return written;
}

static int ntfs_vfs_resize(void *file, u64 size)
{
    ntfs_handle_t *h = file;
#if NTFS_HOST_TEST
    return ntfs_vfs_resize_locked(h, size);
#else
    fs_txn_t transaction;
    int      status;

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
#endif
}
static size_t ntfs_vfs_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    ntfs_handle_t *handle;
    u8            *record;
    size_t         copied = 0;

    if (!n || !n->handle || !a || !s) return 0;
    handle = n->handle;
    record = malloc(handle->mnt->mft_size);
    if (!record) return 0;
    if (mft_read(handle->mnt, handle->mft_no, record) < 0) goto out;
    u32 offset = le16(record + 0x14);
    u32 used   = le32(record + 0x18);
    while (offset + 24 <= used) {
        attr_rec_t *attribute = (attr_rec_t *)(record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
        if (type == AT_END) break;
        if (length < 24 || offset > used - length) break;
        if (type == AT_REPARSE_POINT && !attribute->non_resident && !attribute->name_length) {
            u16 value_offset = le16((u8 *)&attribute->d.res.value_offset);
            u32 value_length = le32((u8 *)&attribute->d.res.value_length);
            if (value_offset > length || value_length < 20 || value_length > length - value_offset) break;
            u8 *value = (u8 *)attribute + value_offset;
            if (le32(value) != IO_REPARSE_TAG_SYMLINK || le16(value + 4) != value_length - 8) break;
            u16 substitute_offset = le16(value + 8);
            u16 substitute_length = le16(value + 10);
            if ((substitute_offset & 1) || (substitute_length & 1) || substitute_offset > value_length - 20
                || substitute_length > value_length - 20 - substitute_offset)
                break;
            u16  *target = utf16_from(value + 20, substitute_offset, substitute_length / 2);
            char *utf8   = utf8_from_utf16(target, substitute_length / 2);
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

static int ntfs_namespace_create_locked(ntfs_handle_t *parent, const char *source_name, vfs_node_t node, int directory)
{
    ntfs_mount_t  *mnt;
    ntfs_handle_t *child         = NULL;
    u8            *parent_record = NULL;
    u8            *child_record  = NULL;
    u16           *name          = NULL;
    u8             name_length;
    u64            record_number;
    u64            parent_reference;
    u64            child_reference;
    u16            child_sequence = 1;
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
    child->name = malloc((u32)name_length * sizeof(u16));
    if (!child->name) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(child->name, name, (u32)name_length * sizeof(u16));

    if (mft_read(mnt, parent->mft_no, parent_record) < 0 || le32(parent_record) != MFT_MAGIC || !(le16(parent_record + 0x16) & 2)) {
        status = -EIO;
        goto out;
    }
    status = ntfs_mft_bitmap_find_free(mnt, &record_number);
    if (status < 0) goto out;
    parent_reference = parent->mft_no | ((u64)le16(parent_record + 0x10) << 48);
    child_reference  = record_number | ((u64)child_sequence << 48);
    status           = ntfs_build_file_record(mnt, child_record, record_number, child_sequence, parent_reference, name, name_length, directory);
    if (status < 0) goto out;
    status = mft_write(mnt, record_number, child_record);
    if (status < 0) goto out;
    status = ntfs_mft_bitmap_set(mnt, record_number, 1);
    if (status < 0) goto out;
    status
        = ntfs_directory_index_insert(mnt, parent_record, child_reference, parent_reference, name, name_length, directory ? 0x10 : 0x20, 0, 0);
    if (status < 0) {
        ntfs_mft_bitmap_set(mnt, record_number, 0);
        goto out;
    }
    status = mft_write(mnt, parent->mft_no, parent_record);
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

static int ntfs_namespace_create(ntfs_handle_t *parent, const char *name, vfs_node_t node, int directory)
{
    int status;

    if (!parent || !parent->mnt) return -EINVAL;
#if NTFS_HOST_TEST
    status = ntfs_namespace_create_locked(parent, name, node, directory);
#else
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
#endif
    return status;
}

static int ntfs_namespace_hardlink_locked(ntfs_handle_t *parent, ntfs_handle_t *target, const char *source_name, vfs_node_t node)
{
    ntfs_mount_t  *mnt;
    ntfs_handle_t *link          = NULL;
    u8            *parent_record = NULL;
    u8            *target_record = NULL;
    u8            *old_record    = NULL;
    u16           *name          = NULL;
    u8             name_length;
    u64            parent_reference;
    u64            target_reference;
    u16            links;
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
    link->name = malloc((u32)name_length * sizeof(u16));
    if (!link->name) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(link->name, name, (u32)name_length * sizeof(u16));
    if (mft_read(mnt, parent->mft_no, parent_record) < 0 || mft_read(mnt, target->mft_no, target_record) < 0) {
        status = -EIO;
        goto out;
    }
    links = le16(target_record + 0x12);
    if (!links || links == UINT16_MAX || !(le16(target_record + 0x16) & 1)) {
        status = -EIO;
        goto out;
    }
    memcpy(old_record, target_record, mnt->mft_size);
    parent_reference = parent->mft_no | ((u64)le16(parent_record + 0x10) << 48);
    target_reference = target->mft_no | ((u64)le16(target_record + 0x10) << 48);
    status           = ntfs_file_name_add(mnt, target_record, parent_reference, name, name_length, target->file_attr, target->file_size);
    if (status < 0) goto out;
    put_le16(target_record + 0x12, links + 1);
    status = mft_write(mnt, target->mft_no, target_record);
    if (status < 0) goto out;
    status = ntfs_directory_index_insert(mnt, parent_record, target_reference, parent_reference, name, name_length, target->file_attr,
                                         target->file_size, target->file_size);
    if (status < 0) {
        mft_write(mnt, target->mft_no, old_record);
        goto out;
    }
    status = mft_write(mnt, parent->mft_no, parent_record);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, target_reference, name, name_length);
        ntfs_index_reclaim_abort(mnt);
        mft_write(mnt, target->mft_no, old_record);
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

static int ntfs_namespace_hardlink(ntfs_handle_t *parent, ntfs_handle_t *target, const char *name, vfs_node_t node)
{
    if (!parent || !parent->mnt) return -EINVAL;
#if NTFS_HOST_TEST
    return ntfs_namespace_hardlink_locked(parent, target, name, node);
#else
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
#endif
}

static int ntfs_namespace_symlink_locked(ntfs_handle_t *parent, const char *source_name, const char *source_target, vfs_node_t node)
{
    ntfs_mount_t  *mnt;
    ntfs_handle_t *link          = NULL;
    u8            *parent_record = NULL;
    u8            *link_record   = NULL;
    u16           *name          = NULL;
    u16           *target        = NULL;
    u8             name_length;
    u8             target_length;
    u64            record_number;
    u64            parent_reference;
    u64            link_reference;
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
               && !(((source_target[0] >= 'A' && source_target[0] <= 'Z') || (source_target[0] >= 'a' && source_target[0] <= 'z'))
                    && source_target[1] == ':');
    parent_record = malloc(mnt->mft_size);
    link_record   = malloc(mnt->mft_size);
    link          = calloc(1, sizeof(*link));
    if (!parent_record || !link_record || !link) {
        status = -ENOMEM;
        goto out;
    }
    link->name = malloc((u32)name_length * sizeof(u16));
    if (!link->name) {
        status = -ENOMEM;
        goto out;
    }
    memcpy(link->name, name, (u32)name_length * sizeof(u16));
    if (mft_read(mnt, parent->mft_no, parent_record) < 0 || !(le16(parent_record + 0x16) & 2)) {
        status = -EIO;
        goto out;
    }
    status = ntfs_mft_bitmap_find_free(mnt, &record_number);
    if (status < 0) goto out;
    parent_reference = parent->mft_no | ((u64)le16(parent_record + 0x10) << 48);
    link_reference   = record_number | (1ULL << 48);
    status = ntfs_build_symlink_record(mnt, link_record, record_number, 1, parent_reference, name, name_length, target, target_length, relative);
    if (status < 0) goto out;
    status = mft_write(mnt, record_number, link_record);
    if (status < 0) goto out;
    status = ntfs_mft_bitmap_set(mnt, record_number, 1);
    if (status < 0) goto out;
    status = ntfs_directory_index_insert(mnt, parent_record, link_reference, parent_reference, name, name_length,
                                         0x20 | FILE_ATTRIBUTE_REPARSE_POINT, 0, 0);
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
    status = mft_write(mnt, parent->mft_no, parent_record);
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

static int ntfs_namespace_symlink(ntfs_handle_t *parent, const char *name, const char *target, vfs_node_t node)
{
#if NTFS_HOST_TEST
    return ntfs_namespace_symlink_locked(parent, name, target, node);
#else
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
#endif
}

static int ntfs_vfs_mkdir(void *p, const char *n, vfs_node_t nd)
{
    return ntfs_namespace_create(p, n, nd, 1);
}
static int ntfs_vfs_mkfile(void *p, const char *n, vfs_node_t nd)
{
    return ntfs_namespace_create(p, n, nd, 0);
}
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
static int ntfs_vfs_symlink(void *p, const char *target_name, vfs_node_t node)
{
    if (!node || !node->name) return -EINVAL;
    return ntfs_namespace_symlink(p, node->name, target_name, node);
}
static int ntfs_vfs_stat(void *file, vfs_node_t nd)
{
    ntfs_handle_t *h = file;
    if (!h || !nd) return -EINVAL;

    /* VFS nodes materialized from a directory index already have a handle, so
     * traversal invokes stat rather than open.  Populate nested directories
     * here as well as regular-file data, otherwise only the mount root gets a
     * child cache and existing nested files disappear from pathname lookup. */
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
static int ntfs_vfs_ioctl(void *f, size_t r, void *a)
{
    (void)f;
    (void)r;
    (void)a;
    return -ENOTTY;
}

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

static int ntfs_vfs_delete(void *p, vfs_node_t n)
{
    ntfs_handle_t *parent = p;
    ntfs_handle_t *child;
    ntfs_mount_t  *mnt;
    u8            *parent_record;
    u8            *child_record;
    u8            *old_child_record;
    s64            data_lcn[256];
    s64            data_length[256];
    s64            index_lcn[256];
    s64            index_length[256];
    int            data_extents        = 0;
    int            index_extents       = 0;
    int            namespace_committed = 0;
    int            status;
#if !NTFS_HOST_TEST
    fs_txn_t transaction;
    int      transaction_started = 0;
#endif

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

#if !NTFS_HOST_TEST
    spin_lock(&mnt->write_lock);
    if (!mnt->dirty_owned) {
        status = ntfs_set_volume_dirty(mnt, 1);
        if (status < 0) goto unlock;
        mnt->dirty_owned = 1;
    }
    status = ntfs_transaction_begin(mnt, &transaction, 65536);
    if (status < 0) goto unlock;
    transaction_started = 1;
#endif
    if (mft_read(mnt, parent->mft_no, parent_record) < 0 || mft_read(mnt, child->mft_no, child_record) < 0) {
        status = -EIO;
        goto unlock;
    }
    u16 link_count = le16(child_record + 0x12);
    if (!(le16(child_record + 0x16) & 1) || !link_count) {
        status = -EIO;
        goto unlock;
    }
    if (link_count > 1) {
        u64 parent_reference = parent->mft_no | ((u64)le16(parent_record + 0x10) << 48);
        memcpy(old_child_record, child_record, mnt->mft_size);
        status = ntfs_directory_index_remove(mnt, parent_record, child->mft_no, child->name, child->name_length);
        if (status < 0) goto unlock;
        status = ntfs_file_name_remove(mnt, child_record, parent_reference, child->name, child->name_length);
        if (status < 0) goto unlock;
        put_le16(child_record + 0x12, link_count - 1);
        status = mft_write(mnt, child->mft_no, child_record);
        if (status < 0) goto unlock;
        status = mft_write(mnt, parent->mft_no, parent_record);
        if (status < 0) {
            mft_write(mnt, child->mft_no, old_child_record);
        } else {
            namespace_committed = 1;
            status              = ntfs_index_reclaim_commit(mnt);
        }
        goto unlock;
    }
    if (child->is_dir) {
        ntfs_index_attributes_t attributes;
        ntfs_index_item_t      *items      = NULL;
        u32                     item_count = 0;
        status                             = ntfs_directory_index_collect(mnt, child_record, &items, &item_count, &attributes);
        ntfs_index_items_free(items, item_count);
        if (status < 0) goto unlock;
        if (item_count) {
            status = -ENOTEMPTY;
            goto unlock;
        }
        if (attributes.allocation) {
            u32 allocation_length = le32((u8 *)&attributes.allocation->length);
            u16 mapping_offset    = le16((u8 *)&attributes.allocation->d.nres.mapping_pairs_off);
            s64 vcn[256];
            if (!attributes.allocation->non_resident || mapping_offset < 64 || mapping_offset >= allocation_length) {
                status = -EIO;
                goto unlock;
            }
            index_extents = runlist_parse((u8 *)attributes.allocation + mapping_offset, (int)(allocation_length - mapping_offset), vcn,
                                          index_lcn, index_length, 256);
            if (index_extents <= 0) {
                status = -EIO;
                goto unlock;
            }
        }
    }
    u32 bytes_in_use = le32(child_record + 0x18);
    u32 offset       = le16(child_record + 0x14);
    while (offset + 24 <= bytes_in_use) {
        attr_rec_t *attribute = (attr_rec_t *)(child_record + offset);
        u32         type      = le32((u8 *)&attribute->type);
        u32         length    = le32((u8 *)&attribute->length);
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
                u16 flags          = le16((u8 *)&attribute->flags);
                u16 mapping_offset = le16((u8 *)&attribute->d.nres.mapping_pairs_off);
                s64 vcn[256];
                if (flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE) || length < 64 || mapping_offset < 64
                    || mapping_offset >= length || data_extents) {
                    status = -EOPNOTSUPP;
                    goto unlock;
                }
                data_extents = runlist_parse((u8 *)attribute + mapping_offset, (int)(length - mapping_offset), vcn, data_lcn, data_length, 256);
                if (data_extents <= 0) {
                    status = -EIO;
                    goto unlock;
                }
                for (int i = 0; i < data_extents; i++) {
                    if (data_lcn[i] < 0 || data_length[i] <= 0 || data_lcn[i] >= mnt->nr_clusters
                        || data_length[i] > mnt->nr_clusters - data_lcn[i]) {
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
    status = mft_write(mnt, parent->mft_no, parent_record);
    if (status < 0) goto unlock;
    namespace_committed = 1;
    status              = ntfs_index_reclaim_commit(mnt);
    if (status < 0) goto unlock;
    if (data_extents) {
        status = bitmap_change_extents(mnt, data_lcn, data_length, data_extents, 0);
        if (status < 0) goto unlock;
    }
    if (index_extents) {
        status = bitmap_change_extents(mnt, index_lcn, index_length, index_extents, 0);
        if (status < 0) {
            if (data_extents) bitmap_change_extents(mnt, data_lcn, data_length, data_extents, 1);
            goto unlock;
        }
    }
    put_le16(child_record + 0x12, 0);
    put_le16(child_record + 0x16, le16(child_record + 0x16) & (u16)~1U);
    status = mft_write(mnt, child->mft_no, child_record);
    if (status < 0) goto unlock;
    status = ntfs_mft_bitmap_set(mnt, child->mft_no, 0);
unlock:
    if (status < 0 && !namespace_committed) ntfs_index_reclaim_abort(mnt);
#if !NTFS_HOST_TEST
    if (transaction_started) status = ntfs_transaction_finish(mnt, &transaction, status);
    spin_unlock(&mnt->write_lock);
#endif
    free(child_record);
    free(old_child_record);
    free(parent_record);
    return status;
}
static int ntfs_vfs_rename(void *c, const char *nn)
{
    ntfs_handle_t *child = c;
    ntfs_mount_t  *mnt;
    u8            *parent_record     = NULL;
    u8            *old_parent_record = NULL;
    u8            *child_record      = NULL;
    u8            *old_record        = NULL;
    u16           *new_name          = NULL;
    u8             new_length;
    u64            parent_reference;
    u64            child_reference;
    int            namespace_committed = 0;
    int            status;
#if !NTFS_HOST_TEST
    fs_txn_t transaction;
    int      transaction_started = 0;
#endif

    if (!child || !child->mnt || !nn || !child->name || !child->name_length) return -EINVAL;
    mnt = child->mnt;
    if (!mnt->write_enabled || mnt->dev.read_only) return -EROFS;
    status = ntfs_name_from_utf8(nn, &new_name, &new_length);
    if (status < 0) return status;
    if (new_length == child->name_length) {
        int identical = 1;
        for (u32 index = 0; index < new_length; index++)
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
#if !NTFS_HOST_TEST
    spin_lock(&mnt->write_lock);
    if (!mnt->dirty_owned) {
        status = ntfs_set_volume_dirty(mnt, 1);
        if (status < 0) goto unlock_rename;
        mnt->dirty_owned = 1;
    }
    status = ntfs_transaction_begin(mnt, &transaction, 65536);
    if (status < 0) goto unlock_rename;
    transaction_started = 1;
#endif
    if (mft_read(mnt, child->parent_mft_no, parent_record) < 0 || mft_read(mnt, child->mft_no, child_record) < 0) {
        status = -EIO;
        goto unlock_rename;
    }
    memcpy(old_record, child_record, mnt->mft_size);
    memcpy(old_parent_record, parent_record, mnt->mft_size);
    parent_reference = child->parent_mft_no | ((u64)le16(parent_record + 0x10) << 48);
    child_reference  = child->mft_no | ((u64)le16(child_record + 0x10) << 48);
    status           = ntfs_directory_index_remove(mnt, parent_record, child->mft_no, child->name, child->name_length);
    if (status < 0) goto unlock_rename;
    status = ntfs_directory_index_insert(mnt, parent_record, child_reference, parent_reference, new_name, new_length, child->file_attr,
                                         child->file_size, child->file_size);
    if (status < 0) {
        ntfs_directory_index_insert(mnt, old_parent_record, child_reference, parent_reference, child->name, child->name_length, child->file_attr,
                                    child->file_size, child->file_size);
        ntfs_index_reclaim_abort(mnt);
        goto unlock_rename;
    }
    status = ntfs_file_name_replace(mnt, child_record, parent_reference, child->name, child->name_length, new_name, new_length);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, child->mft_no, new_name, new_length);
        ntfs_directory_index_insert(mnt, old_parent_record, child_reference, parent_reference, child->name, child->name_length, child->file_attr,
                                    child->file_size, child->file_size);
        ntfs_index_reclaim_abort(mnt);
        goto unlock_rename;
    }
    status = mft_write(mnt, child->mft_no, child_record);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, child->mft_no, new_name, new_length);
        ntfs_directory_index_insert(mnt, old_parent_record, child_reference, parent_reference, child->name, child->name_length, child->file_attr,
                                    child->file_size, child->file_size);
        ntfs_index_reclaim_abort(mnt);
        mft_write(mnt, child->mft_no, old_record);
        goto unlock_rename;
    }
    status = mft_write(mnt, child->parent_mft_no, parent_record);
    if (status < 0) {
        ntfs_directory_index_remove(mnt, parent_record, child->mft_no, new_name, new_length);
        ntfs_directory_index_insert(mnt, old_parent_record, child_reference, parent_reference, child->name, child->name_length, child->file_attr,
                                    child->file_size, child->file_size);
        ntfs_index_reclaim_abort(mnt);
        mft_write(mnt, child->mft_no, old_record);
        goto unlock_rename;
    }
    namespace_committed = 1;
    status              = ntfs_index_reclaim_commit(mnt);
    if (status < 0) goto unlock_rename;
unlock_rename:
    if (status < 0 && !namespace_committed) ntfs_index_reclaim_abort(mnt);
#if !NTFS_HOST_TEST
    if (transaction_started) status = ntfs_transaction_finish(mnt, &transaction, status);
    spin_unlock(&mnt->write_lock);
#endif
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

#if CONFIG_NTFS_FS
static struct vfs_callback ntfs_cb = {
    .mount    = ntfs_vfs_mount,
    .unmount  = ntfs_vfs_unmount,
    .open     = ntfs_vfs_open,
    .close    = ntfs_vfs_close,
    .read     = ntfs_vfs_read,
    .write    = ntfs_vfs_write,
    .resize   = ntfs_vfs_resize,
    .readlink = ntfs_vfs_readlink,
    .mkdir    = ntfs_vfs_mkdir,
    .mkfile   = ntfs_vfs_mkfile,
    .link     = ntfs_vfs_link,
    .symlink  = ntfs_vfs_symlink,
    .stat     = ntfs_vfs_stat,
    .ioctl    = ntfs_vfs_ioctl,
    .free     = ntfs_vfs_free,
    .delete   = ntfs_vfs_delete,
    .rename   = ntfs_vfs_rename,
};

int ntfs_vfs_regist(void)
{
    int id = vfs_regist_fs("ntfs", &ntfs_cb);
    if (id & ERRNO_MASK) {
        plogk("ntfs: failed to register filesystem.\n");
        return -EINVAL;
    }
    plogk("ntfs: Filesystem registered (id=%d)\n", id);
    return 0;
}
#else
int ntfs_vfs_regist(void)
{
    return -EINVAL;
}
#endif
