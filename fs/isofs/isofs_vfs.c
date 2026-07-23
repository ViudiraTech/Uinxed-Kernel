/*
 *
 *      isofs_vfs.c
 *      ISO 9660 filesystem VFS integration
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/ahci.h>
#include <drivers/atapi.h>
#include <drivers/blockdev.h>
#include <drivers/ide.h>
#include <fs/isofs/isofs.h>
#include <fs/isofs/rock.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

static int isofs_fs_id = 0;

/* ─── Block device I/O helpers ─── */

static int isofs_read_block(isofs_mount_t *mnt, uint32_t block, void *buf, uint32_t size)
{
    uint64_t offset = (uint64_t)block * mnt->block_size;
    uint32_t rsize  = size > mnt->block_size ? mnt->block_size : size;
    return blockdev_read_bytes(&mnt->device, offset, buf, rsize);
}

/* wrapper matching rr_read_block signature for Rock Ridge */
static int isofs_rr_read_block(void *ctx, uint32_t block, void *buf, uint32_t size)
{
    return isofs_read_block((isofs_mount_t *)ctx, block, buf, size);
}

static int isofs_read_bytes(isofs_mount_t *mnt, uint64_t offset, void *buf, uint32_t size)
{
    return blockdev_read_bytes(&mnt->device, offset, buf, size);
}

/* ─── ISO date helper ─── */

static uint64_t iso_date_from_de(const uint8_t *d, int high_sierra)
{
    return iso_date_to_unix(d, high_sierra ? ISO_DATE_HIGH_SIERRA : 0);
}

/* ─── Mount/dismount ─── */

static void isofs_mount_destroy(isofs_mount_t *mnt)
{
    if (!mnt) return;
    free(mnt);
}

static void isofs_handle_destroy(isofs_handle_t *h)
{
    if (!h) return;
    free(h->raw_de_buf);
    if (h->owns_mount && h->mount) isofs_mount_destroy(h->mount);
    free(h);
}

/* ─── Parse a drive specifier: "sr0", "sr1", ... ─── */

static int isofs_parse_src(const char *src, uint8_t *drive)
{
    if (!src || !drive) return -EINVAL;
    return blockdev_parse_drive(src, drive);
}

/* ─── Scan directory records to find a child by name ─── */

static iso_directory_record_t *isofs_find_entry(isofs_mount_t *mnt, uint32_t dir_block, uint64_t dir_size, const char *name, uint32_t *out_block,
                                                uint32_t *out_offset)
{
    uint8_t *buf;
    uint32_t block_size = mnt->block_size;
    uint64_t pos        = 0;

    buf = malloc(block_size);
    if (!buf) return NULL;

    uint8_t cross_buf[512];

    while (pos < dir_size) {
        uint32_t blk = dir_block + (uint32_t)(pos / block_size);
        uint32_t off = (uint32_t)(pos % block_size);

        if (isofs_read_block(mnt, blk, buf, block_size) != EOK) {
            free(buf);
            return NULL;
        }

        iso_directory_record_t *de     = (iso_directory_record_t *)(buf + off);
        uint8_t                 de_len = de->length;

        if (de_len == 0) {
            pos = (pos + block_size) & ~(uint64_t)(block_size - 1);
            continue;
        }

        uint32_t total = sizeof(iso_directory_record_t) + de->name_len[0];

        /* handle record crossing a block boundary */
        if (off + de_len > block_size) {
            uint32_t frag1 = block_size - off;
            memcpy(cross_buf, de, frag1);
            if (isofs_read_block(mnt, blk + 1, buf, block_size) != EOK) {
                free(buf);
                return NULL;
            }
            memcpy(cross_buf + frag1, buf, de_len - frag1);
            de     = (iso_directory_record_t *)cross_buf;
            de_len = de->length;
            total  = sizeof(iso_directory_record_t) + de->name_len[0];
        }

        if (de_len < total) {
            pos += de_len;
            continue;
        }

        /* multi-extent entries are skipped in find */
        if (de->flags[0] & 0x80) {
            pos += de_len;
            continue;
        }

        /* check name */
        int   name_len = de->name_len[0];
        char  rr_name[256];
        int   match = 0;
        char *dpnt  = de->name;
        int   dlen  = name_len;

        /* skip . and .. */
        if (name_len == 1 && de->name[0] <= 1) {
            pos += de_len;
            continue;
        }

        if (mnt->rock_ridge) {
            int rr_len = get_rock_ridge_filename(de, rr_name, sizeof(rr_name), mnt);
            if (rr_len > 0) {
                dpnt = rr_name;
                dlen = rr_len;
            }
        } else {
            char xbuf[256];
            dlen = isofs_name_translate(de, xbuf, sizeof(xbuf));
            dpnt = xbuf;
            memcpy(rr_name, xbuf, dlen + 1);
        }

        if (!(de->flags[0] & 1) || !mnt->hide) {
            if (dlen > 0 && (int)strlen(name) == dlen && !memcmp(dpnt, name, (size_t)dlen)) { match = 1; }
        }

        if (match) {
            if (out_block) *out_block = blk;
            if (out_offset) *out_offset = off;
            iso_directory_record_t *ret = malloc(de_len);
            if (ret) memcpy(ret, de, de_len);
            free(buf);
            return ret;
        }

        pos += de_len;
    }

    free(buf);
    return NULL;
}

