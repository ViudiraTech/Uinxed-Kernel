/*
 *
 *      ntfs.h
 *      New Technology File System - internal interfaces
 *
 *      2026/7/26 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NTFS_H_
#define INCLUDE_NTFS_H_

#include <drivers/block/core/blockdev.h>
#include <fs/core/fs_txn.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

/* "$I30" index attribute name, as UTF-16 code units. */
extern const uint16_t ntfs_i30[4];

/* Volume signature at offset 3 of the boot sector ("NTFS    "). */
#define magicNTFS 0x202020205346544eULL

/* Attribute type codes. */
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

/* Record magics. */
#define MFT_MAGIC  0x454c4946
#define INDX_MAGIC 0x58444E49

/* Index entry flags. */
#define INDEX_ENTRY_NODE 0x01
#define INDEX_ENTRY_END  0x02

/* Reparse points. */
#define IO_REPARSE_TAG_SYMLINK       0xa000000cU
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400U

/* Runlist marker for a sparse hole. */
#define LCN_HOLE ((int64_t) - 2)

/* On-disk boot sector - packed because NTFS has no natural alignment. */
typedef struct ntfs_boot_sector {
        uint8_t  jump[3];
        uint64_t oem_id;
        struct {
                uint16_t bytes_per_sector;
                uint8_t  sectors_per_cluster;
                uint16_t reserved_sectors;
                uint8_t  fats;
                uint16_t root_entries;
                uint16_t sectors;
                uint8_t  media_type;
                uint16_t sectors_per_fat;
                uint16_t sectors_per_track;
                uint16_t heads;
                uint32_t hidden_sectors;
                uint32_t large_sectors;
        } __attribute__((packed)) bpb;
        uint8_t                   unused[4];
        uint64_t                  number_of_sectors;
        uint64_t                  mft_lcn;
        uint64_t                  mftmirr_lcn;
        int8_t                    clusters_per_mft_record;
        uint8_t                   reserved0[3];
        int8_t                    clusters_per_index_record;
        uint8_t                   reserved1[3];
        uint64_t                  volume_serial_number;
        uint32_t                  checksum;
        uint8_t                   bootstrap[426];
        uint16_t                  end_of_sector_marker;
} __attribute__((packed)) ntfs_boot_sector_t;

_Static_assert(sizeof(ntfs_boot_sector_t) == 512, "NTFS boot sector on-disk size");

/* MFT record header (the update-sequence array follows it). */
typedef struct mft_rec {
        uint32_t magic;
        uint16_t usa_ofs;
        uint16_t usa_count;
        uint64_t lsn;
        uint16_t seq;
        uint16_t link_count;
        uint16_t attrs;
        uint16_t flags;
        uint32_t bytes_in_use;
        uint32_t bytes_alloc;
        uint64_t base;
        uint16_t next_attr;
        uint16_t _resv;
        uint32_t mft_no;
} __attribute__((packed)) mft_rec_t;

_Static_assert(sizeof(mft_rec_t) == 48, "NTFS MFT record header on-disk size");

/* Attribute record; .d holds the resident or non-resident body. */
typedef struct attr_rec {
        uint32_t type;
        uint32_t length;
        uint8_t  non_resident;
        uint8_t  name_length;
        uint16_t name_offset;
        uint16_t flags;
        uint16_t instance;
        union {
                struct {
                        uint32_t value_length;
                        uint16_t value_offset;
                        uint8_t  _flags;
                        int8_t   _resv;
                } res;
                struct {
                        uint64_t lowest_vcn;
                        uint64_t highest_vcn;
                        uint16_t mapping_pairs_off;
                        uint8_t  comp_unit;
                        uint8_t  _r[5];
                        uint64_t alloc_size;
                        uint64_t data_size;
                        uint64_t init_size;
                        uint64_t compr_size;
                } nres;
        } d;
} __attribute__((packed)) attr_rec_t;

_Static_assert(sizeof(attr_rec_t) == 72, "NTFS attribute record size (16-byte header plus the 56-byte non-resident body)");

