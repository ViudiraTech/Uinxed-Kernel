/*
 *
 *      extfs.h
 *      ext2/ext3/ext4 filesystem driver
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EXTFS_H_
#define INCLUDE_EXTFS_H_

#include <drivers/block/core/blockdev.h>
#include <fs/core/fs_txn.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

typedef struct extfs_journal extfs_journal_t;

/* Special inode numbers */
#define EXT2_BAD_INO            1
#define EXT2_ROOT_INO           2
#define EXT2_BOOT_LOADER_INO    5
#define EXT2_UNDEL_DIR_INO      6
#define EXT2_GOOD_OLD_FIRST_INO 11

/* Block sizes */
#define EXT2_MIN_BLOCK_SIZE     1024
#define EXT2_MAX_BLOCK_SIZE     65536
#define EXT2_MIN_BLOCK_LOG_SIZE 10
#define EXT2_MAX_BLOCK_LOG_SIZE 16

/* Constants relative to data blocks */
#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK  (EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK  (EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS    (EXT2_TIND_BLOCK + 1)

/* Directory entry sizes */
#define EXT2_DIR_PAD               4
#define EXT2_DIR_ROUND             (EXT2_DIR_PAD - 1)
#define EXT2_DIR_REC_LEN(name_len) (((name_len) + 8 + EXT2_DIR_ROUND) & ~EXT2_DIR_ROUND)
#define EXT2_MAX_REC_LEN           ((1 << 16) - 1)
#define EXT2_NAME_LEN              255

/* File system states */
#define EXT2_VALID_FS 0x0001
#define EXT2_ERROR_FS 0x0002

/* Revision levels */
#define EXT2_GOOD_OLD_REV        0
#define EXT2_DYNAMIC_REV         1
#define EXT2_CURRENT_REV         EXT2_GOOD_OLD_REV
#define EXT2_MAX_SUPP_REV        EXT2_DYNAMIC_REV
#define EXT2_GOOD_OLD_INODE_SIZE 128

/* Feature flags */
#define EXT2_FEATURE_INCOMPAT_FILETYPE    0x0002
#define EXT3_FEATURE_INCOMPAT_RECOVER     0x0004
#define EXT3_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008
#define EXT2_FEATURE_INCOMPAT_META_BG     0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS     0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT       0x0080
#define EXT4_FEATURE_INCOMPAT_MMP         0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG     0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE    0x0400
#define EXT4_FEATURE_INCOMPAT_DIRDATA     0x1000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED   0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR    0x4000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT     0x10000

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER   0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE     0x0002
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR      0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE      0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM       0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK      0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE    0x0040
#define EXT4_FEATURE_RO_COMPAT_QUOTA          0x0100
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC       0x0200
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM  0x0400
#define EXT4_FEATURE_RO_COMPAT_READONLY       0x1000
#define EXT4_FEATURE_RO_COMPAT_PROJECT        0x2000
#define EXT4_FEATURE_RO_COMPAT_VERITY         0x8000
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x10000

#define EXT2_FEATURE_COMPAT_EXT_ATTR      0x0008
#define EXT2_FEATURE_COMPAT_DIR_INDEX     0x0020
#define EXT3_FEATURE_COMPAT_HAS_JOURNAL   0x0004
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200

/* Directory file types */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

/* Inode mode bits */
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK  0xA000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFBLK  0x6000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFCHR  0x2000
#define EXT2_S_IFIFO  0x1000
#define EXT2_S_ISUID  0x0800
#define EXT2_S_ISGID  0x0400
#define EXT2_S_ISVTX  0x0200
#define EXT2_S_IRWXU  0x01C0
#define EXT2_S_IRUSR  0x0100
#define EXT2_S_IWUSR  0x0080
#define EXT2_S_IXUSR  0x0040
#define EXT2_S_IRWXG  0x0038
#define EXT2_S_IRGRP  0x0020
#define EXT2_S_IWGRP  0x0010
#define EXT2_S_IXGRP  0x0008
#define EXT2_S_IRWXO  0x0007
#define EXT2_S_IROTH  0x0004
#define EXT2_S_IWOTH  0x0002
#define EXT2_S_IXOTH  0x0001

#define EXT4_EXTENTS_FL      0x00080000U
#define EXT4_INDEX_FL        0x00001000U
#define EXT4_EXT_MAGIC       0xF30AU
#define EXT4_BG_INODE_UNINIT 0x0001U
#define EXT4_BG_BLOCK_UNINIT 0x0002U
#define EXT4_BG_INODE_ZEROED 0x0004U