/* ─── Read directory entries into the VFS child list ─── */

static int isofs_load_directory(vfs_node_t node)
{
    isofs_handle_t *h = node ? node->handle : NULL;

    if (!node || !h || !h->is_dir || !h->mount) return -EINVAL;
    if (node->visited) return EOK;

    isofs_mount_t *mnt        = h->mount;
    uint32_t       block_size = mnt->block_size;
    uint32_t       dir_block  = h->first_extent;
    uint64_t       pos        = 0;

    uint8_t *buf = malloc(block_size);
    if (!buf) return -ENOMEM;

    /* temporary buffer for records that cross block boundaries */
    uint8_t cross_buf[512];

    while (pos < h->size) {
        uint32_t blk = dir_block + (uint32_t)(pos / block_size);
        uint32_t off = (uint32_t)(pos % block_size);

        if (isofs_read_block(mnt, blk, buf, block_size) != EOK) break;

        iso_directory_record_t *de     = (iso_directory_record_t *)(buf + off);
        uint8_t                 de_len = de->length;

        if (de_len == 0) {
            pos = (pos + block_size) & ~(uint64_t)(block_size - 1);
            continue;
        }

        uint32_t total = sizeof(iso_directory_record_t) + de->name_len[0];

        /* handle record crossing a block boundary */
        if (off + de_len > block_size) {
            uint32_t frag1 = block_size - off;
            memcpy(cross_buf, de, frag1);
            if (isofs_read_block(mnt, blk + 1, buf, block_size) != EOK) break;
            memcpy(cross_buf + frag1, buf, de_len - frag1);
            de     = (iso_directory_record_t *)cross_buf;
            de_len = de->length;
            total  = sizeof(iso_directory_record_t) + de->name_len[0];
        }

        if (de_len < total) {
            pos += de_len;
            continue;
        }

        /* skip multi-extent continuation entries */
        if (de->flags[0] & 0x80) {
            pos += de_len;
            continue;
        }

        /* skip . and .. */
        if (de->name_len[0] == 1 && de->name[0] <= 1) {
            pos += de_len;
            continue;
        }

        /* Hide hidden / associated files if requested */
        if (mnt->hide && (de->flags[0] & 1)) {
            pos += de_len;
            continue;
        }
        if (!mnt->showassoc && (de->flags[0] & 4)) {
            pos += de_len;
            continue;
        }

        /* determine the visible name */
        char name_buf[256];
        int  dlen;
        int  is_dir = (de->flags[0] & 2) != 0;

        if (mnt->rock_ridge) {
            dlen = get_rock_ridge_filename(de, name_buf, sizeof(name_buf), mnt);
            if (dlen <= 0) { dlen = isofs_name_translate(de, name_buf, sizeof(name_buf)); }
        } else {
            dlen = isofs_name_translate(de, name_buf, sizeof(name_buf));
        }

        if (dlen == 0) {
            pos += de_len;
            continue;
        }
        name_buf[dlen] = '\0';

        /* skip duplicates */
        if (vfs_do_search(node, name_buf)) {
            pos += de_len;
            continue;
        }

        /* allocate VFS node */
        vfs_node_t      child = vfs_node_alloc(node, name_buf);
        isofs_handle_t *ch    = calloc(1, sizeof(isofs_handle_t));

        if (!child || !ch) {
            free(ch);
            if (child) {
                node->child = clist_delete(node->child, child);
                vfs_free(child);
            }
            pos += de_len;
            continue;
        }

        ch->mount         = mnt;
        ch->is_dir        = is_dir;
        ch->first_extent  = isonum_733(de->extent) + isonum_711(&de->ext_attr_length);
        ch->size          = isonum_733(de->size);
        ch->extent_block  = blk;
        ch->extent_offset = off;
        ch->raw_de_buf    = malloc(de_len);
        if (ch->raw_de_buf) memcpy(ch->raw_de_buf, de, de_len);
        ch->raw_de = (iso_directory_record_t *)ch->raw_de_buf;

        /* parse Rock Ridge if available */
        if (mnt->rock_ridge && ch->raw_de) parse_rock_ridge_inode(ch->raw_de, ch, mnt);

        if (mnt->cruft) ch->size &= 0x00ffffff;

        child->handle = ch;
        child->type   = is_dir ? file_dir : (ch->is_symlink ? file_symlink : file_none);
        child->size   = ch->size;
        child->blksz  = mnt->block_size;
        child->inode  = ((uint64_t)blk << 32) | off;

        /* timestamps */
        uint64_t ts       = iso_date_from_de(de->date, mnt->high_sierra);
        child->createtime = ts;
        child->readtime   = ts;
        child->writetime  = ts;

        pos += de_len;
    }

    free(buf);
    node->visited = 1;
    return EOK;
}