/* $FILE_NAME attribute value: parent reference, timestamps and the name. */
typedef struct fname_attr {
        uint64_t parent_dir;
        uint64_t crtime;
        uint64_t mtime_data;
        uint64_t mtime_mft;
        uint64_t atime;
        uint64_t alloc_size;
        uint64_t data_size;
        uint32_t fa;
        union {
                uint32_t ea_size;
                uint32_t rp_tag;
        };
        uint8_t  name_len;
        uint8_t  name_type;
        uint16_t name[];
} __attribute__((packed)) fname_attr_t;

_Static_assert(sizeof(fname_attr_t) == 66, "NTFS $FILE_NAME attribute fixed size");

/* A mounted NTFS volume: geometry, the $MFT/$Bitmap streams and write state. */
typedef struct {
        blockdev_device_t dev;
        uint32_t          cluster_size;
        uint32_t          cluster_bits;
        uint32_t          cluster_mask;
        uint32_t          sector_size;
        uint32_t          mft_size;
        uint32_t          mft_bits;
        uint32_t          indx_size;
        uint32_t          indx_vcn_per_cluster;
        int64_t           mft_lcn;
        int64_t           mftmirr_lcn;
        int64_t           nr_clusters;
        uint8_t          *mft_runlist;
        uint32_t          mft_runlist_size;
        uint64_t          mft_data_size;
        uint8_t          *bitmap_runlist;
        uint32_t          bitmap_runlist_size;
        uint64_t          bitmap_data_size;
        uint16_t         *upcase;
        uint32_t          upcase_length;
        int64_t           index_reclaim_lcn[256];
        int64_t           index_reclaim_length[256];
        int               index_reclaim_count;
        spinlock_t        write_lock;
        int               write_enabled;
        int               dirty_owned;
        fs_txn_log_t      transaction_log;
        fs_txn_t         *active_transaction;
        int               transaction_log_initialized;
} ntfs_mount_t;

/* An open file or directory: its MFT record number and decoded runlist. */
typedef struct {
        ntfs_mount_t *mnt;
        uint64_t      mft_no;
        uint64_t      parent_mft_no;
        uint64_t      file_size;
        uint32_t      file_attr;
        uint16_t     *name;
        uint8_t       name_length;
        int           is_dir;
        int           dir_loaded;
        uint8_t      *runlist_buf;
        uint32_t      runlist_sz;
        int           is_resident; // 1 if runlist_buf holds inline resident data
} ntfs_handle_t;

/* Cross-file prototypes, grouped by the file that defines them. */

/* ntfs_util.c */
uint64_t  ntfs_current_filetime(void);
uint16_t *ntfs_utf16_from(const uint8_t *buf, int ofs, int len);
char     *ntfs_utf8_from_utf16(const uint16_t *u, int len);
uint32_t  ntfs_align8(uint32_t value);
int       ntfs_attr_name_is_i30(const attr_rec_t *attribute, uint32_t length);
int       ntfs_utf16_compare(ntfs_mount_t *mnt, const uint16_t *left, uint8_t left_length, const uint16_t *right, uint8_t right_length);
int       ntfs_utf16_from_utf8(const char *source, uint16_t **name, uint8_t *name_length, int filename);
int       ntfs_name_from_utf8(const char *source, uint16_t **name, uint8_t *name_length);

/* ntfs_super.c */
int ntfs_prepare_write(ntfs_mount_t *mnt);
int ntfs_set_volume_dirty(ntfs_mount_t *mnt, int dirty);
int ntfs_clear_owned_dirty(ntfs_mount_t *mnt);
int ntfs_transaction_begin(ntfs_mount_t *mnt, fs_txn_t *transaction, uint32_t credits);
int ntfs_transaction_finish(ntfs_mount_t *mnt, fs_txn_t *transaction, int status);