/* Super block - 1024 bytes at byte offset 1024 */
typedef struct ext2_super_block {
        uint32_t s_inodes_count;
        uint32_t s_blocks_count;
        uint32_t s_r_blocks_count;
        uint32_t s_free_blocks_count;
        uint32_t s_free_inodes_count;
        uint32_t s_first_data_block;
        uint32_t s_log_block_size;
        uint32_t s_log_frag_size;
        uint32_t s_blocks_per_group;
        uint32_t s_frags_per_group;
        uint32_t s_inodes_per_group;
        uint32_t s_mtime;
        uint32_t s_wtime;
        uint16_t s_mnt_count;
        uint16_t s_max_mnt_count;
        uint16_t s_magic;
        uint16_t s_state;
        uint16_t s_errors;
        uint16_t s_minor_rev_level;
        uint32_t s_lastcheck;
        uint32_t s_checkinterval;
        uint32_t s_creator_os;
        uint32_t s_rev_level;
        uint16_t s_def_resuid;
        uint16_t s_def_resgid;
        /* Dynamic rev fields */
        uint32_t s_first_ino;
        uint16_t s_inode_size;
        uint16_t s_block_group_nr;
        uint32_t s_feature_compat;
        uint32_t s_feature_incompat;
        uint32_t s_feature_ro_compat;
        uint8_t  s_uuid[16];
        char     s_volume_name[16];
        char     s_last_mounted[64];
        uint32_t s_algorithm_usage_bitmap;
        uint8_t  s_prealloc_blocks;
        uint8_t  s_prealloc_dir_blocks;
        uint16_t s_reserved_gdt_blocks;
        uint8_t  s_journal_uuid[16];
        uint32_t s_journal_inum;
        uint32_t s_journal_dev;
        uint32_t s_last_orphan;
        uint32_t s_hash_seed[4];
        uint8_t  s_def_hash_version;
        uint8_t  s_reserved_char_pad;
        uint16_t s_desc_size;
        uint32_t s_default_mount_opts;
        uint32_t s_first_meta_bg;
        uint32_t s_reserved[190];
} __attribute__((packed)) ext2_super_block_t;

/* Normalized ext4 block group descriptor (the ext2 disk form is its first 32 bytes). */
typedef struct ext2_group_desc {
        uint32_t bg_block_bitmap;
        uint32_t bg_inode_bitmap;
        uint32_t bg_inode_table;
        uint16_t bg_free_blocks_count;
        uint16_t bg_free_inodes_count;
        uint16_t bg_used_dirs_count;
        uint16_t bg_flags;
        uint32_t bg_exclude_bitmap_lo;
        uint16_t bg_block_bitmap_csum_lo;
        uint16_t bg_inode_bitmap_csum_lo;
        uint16_t bg_itable_unused_lo;
        uint16_t bg_checksum;
        uint32_t bg_block_bitmap_hi;
        uint32_t bg_inode_bitmap_hi;
        uint32_t bg_inode_table_hi;
        uint16_t bg_free_blocks_count_hi;
        uint16_t bg_free_inodes_count_hi;
        uint16_t bg_used_dirs_count_hi;
        uint16_t bg_itable_unused_hi;
        uint32_t bg_exclude_bitmap_hi;
        uint16_t bg_block_bitmap_csum_hi;
        uint16_t bg_inode_bitmap_csum_hi;
        uint32_t bg_reserved;
} __attribute__((packed)) ext2_group_desc_t;

/* Inode - 128 bytes (minimum) */
typedef struct ext2_inode {
        uint16_t i_mode;
        uint16_t i_uid;
        uint32_t i_size;
        uint32_t i_atime;
        uint32_t i_ctime;
        uint32_t i_mtime;
        uint32_t i_dtime;
        uint16_t i_gid;
        uint16_t i_links_count;
        uint32_t i_blocks;
        uint32_t i_flags;
        uint32_t osd1;
        uint32_t i_block[EXT2_N_BLOCKS];
        uint32_t i_generation;
        uint32_t i_file_acl;
        uint32_t i_dir_acl;
        uint32_t i_faddr;
        uint16_t l_i_blocks_high;
        uint16_t l_i_file_acl_high;
        uint16_t l_i_uid_high;
        uint16_t l_i_gid_high;
        uint16_t l_i_checksum_lo;
        uint16_t l_i_reserved;
} __attribute__((packed)) ext2_inode_t;

/* Directory entry (new format with file_type) */
typedef struct ext2_dir_entry {
        uint32_t inode;
        uint16_t rec_len;
        uint8_t  name_len;
        uint8_t  file_type;
        char     name[EXT2_NAME_LEN];
} __attribute__((packed)) ext2_dir_entry_t;

/* Per-filesystem superblock info */
typedef struct extfs_sb_info {
        blockdev_device_t   device;
        ext2_super_block_t *es;         // Pointer to on-disk superblock
        ext2_group_desc_t  *group_desc; // Array of group descriptors
        uint32_t            block_size;
        uint32_t            blocks_per_group;
        uint32_t            inodes_per_group;
        uint32_t            groups_count;
        uint32_t            desc_per_block;
        uint32_t            desc_size;
        uint64_t            blocks_count;
        uint32_t            checksum_seed;
        uint32_t            s_first_data_block;
        uint32_t            inode_size;
        uint32_t            s_first_ino;
        uint32_t            gdb_count; // Group descriptor blocks count
        uint32_t            sb_block;  // Superblock block number
        uint8_t             log_block_size;
        spinlock_t          lock;
        int                 read_only;
        fs_txn_log_t        transaction_log;
        fs_txn_t           *active_transaction;
        int                 transaction_log_initialized;
        extfs_journal_t    *journal;
} extfs_sb_info_t;