/* ─── Read symlink target (delegates to Rock Ridge parser) ─── */

static int isofs_read_symlink(isofs_handle_t *h, char *buf, size_t bufsize)
{
    if (!h || !h->is_symlink || !h->raw_de || !h->mount) return -EINVAL;
    return get_rock_ridge_symlink(h->raw_de, h->mount, buf, (int)bufsize);
}

/* ─── VFS callbacks ─── */

static int isofs_vfs_mount(const char *src, vfs_node_t node)
{
    if (!src || !node) return -EINVAL;

    uint8_t         drive;
    int             status;
    isofs_mount_t  *mnt;
    isofs_handle_t *root_h;

    status = isofs_parse_src(src, &drive);
    if (status != EOK) return status;

    mnt = calloc(1, sizeof(isofs_mount_t));
    if (!mnt) return -ENOMEM;

    status = blockdev_open_drive(drive, &mnt->device);
    if (status != EOK) {
        free(mnt);
        return status;
    }

    mnt->owns_device   = 1;
    mnt->rock_ridge    = 1;
    mnt->rock_offset   = -1;
    mnt->hide          = 0;
    mnt->showassoc     = 0;
    mnt->cruft         = 0;
    mnt->rr_read_block = isofs_rr_read_block;
    mnt->rr_read_ctx   = mnt;

    /* Determine block size from ATAPI device */
    mnt->block_size = mnt->device.sector_size;
    if (mnt->block_size < 2048) mnt->block_size = 2048;
    if (mnt->block_size > 4096) mnt->block_size = 2048;

    switch (mnt->block_size) {
        case 512 :
            mnt->block_bits = 9;
            break;
        case 1024 :
            mnt->block_bits = 10;
            break;
        case 2048 :
            mnt->block_bits = 11;
            break;
        case 4096 :
            mnt->block_bits = 12;
            break;
        default :
            mnt->block_bits = 11;
            mnt->block_size = 2048;
            break;
    }

    /* Read volume descriptors starting at LBA 16 */
    uint8_t                   vd_buf[2048];
    iso_primary_descriptor_t *pri      = NULL;
    uint8_t                  *pri_copy = NULL;
    int                       found    = 0;

    for (uint32_t blk = 16; blk < 100; blk++) {
        if (isofs_read_block(mnt, blk, vd_buf, sizeof(vd_buf)) != EOK) {
            isofs_mount_destroy(mnt);
            free(pri_copy);
            return -EIO;
        }

        iso_volume_descriptor_t *vd = (iso_volume_descriptor_t *)vd_buf;

        if (strncmp(vd->id, ISO_STANDARD_ID, 5) == 0) {
            if (vd->type == ISO_VD_END) break;
            if (vd->type == ISO_VD_PRIMARY) {
                if (!pri_copy) {
                    pri_copy = malloc(sizeof(iso_primary_descriptor_t));
                    if (!pri_copy) {
                        isofs_mount_destroy(mnt);
                        return -ENOMEM;
                    }
                    memcpy(pri_copy, vd_buf, sizeof(iso_primary_descriptor_t));
                    pri   = (iso_primary_descriptor_t *)pri_copy;
                    found = 1;
                }
            }
        }
    }

    if (!found || !pri) {
        isofs_mount_destroy(mnt);
        free(pri_copy);
        return -EINVAL;
    }

    /* Extract metadata from primary descriptor */
    mnt->vol_space_size  = isonum_733(pri->volume_space_size);
    uint32_t logical_blk = isonum_723(pri->logical_block_size);

    if (logical_blk < mnt->block_size) {
        isofs_mount_destroy(mnt);
        free(pri_copy);
        return -EINVAL;
    }

    /* Use logical block size from the descriptor */
    if (logical_blk != mnt->block_size) {
        mnt->block_size = logical_blk;
        switch (mnt->block_size) {
            case 512 :
                mnt->block_bits = 9;
                break;
            case 1024 :
                mnt->block_bits = 10;
                break;
            case 2048 :
                mnt->block_bits = 11;
                break;
            case 4096 :
                mnt->block_bits = 12;
                break;
            default :
                isofs_mount_destroy(mnt);
                free(pri_copy);
                return -EINVAL;
        }
    }

    /* Read root directory record */
    iso_directory_record_t *root_de = (iso_directory_record_t *)pri->root_directory_record;

    mnt->first_data_zone = isonum_733(root_de->extent) + isonum_711(&root_de->ext_attr_length);
    mnt->high_sierra     = 0;

    /* Build root handle */
    root_h = calloc(1, sizeof(isofs_handle_t));
    if (!root_h) {
        isofs_mount_destroy(mnt);
        free(pri_copy);
        return -ENOMEM;
    }

    root_h->mount        = mnt;
    root_h->owns_mount   = 1;
    root_h->is_dir       = 1;
    root_h->first_extent = mnt->first_data_zone;
    root_h->size         = isonum_733(root_de->size);
    root_h->raw_de_buf   = malloc(sizeof(pri->root_directory_record));
    if (root_h->raw_de_buf) memcpy(root_h->raw_de_buf, root_de, sizeof(pri->root_directory_record));
    root_h->raw_de = (iso_directory_record_t *)root_h->raw_de_buf;

    /* Root directory record timestamp – save before freeing PVD copy */
    uint64_t root_ts = iso_date_from_de(root_de->date, 0);

    /* PVD copy no longer needed */
    free(pri_copy);
    pri_copy = NULL;

    /* Parse Rock Ridge for root */
    if (mnt->rock_ridge && root_h->raw_de) parse_rock_ridge_inode(root_h->raw_de, root_h, mnt);

    /* Fill VFS node */
    node->handle     = root_h;
    node->type       = file_dir;
    node->size       = root_h->size;
    node->blksz      = mnt->block_size;
    node->createtime = root_ts;
    node->readtime   = root_ts;
    node->writetime  = root_ts;

    /* Load root directory entries */
    if (isofs_load_directory(node) != EOK) {
        isofs_handle_destroy(root_h);
        node->handle = NULL;
        return -EIO;
    }

    plogk("isofs: Mounted atapi%u, block size %u, volume space %u blocks.\n", drive, mnt->block_size, mnt->vol_space_size);
    return EOK;
}

