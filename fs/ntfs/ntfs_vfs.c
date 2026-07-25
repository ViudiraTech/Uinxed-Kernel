/*
 *
 *      ntfs_vfs.c
 *      New Technology File System
 *
 *      2026/7/25 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/blockdev.h>
#include <fs/ntfs/ntfs_vfs.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int64_t  s64;

#define magicNTFS 0x202020205346544eULL

#define AT_STANDARD_INFORMATION 0x10
#define AT_FILE_NAME            0x30
#define AT_DATA                 0x80
#define AT_INDEX_ROOT           0x90
#define AT_INDEX_ALLOCATION     0xa0
#define AT_END                  0xffffffff
#define ATTR_COMPRESSED_MASK    0x00ff
#define ATTR_IS_COMPRESSED      0x0001
#define ATTR_IS_ENCRYPTED       0x4000
#define ATTR_IS_SPARSE          0x8000

#define MFT_MAGIC  0x454c4946
#define INDX_MAGIC 0x58444E49

#define LCN_HOLE ((s64) - 2)

/* on-disk layout structs — packed because NTFS has no natural alignment */
struct ntfs_boot_sector {
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
} __attribute__((packed));

struct mft_rec {
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
} __attribute__((packed));

struct attr_rec {
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
} __attribute__((packed));

struct fname_attr {
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
} __attribute__((packed));

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
        s64               nr_clusters;
} ntfs_mount_t;