/* Per-inode info */
typedef struct extfs_inode_info {
        uint32_t i_data[EXT2_N_BLOCKS];
        uint32_t i_flags;
        uint32_t i_file_acl;
        uint32_t i_dir_acl;
        uint32_t i_dtime;
        uint32_t i_block_group;
} extfs_inode_info_t;

/* VFS handle stored in node->handle */
typedef struct extfs_handle {
        extfs_sb_info_t   *sb;       // Pointer to superblock info
        extfs_inode_info_t ei;       // In-core inode info
        uint32_t           inode_no; // On-disk inode number
        int                owns_sb;  // Whether this handle owns the sb_info
} extfs_handle_t;

/* super.c */
int  extfs_read_super(extfs_sb_info_t *sb, const blockdev_device_t *device);
int  extfs_detect_version(const ext2_super_block_t *es);
int  extfs_write_super(extfs_sb_info_t *sb);
void extfs_free_super(extfs_sb_info_t *sb);
int  extfs_read_inode_raw(extfs_sb_info_t *sb, uint32_t ino, ext2_inode_t *raw);
int  extfs_write_inode_raw(extfs_sb_info_t *sb, uint32_t ino, const ext2_inode_t *raw);
int  extfs_read_group_desc(extfs_sb_info_t *sb, uint32_t group, ext2_group_desc_t *desc);
int  extfs_write_group_desc(extfs_sb_info_t *sb, uint32_t group, const ext2_group_desc_t *desc);
int  extfs_read_block(extfs_sb_info_t *sb, uint32_t phys_block, void *buf);
int  extfs_write_block(extfs_sb_info_t *sb, uint32_t phys_block, const void *buf);
int  extfs_write_data_block(extfs_sb_info_t *sb, uint32_t phys_block, const void *buf);
int  extfs_update_bitmap_checksum(extfs_sb_info_t *sb, uint32_t group, int inode_bitmap, const void *bitmap);
int  extfs_transaction_begin(extfs_sb_info_t *sb, fs_txn_t *transaction, uint32_t credits);
int  extfs_transaction_commit(extfs_sb_info_t *sb, fs_txn_t *transaction);
void extfs_transaction_abort(extfs_sb_info_t *sb, fs_txn_t *transaction, int error);

/* alloc.c */
int      extfs_alloc_block(extfs_sb_info_t *sb, uint32_t goal, uint32_t *out);
void     extfs_free_block(extfs_sb_info_t *sb, uint32_t block);
int      extfs_alloc_inode(extfs_sb_info_t *sb, uint32_t *out);
void     extfs_free_inode(extfs_sb_info_t *sb, uint32_t ino);
int      extfs_adjust_used_dirs(extfs_sb_info_t *sb, uint32_t ino, int delta);
uint32_t extfs_count_free_blocks(extfs_sb_info_t *sb);
uint32_t extfs_count_free_inodes(extfs_sb_info_t *sb);

/* inode.c */
extfs_handle_t *extfs_alloc_handle(extfs_sb_info_t *sb, uint32_t ino);
int             extfs_load_inode(extfs_handle_t *h);
int             extfs_flush_inode(extfs_handle_t *h);
uint32_t        extfs_map_block(extfs_handle_t *h, uint32_t logical, int create);
int             extfs_read_data(extfs_handle_t *h, void *buf, uint64_t offset, size_t size);
int             extfs_write_data(extfs_handle_t *h, const void *buf, uint64_t offset, size_t size);
int             extfs_truncate(extfs_handle_t *h, uint64_t size);
void            extfs_free_inode_blocks(extfs_handle_t *h);
int             extfs_release_xattr_block(extfs_handle_t *h);

/* extents.c */
uint32_t extfs_extent_map_block(extfs_handle_t *h, uint32_t logical, int create);
int      extfs_extent_remove_space(extfs_handle_t *h, uint32_t first, uint32_t last);
int      extfs_extent_free_all(extfs_handle_t *h);
int      extfs_extent_count_blocks(extfs_handle_t *h, uint64_t *blocks);

/* dir.c */
int extfs_dir_lookup(extfs_handle_t *dir_h, const char *name, uint32_t *ino);
int extfs_dir_add_entry(extfs_handle_t *dir_h, const char *name, uint32_t ino, uint8_t file_type);
int extfs_dir_remove_entry(extfs_handle_t *dir_h, const char *name);
int extfs_dir_read_entries(extfs_handle_t *dir_h, void *buf, size_t bufsize, size_t *done);
int extfs_make_empty_dir(extfs_handle_t *dir_h, uint32_t self_ino, uint32_t parent_ino);
int extfs_dir_set_parent(extfs_handle_t *dir_h, uint32_t parent_ino);
int extfs_dir_empty(extfs_handle_t *dir_h);
int extfs_dir_block_verify(extfs_handle_t *dir_h, uint32_t logical, const void *block);

/* extfs.c - registration */
void extfs_regist(void);

#endif // INCLUDE_EXTFS_H_