static void isofs_vfs_unmount(void *root)
{
    isofs_handle_destroy(root);
}

static void isofs_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    if (!parent || !node || !node->parent || node->handle) return;

    isofs_handle_t *p = node->parent->handle;
    if (!p || !p->mount || !p->is_dir) return;

    /* search the parent directory for this child's record */
    iso_directory_record_t *de = isofs_find_entry(p->mount, p->first_extent, p->size, name, NULL, NULL);
    if (!de) return;

    isofs_handle_t *h = calloc(1, sizeof(isofs_handle_t));
    if (!h) {
        free(de);
        return;
    }

    h->mount        = p->mount;
    h->is_dir       = (de->flags[0] & 2) != 0;
    h->first_extent = isonum_733(de->extent) + isonum_711(&de->ext_attr_length);
    h->size         = isonum_733(de->size);
    h->raw_de_buf   = (uint8_t *)de;
    h->raw_de       = de;

    if (p->mount->rock_ridge) parse_rock_ridge_inode(de, h, p->mount);

    if (p->mount->cruft) h->size &= 0x00ffffff;

    node->handle = h;
    node->type   = h->is_dir ? file_dir : (h->is_symlink ? file_symlink : file_none);
    node->size   = h->size;
    node->blksz  = p->mount->block_size;

    uint64_t ts      = iso_date_from_de(de->date, p->mount->high_sierra);
    node->createtime = ts;
    node->readtime   = ts;
    node->writetime  = ts;

    if (h->is_dir) isofs_load_directory(node);
}