typedef struct {
        ntfs_mount_t *mnt;
        u64           mft_no;
        u64           file_size;
        u32           file_attr;
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

/* ---------- utf16 → utf8 ---------- */
static u16 *utf16_from(const u8 *buf, int ofs, int len)
{
    if (len <= 0 || len > 255) return NULL;
    u16 *out = calloc(len + 1, sizeof(u16));
    if (!out) return NULL;
    for (int i = 0; i < len; i++) out[i] = le16(buf + ofs + i * 2);
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

/* ---------- disk I/O helpers ---------- */
static int mft_read(ntfs_mount_t *mnt, u64 mft_no, u8 *buf)
{
    if (!mnt->cluster_size) return -EIO;
    u64 cl       = mnt->mft_lcn + (mft_no * mnt->mft_size) / mnt->cluster_size;
    u64 off      = (mft_no * mnt->mft_size) % mnt->cluster_size;
    u64 byte_off = (cl << mnt->cluster_bits) + off;
    if (blockdev_read_bytes(&mnt->dev, byte_off, buf, mnt->mft_size) < 0) return -EIO;
    return 0;
}

static int dev_read(ntfs_mount_t *mnt, u64 byte_off, u8 *buf, size_t size)
{
    return blockdev_read_bytes(&mnt->dev, byte_off, buf, size);
}

/* ---------- runlist parser ---------- */
static int runlist_parse(u8 *rl, int len, s64 *vcn, s64 *lcn, s64 *run, int max)
{
    int off = 0, cnt = 0;
    s64 cur_vcn = 0, cur_lcn = 0;
    while (off < len && cnt < max && rl[off] != 0) {
        u8  h  = rl[off++];
        int lb = h & 0x0F, ob = (h >> 4) & 0x0F;
        if (lb == 0 && ob == 0) break;
        if (lb > 8 || ob > 8) break;
        if (off + lb + ob > len) break;

        s64 rlen = 0;
        for (int i = 0; i < lb; i++) rlen |= (s64)rl[off++] << (i * 8);
        if (lb > 0 && lb < 8 && (rl[off - 1] & 0x80)) {
            s64 mask = (s64)((1ULL << (lb * 8)) - 1);
            rlen     = (rlen & mask) | (~mask);
        }

        if (ob == 0) {
            /* sparse / hole run: no LCN delta */
            vcn[cnt] = cur_vcn;
            lcn[cnt] = LCN_HOLE;
            run[cnt] = rlen;
        } else {
            s64 roff = 0;
            for (int i = 0; i < ob; i++) roff |= (s64)rl[off++] << (i * 8);
            if (ob < 8 && (rl[off - 1] & 0x80)) {
                s64 mask = (s64)((1ULL << (ob * 8)) - 1);
                roff     = (roff & mask) | (~mask);
            }
            cur_lcn += roff;
            vcn[cnt] = cur_vcn;
            lcn[cnt] = cur_lcn;
            run[cnt] = rlen;
        }
        cur_vcn += rlen;
        cnt++;
    }
    return cnt;
}

static int read_by_runlist(ntfs_mount_t *mnt, u8 *rl, int rl_len, u64 offset, u8 *buf, size_t size, u64 max_size)
{
    s64 v[256], l[256], r[256];
    int n = runlist_parse(rl, rl_len, v, l, r, 256);
    if (n <= 0 || !size || offset >= max_size) return 0;

    size_t done = 0;
    while (done < size && offset + done < max_size) {
        s64 pos       = (s64)(offset + done);
        s64 clu       = pos >> mnt->cluster_bits;
        s64 found_lcn = -1, hole_end = 0;
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (clu >= v[i] && clu < v[i] + r[i]) {
                found = 1;
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
            if (dev_read(mnt, (u64)byte_off, buf + done, (size_t)to_read) < 0) return done > 0 ? (int)done : -EIO;
            done += (size_t)to_read;
        }
    }
    return (int)done;
}

/* ---------- directory entry helpers ---------- */
static void add_dir_entry(vfs_node_t parent, ntfs_mount_t *mnt, u8 *entry, u32 entry_len)
{
    u64 mft_ref = le64(entry);

    /* validate FILE_NAME attr fits inside the entry */
    if (entry_len < 0x52) return; /* need at least: entry_hdr(0x10) + fname_hdr(0x42) */
    struct fname_attr *fna      = (struct fname_attr *)(entry + 0x10);
    u32                fn_bytes = (u32)fna->name_len * 2;
    if (fn_bytes > 510 || (u32)0x52 + fn_bytes > entry_len) return; /* filename overflow */

    u16 *fname16 = utf16_from((u8 *)entry, 0x52, fna->name_len);
    if (!fname16) return;
    char *fname8 = utf8_from_utf16(fname16, fna->name_len);
    free(fname16);
    if (!fname8) return;

    if (!strcmp(fname8, ".") || !strcmp(fname8, "..")) {
        free(fname8);
        return;
    }
    if (vfs_do_search(parent, fname8)) {
        free(fname8);
        return;
    }

    ntfs_handle_t *ch = calloc(1, sizeof(ntfs_handle_t));
    if (!ch) {
        free(fname8);
        return;
    }
    ch->mnt       = mnt;
    ch->mft_no    = mft_ref & 0x0000FFFFFFFFFFFFULL;
    ch->is_dir    = (le32((u8 *)&fna->fa) & 0x10) ? 1 : 0;
    ch->file_size = le64((u8 *)&fna->data_size);
    ch->file_attr = le32((u8 *)&fna->fa);

    vfs_node_t child = vfs_node_alloc(parent, fname8);
    free(fname8);
    if (child) {
        child->handle = ch;
        child->type   = ch->is_dir ? file_dir : file_none;
        child->size   = ch->file_size;
    } else {
        free(ch);
    }
}

static void parse_index_buf(vfs_node_t parent, ntfs_mount_t *mnt, u8 *buf, u32 buf_size, u32 hdr_off)
{
    /* hdr_off: byte offset of INDEX_HEADER within buf
	 *   INDEX_ROOT value: hdr_off = 0x10
	 *   INDEX_ALLOCATION block: hdr_off = 0x18 */
    if (buf_size < hdr_off + 16) return;
    u32 entry_start = le32(buf + hdr_off) + hdr_off;
    if (entry_start >= buf_size) return;
    u32 pos = entry_start;
    while (pos + 0x10 <= buf_size) {
        u8 *e      = buf + pos;
        u16 elen   = le16(e + 8);
        u16 eflags = le16(e + 12);
        if (elen < 0x10 || (u32)(pos + elen) > buf_size) break;
        add_dir_entry(parent, mnt, e, elen);
        if (eflags & 0x01) break;
        pos += elen;
    }
}

static void load_indx_blocks(vfs_node_t parent, ntfs_mount_t *mnt, u8 *rl, int rl_len)
{
    u32 ibs = mnt->indx_size;
    if (ibs < 0x40 || ibs > 65536) return;

    u32 bpc = mnt->indx_vcn_per_cluster;
    if (bpc == 0) bpc = 1;

    s64 v[256], l[256], r[256];
    int n = runlist_parse(rl, rl_len, v, l, r, 256);
    if (n <= 0) return;

    u8 *buf = malloc(ibs);
    if (!buf) return;

    for (int i = 0; i < n; i++) {
        if (l[i] < 0) continue;
        s64 total_blocks = r[i];
        if (total_blocks > 0x100000) total_blocks = 0x100000; /* sanity cap */
        for (s64 blk = 0; blk < total_blocks; blk++) {
            s64 clu        = l[i] + blk / (s64)bpc;
            u32 off_in_clu = (u32)((blk % (s64)bpc) * ibs);
            u64 byte_off   = ((u64)clu << mnt->cluster_bits) + off_in_clu;

            if (dev_read(mnt, byte_off, buf, ibs) < 0) continue;
            if (le32(buf) != INDX_MAGIC) continue;
            /* skip intermediate B-tree nodes (they duplicate leaf entries) */
            if (buf[0x18 + 12] & 0x01) continue;
            parse_index_buf(parent, mnt, buf, ibs, 0x18);
        }
    }
    free(buf);
}

static int ntfs_load_directory(ntfs_handle_t *h, vfs_node_t node)
{
    ntfs_mount_t *mnt = h->mnt;
    u8           *mft = malloc(mnt->mft_size);
    if (!mft) return -ENOMEM;
    if (mft_read(mnt, h->mft_no, mft) < 0) {
        free(mft);
        return -EIO;
    }

    struct mft_rec *mr = (struct mft_rec *)mft;
    if (mr->magic != MFT_MAGIC) {
        free(mft);
        return -EIO;
    }

    u32 off       = le16((u8 *)&mr->attrs);
    u8 *ia_rl     = NULL;
    u32 ia_rl_len = 0;

    while (off + 16 <= mnt->mft_size) {
        struct attr_rec *a  = (struct attr_rec *)(mft + off);
        u32              at = le32((u8 *)&a->type);
        u32              al = le32((u8 *)&a->length);
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
        } else if (at == AT_INDEX_ROOT && a->name_length == 0) {
            u32 voff = le16((u8 *)&a->d.res.value_offset);
            u32 vsz  = le32((u8 *)&a->d.res.value_length);
            if (off + voff + vsz <= mnt->mft_size) parse_index_buf(node, mnt, mft + off + voff, vsz, 0x10);
        } else if (at == AT_INDEX_ALLOCATION && a->name_length == 0 && a->non_resident) {
            u16 mp = le16((u8 *)&a->d.nres.mapping_pairs_off);
            if (mp < al) {
                ia_rl_len = al - mp;
                ia_rl     = mft + off + mp;
            }
        }
        off += al;
    }

    if (ia_rl && ia_rl_len > 0) {
        u8 *src = malloc(ia_rl_len + 1);
        if (src) {
            memcpy(src, ia_rl, ia_rl_len);
            src[ia_rl_len] = 0;
            load_indx_blocks(node, mnt, src, ia_rl_len);
            free(src);
        }
    }

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

    struct mft_rec *mr = (struct mft_rec *)mft;
    if (mr->magic != MFT_MAGIC) {
        free(mft);
        return -EIO;
    }

    u32 off = le16((u8 *)&mr->attrs);
    while (off + 16 <= mnt->mft_size) {
        struct attr_rec *a  = (struct attr_rec *)(mft + off);
        u32              at = le32((u8 *)&a->type);
        u32              al = le32((u8 *)&a->length);
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
    if (!src || !node) return -EINVAL;

    uint8_t drive;
    if (blockdev_parse_drive(src, &drive) < 0) return -ENODEV;
    blockdev_device_t dev;
    if (blockdev_open_drive(drive, &dev) < 0) return -ENODEV;

    u8 boot[512];
    if (blockdev_read_bytes(&dev, 0, boot, 512) < 0) return -EIO;

    struct ntfs_boot_sector *bs = (struct ntfs_boot_sector *)boot;
    if (le64((u8 *)&bs->oem_id) != magicNTFS) return -EINVAL;

    u16 bps = le16((u8 *)&bs->bpb.bytes_per_sector);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1))) return -EINVAL;

    s8  spc = bs->bpb.sectors_per_cluster;
    u32 sec_per_cluster;
    if ((u8)spc >= 0xf4)
        sec_per_cluster = 1U << -(s8)spc;
    else
        sec_per_cluster = (u32)(u8)spc;
    if (!sec_per_cluster) return -EINVAL;

    ntfs_mount_t *mnt = calloc(1, sizeof(ntfs_mount_t));
    if (!mnt) return -ENOMEM;
    memcpy(&mnt->dev, &dev, sizeof(dev));

    mnt->sector_size  = bps;
    mnt->cluster_size = bps * sec_per_cluster;
    mnt->cluster_bits = __builtin_ctz(mnt->cluster_size);
    mnt->cluster_mask = mnt->cluster_size - 1;

    s8 cmr        = bs->clusters_per_mft_record;
    mnt->mft_size = cmr > 0 ? mnt->cluster_size << (__builtin_ffs(cmr) - 1) : (1U << -cmr);
    if (mnt->mft_size < 256 || mnt->mft_size > 65536) {
        free(mnt);
        return -EINVAL;
    }
    mnt->mft_bits = __builtin_ctz(mnt->mft_size);

    s8 cir         = bs->clusters_per_index_record;
    mnt->indx_size = cir > 0 ? mnt->cluster_size << (__builtin_ffs(cir) - 1) : (1U << -cir);
    if (mnt->indx_size < 512 || mnt->indx_size > 65536) {
        free(mnt);
        return -EINVAL;
    }

    mnt->mft_lcn     = le64((u8 *)&bs->mft_lcn);
    mnt->nr_clusters = le64((u8 *)&bs->number_of_sectors) >> (__builtin_ctz(sec_per_cluster));
    if (mnt->mft_lcn >= mnt->nr_clusters) {
        free(mnt);
        return -EINVAL;
    }

    /* blocks per cluster for index allocation VCN→LCN mapping */
    mnt->indx_vcn_per_cluster = mnt->cluster_size / mnt->indx_size;
    if (mnt->indx_vcn_per_cluster == 0) mnt->indx_vcn_per_cluster = 1;

    ntfs_handle_t *root_h = calloc(1, sizeof(ntfs_handle_t));
    if (!root_h) {
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
    free(h->runlist_buf);
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

static size_t ntfs_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}
static size_t ntfs_vfs_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    (void)n;
    (void)a;
    (void)o;
    (void)s;
    return 0;
}
static int ntfs_vfs_mkdir(void *p, const char *n, vfs_node_t nd)
{
    (void)p;
    (void)n;
    (void)nd;
    return -EROFS;
}
static int ntfs_vfs_mkfile(void *p, const char *n, vfs_node_t nd)
{
    (void)p;
    (void)n;
    (void)nd;
    return -EROFS;
}
static int ntfs_vfs_stat(void *file, vfs_node_t nd)
{
    ntfs_handle_t *h = file;
    if (!h || !nd) return -EINVAL;
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
    return -ENOSYS;
}

static int ntfs_vfs_free(void *handle)
{
    ntfs_handle_t *h = handle;
    if (!h) return -EINVAL;
    free(h->runlist_buf);
    /* note: mnt is shared, freed by unmount */
    free(h);
    return 0;
}

static int ntfs_vfs_delete(void *p, vfs_node_t n)
{
    (void)p;
    (void)n;
    return -EROFS;
}
static int ntfs_vfs_rename(void *c, const char *nn)
{
    (void)c;
    (void)nn;
    return -EROFS;
}

#if CONFIG_NTFS_FS
static struct vfs_callback ntfs_cb = {
    .mount    = ntfs_vfs_mount,
    .unmount  = ntfs_vfs_unmount,
    .open     = ntfs_vfs_open,
    .close    = ntfs_vfs_close,
    .read     = ntfs_vfs_read,
    .write    = ntfs_vfs_write,
    .readlink = ntfs_vfs_readlink,
    .mkdir    = ntfs_vfs_mkdir,
    .mkfile   = ntfs_vfs_mkfile,
    .link     = ntfs_vfs_mkfile,
    .symlink  = ntfs_vfs_mkfile,
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