/* ntfs_mft.c */
int      ntfs_record_touch(uint8_t *record, uint32_t record_size, int data_changed);
int      ntfs_record_unpack(ntfs_mount_t *mnt, uint8_t *record, uint32_t record_size);
int      ntfs_record_pack(ntfs_mount_t *mnt, uint8_t *record, uint32_t record_size);
int      ntfs_file_name_replace(ntfs_mount_t *mnt, uint8_t *record, uint64_t parent_reference, const uint16_t *old_name, uint8_t old_length, const uint16_t *new_name, uint8_t new_length);
int      ntfs_file_name_add(ntfs_mount_t *mnt, uint8_t *record, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, uint32_t file_attributes, uint64_t data_size);
int      ntfs_file_name_remove(ntfs_mount_t *mnt, uint8_t *record, uint64_t parent_reference, const uint16_t *name, uint8_t name_length);
int      ntfs_mft_bitmap_find_free(ntfs_mount_t *mnt, uint64_t *record_number);
int      ntfs_mft_bitmap_set(ntfs_mount_t *mnt, uint64_t record_number, int allocated);
uint32_t ntfs_resident_attribute(uint8_t *record, uint32_t offset, uint32_t type, uint16_t instance, const uint16_t *attribute_name, uint8_t attribute_name_length, const uint8_t *value,
                                 uint32_t value_length);
int      ntfs_build_file_record(ntfs_mount_t *mnt, uint8_t *record, uint64_t record_number, uint16_t sequence, uint64_t parent_reference, const uint16_t *name, uint8_t name_length, int directory);
int      ntfs_build_symlink_record(ntfs_mount_t *mnt, uint8_t *record, uint64_t record_number, uint16_t sequence, uint64_t parent_reference, const uint16_t *name, uint8_t name_length,
                                   const uint16_t *target, uint8_t target_length, int relative);
int      ntfs_mft_bootstrap_runlist(ntfs_mount_t *mnt);
int      ntfs_mft_read(ntfs_mount_t *mnt, uint64_t mft_no, uint8_t *buf);
int      ntfs_mft_write(ntfs_mount_t *mnt, uint64_t mft_no, const uint8_t *buf);
int      ntfs_record_remove_attribute(uint8_t *record, uint32_t record_size, uint32_t offset);

/* ntfs_runlist.c */
int     ntfs_dev_read(ntfs_mount_t *mnt, uint64_t byte_off, uint8_t *buf, size_t size);
int     ntfs_dev_write(ntfs_mount_t *mnt, uint64_t byte_off, const uint8_t *buf, size_t size);
int     ntfs_runlist_parse(uint8_t *rl, int len, int64_t *vcn, int64_t *lcn, int64_t *run, int max);
int     ntfs_runlist_encode(const int64_t *lcn, const int64_t *run, int count, uint8_t *out, uint32_t capacity);
int     ntfs_read_by_runlist(ntfs_mount_t *mnt, uint8_t *rl, int rl_len, uint64_t offset, uint8_t *buf, size_t size, uint64_t max_size);
int64_t ntfs_write_by_runlist(ntfs_mount_t *mnt, uint8_t *rl, int rl_len, uint64_t offset, const uint8_t *buf, size_t size, uint64_t max_size);
int     ntfs_zero_by_runlist(ntfs_mount_t *mnt, uint8_t *rl, int rl_len, uint64_t offset, uint64_t size, uint64_t max_size);
int     ntfs_bitmap_find_free(ntfs_mount_t *mnt, uint64_t count, int64_t *extent_lcn, int64_t *extent_length, int *extent_count);
int     ntfs_bitmap_change_extents(ntfs_mount_t *mnt, const int64_t *extent_lcn, const int64_t *extent_length, int extent_count, int allocated);
size_t  ntfs_nonresident_grow(ntfs_handle_t *h, uint8_t *mft, attr_rec_t *attribute, uint32_t length, uint16_t mapping_offset, uint64_t offset, const uint8_t *buffer, size_t size, uint64_t data_size,
                              uint64_t initialized_size);
size_t  ntfs_resident_convert(ntfs_handle_t *h, uint8_t *mft, attr_rec_t *attribute, uint32_t old_length, uint16_t value_offset, uint32_t value_length, uint64_t offset, const uint8_t *buffer,
                              size_t size);

/* ntfs_vfs.c - registration */
#if CONFIG_NTFS_FS
int ntfs_vfs_regist(void);
#else
static inline int ntfs_vfs_regist(void)
{
    return 0;
}
#endif

#endif // INCLUDE_NTFS_H_