static void isofs_vfs_close(void *current)
{
    (void)current;
}

static size_t isofs_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    isofs_handle_t *h = file;

    if (!h || !addr || !h->mount || h->is_dir || h->is_symlink) return 0;
    if (offset >= h->size) return 0;

    isofs_mount_t *mnt   = h->mount;
    uint32_t       bsz   = mnt->block_size;
    uint64_t       start = (uint64_t)h->first_extent * bsz + offset;
    uint32_t       len   = (uint32_t)((offset + size > h->size) ? (h->size - offset) : size);

    if (isofs_read_bytes(mnt, start, addr, len) != EOK) return 0;
    return len;
}

static size_t isofs_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

static size_t isofs_vfs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    isofs_handle_t *h = node ? node->handle : NULL;
    char            buf[4096];
    int             len;

    if (!h || !h->is_symlink || offset > 0) return 0;
    len = isofs_read_symlink(h, buf, sizeof(buf));
    if (len <= 0) return 0;

    size_t copy = (size_t)len < size ? (size_t)len : size;
    memcpy(addr, buf, copy);
    ((char *)addr)[copy] = '\0';
    return copy;
}

static int isofs_vfs_mkdir(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int isofs_vfs_mkfile(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int isofs_vfs_no_link(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int isofs_vfs_stat(void *file, vfs_node_t node)
{
    isofs_handle_t *h = file;

    if (!h || !node) return -EINVAL;
    if (h->is_dir && !node->visited) return isofs_load_directory(node);
    return EOK;
}

static int isofs_vfs_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOSYS;
}

static vfs_node_t isofs_vfs_dup(vfs_node_t node)
{
    vfs_node_t copy;

    if (!node) return NULL;

    copy = vfs_node_alloc(node->parent, node->name);
    if (!copy) return NULL;

    copy->type        = node->type;
    copy->size        = node->size;
    copy->realsize    = node->realsize;
    copy->blksz       = node->blksz;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->group       = node->group;
    copy->linkname    = node->linkname ? strdup(node->linkname) : NULL;
    copy->createtime  = node->createtime;
    copy->readtime    = node->readtime;
    copy->writetime   = node->writetime;
    return copy;
}

static int isofs_vfs_poll(void *file, size_t events)
{
    (void)file;
    return (int)events;
}

static int isofs_vfs_free(void *handle)
{
    isofs_handle_t *h = handle;
    if (!h) return EOK;
    free(h->raw_de_buf);
    h->raw_de_buf = NULL;
    h->raw_de     = NULL;
    if (h->owns_mount && h->mount) isofs_mount_destroy(h->mount);
    free(h);
    return EOK;
}

static int isofs_vfs_delete(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -EROFS;
}

static int isofs_vfs_rename(void *current, const char *new_name)
{
    (void)current;
    (void)new_name;
    return -EROFS;
}

static struct vfs_callback isofs_callbacks = {
    .mount    = isofs_vfs_mount,
    .unmount  = isofs_vfs_unmount,
    .open     = isofs_vfs_open,
    .close    = isofs_vfs_close,
    .read     = isofs_vfs_read,
    .write    = isofs_vfs_write,
    .readlink = isofs_vfs_readlink,
    .mkdir    = isofs_vfs_mkdir,
    .mkfile   = isofs_vfs_mkfile,
    .link     = isofs_vfs_no_link,
    .symlink  = isofs_vfs_no_link,
    .stat     = isofs_vfs_stat,
    .ioctl    = isofs_vfs_ioctl,
    .dup      = isofs_vfs_dup,
    .poll     = isofs_vfs_poll,
    .free     = isofs_vfs_free,
    .delete   = isofs_vfs_delete,
    .rename   = isofs_vfs_rename,
};

void isofs_regist(void)
{
    isofs_fs_id = vfs_regist_fs("isofs", &isofs_callbacks);
    if (isofs_fs_id & ERRNO_MASK) {
        plogk("isofs: Register error.\n");
        return;
    }

    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!atapi_devices[drive].reserved || atapi_devices[drive].type != IDE_ATAPI) continue;
        plogk("isofs: Detected ATAPI device sr%u on IDE (%u blocks, %u bytes/block)\n", drive, atapi_devices[drive].lba_size,
              atapi_devices[drive].blk_size);
    }
    for (int d = 0; d < AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATAPI) continue;
        plogk("isofs: Detected ATAPI device sr%u on AHCI (%u blocks, %u bytes/block)\n", 4 + d, ahci_devices[d].size,
              ahci_devices[d].sector_size);
    }
}

void isofs_mount_all(void)
{
    vfs_node_t root = get_rootdir();
    if (!root || !root->fsid) return;

    uint8_t sr_idx = 0;

    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!atapi_devices[drive].reserved || atapi_devices[drive].type != IDE_ATAPI) continue;

        char       path[32];
        char       src[16];
        vfs_node_t node;

        snprintf(path, sizeof(path), "/mnt/cdrom%u", (unsigned)sr_idx);
        snprintf(src, sizeof(src), "sr%u", (unsigned)sr_idx);

        if (vfs_mkdir(path) != EOK && vfs_mkdir(path) != -EEXIST) {
            sr_idx++;
            continue;
        }
        node = vfs_open(path);
        if (!node) {
            sr_idx++;
            continue;
        }
        if (node->is_mount) {
            vfs_close(node);
            sr_idx++;
            continue;
        }

        if (vfs_mount_fs("isofs", src, node) == EOK)
            plogk("isofs: Auto-mounted %s at %s\n", src, path);
        else
            plogk("isofs: Failed to auto-mount %s (no media or invalid ISO)\n", src);
        vfs_close(node);
        sr_idx++;
    }

    for (int d = 0; d < AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATAPI) continue;

        char       path[32];
        char       src[16];
        vfs_node_t node;

        snprintf(path, sizeof(path), "/mnt/cdrom%u", (unsigned)sr_idx);
        snprintf(src, sizeof(src), "sr%u", (unsigned)sr_idx);

        if (vfs_mkdir(path) != EOK && vfs_mkdir(path) != -EEXIST) {
            sr_idx++;
            continue;
        }
        node = vfs_open(path);
        if (!node) {
            sr_idx++;
            continue;
        }
        if (node->is_mount) {
            vfs_close(node);
            sr_idx++;
            continue;
        }

        if (vfs_mount_fs("isofs", src, node) == EOK)
            plogk("isofs: Auto-mounted %s at %s\n", src, path);
        else
            plogk("isofs: Failed to auto-mount %s (no media or invalid ISO)\n", src);
        vfs_close(node);
        sr_idx++;
    }
}
