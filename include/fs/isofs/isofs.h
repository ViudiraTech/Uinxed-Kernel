/*
 *
 *      isofs.h
 *      ISO 9660 filesystem public header
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ISOFS_H_
#define INCLUDE_ISOFS_H_

#include <drivers/block/core/blockdev.h>
#include <libs/std/stddef.h>

#define ISOFS_BLOCK_SIZE   2048
#define ISOFS_MAX_PATH     256
#define ISOFS_RR_MAX_CE    32
#define ISOFS_MAX_SECTIONS 100

#define ISO_STANDARD_ID "CD001"
#define HS_STANDARD_ID  "CDROM"

#define ISO_VD_PRIMARY       1
#define ISO_VD_SUPPLEMENTARY 2
#define ISO_VD_END           255

#define ISOFS_INVALID_MODE ((uint16_t) - 1)

/* ISO date conversion */

#define ISO_DATE_HIGH_SIERRA (1 << 0)
#define ISO_DATE_LONG_FORM   (1 << 1)

/* ISO 9660 on-disk structures */

typedef struct iso_directory_record {
        uint8_t length;
        uint8_t ext_attr_length;
        uint8_t extent[8];
        uint8_t size[8];
        uint8_t date[7];
        uint8_t flags[1];
        uint8_t file_unit_size[1];
        uint8_t interleave[1];
        uint8_t vol_seq_number[4];
        uint8_t name_len[1];
        char    name[];
} __attribute__((packed)) iso_directory_record_t;

_Static_assert(sizeof(iso_directory_record_t) == 33, "ISO9660 directory record fixed size");

typedef struct iso_volume_descriptor {
        uint8_t type;
        char    id[5];
        uint8_t version;
        uint8_t data[2041];
} __attribute__((packed)) iso_volume_descriptor_t;

_Static_assert(sizeof(iso_volume_descriptor_t) == 2048, "ISO9660 volume descriptor occupies one sector");

typedef struct iso_primary_descriptor {
        uint8_t type;
        char    id[5];
        uint8_t version;
        uint8_t unused1;
        char    system_id[32];
        char    volume_id[32];
        uint8_t unused2[8];
        uint8_t volume_space_size[8];
        uint8_t unused3[32];
        uint8_t volume_set_size[4];
        uint8_t volume_seq_number[4];
        uint8_t logical_block_size[4];
        uint8_t path_table_size[8];
        uint8_t type_l_path_table[4];
        uint8_t opt_type_l_path_table[4];
        uint8_t type_m_path_table[4];
        uint8_t opt_type_m_path_table[4];
        uint8_t root_directory_record[34];
        uint8_t reserved[1856];
} __attribute__((packed)) iso_primary_descriptor_t;

/*
 * ECMA-119 byte positions. The struct is 2046 (reserved[] is two bytes short of the
 * 2048-byte sector), so only the field offsets are assertable.
 */
_Static_assert(offsetof(iso_primary_descriptor_t, system_id) == 8, "ISO9660 PVD system identifier offset");
_Static_assert(offsetof(iso_primary_descriptor_t, volume_id) == 40, "ISO9660 PVD volume identifier offset");
_Static_assert(offsetof(iso_primary_descriptor_t, volume_space_size) == 80, "ISO9660 PVD volume space size offset");
_Static_assert(offsetof(iso_primary_descriptor_t, logical_block_size) == 128, "ISO9660 PVD logical block size offset");
_Static_assert(offsetof(iso_primary_descriptor_t, root_directory_record) == 156, "ISO9660 PVD root directory record offset");

typedef struct iso_supplementary_descriptor {
        uint8_t type;
        char    id[5];
        uint8_t version;
        uint8_t flags;
        char    system_id[32];
        char    volume_id[32];
        uint8_t unused2[8];
        uint8_t volume_space_size[8];
        uint8_t escape[32];
        uint8_t volume_set_size[4];
        uint8_t volume_seq_number[4];
        uint8_t logical_block_size[4];
        uint8_t path_table_size[8];
        uint8_t type_l_path_table[4];
        uint8_t opt_type_l_path_table[4];
        uint8_t type_m_path_table[4];
        uint8_t opt_type_m_path_table[4];
        uint8_t root_directory_record[34];
        uint8_t reserved[1856];
} __attribute__((packed)) iso_supplementary_descriptor_t;

_Static_assert(offsetof(iso_supplementary_descriptor_t, root_directory_record) == 156, "ISO9660 SVD root directory record offset");

/* In-memory mount structure */

typedef struct isofs_mount {
        blockdev_device_t device;
        uint32_t          block_size;
        uint32_t          block_bits;
        uint32_t          vol_space_size;
        uint32_t          first_data_zone;
        int               high_sierra;
        int               rock_ridge;
        int               rock_offset;
        int               hide;
        int               showassoc;
        int               cruft;
        int               owns_device;

        /* internal: block read helper for Rock Ridge CE handling */
        int (*rr_read_block)(void *ctx, uint32_t block, void *buf, uint32_t size);
        void *rr_read_ctx;
} isofs_mount_t;

/* In-memory file handle */

typedef struct isofs_handle {
        isofs_mount_t          *mount;
        iso_directory_record_t *raw_de;
        uint8_t                *raw_de_buf;
        uint32_t                first_extent;
        uint64_t                size;
        uint32_t                extent_block;
        uint32_t                extent_offset;
        int                     is_dir;
        int                     is_symlink;
        int                     owns_mount;
} isofs_handle_t;

/* ISO 9660 byte-order read helpers */
uint8_t isonum_711(const uint8_t *p);

/* Read a signed 8-bit ISO9660 integer. */
int8_t isonum_712(const int8_t *p);

/* Read a little-endian 16-bit ISO9660 integer. */
uint16_t isonum_721(const uint8_t *p);

/* Read a big-endian 16-bit ISO9660 integer. */
uint16_t isonum_722(const uint8_t *p);

/* Read a 16-bit ISO9660 integer (both endiannesses). */
uint16_t isonum_723(const uint8_t *p);

/* Read a little-endian 32-bit ISO9660 integer. */
uint32_t isonum_731(const uint8_t *p);

/* Read a big-endian 32-bit ISO9660 integer. */
uint32_t isonum_732(const uint8_t *p);

/* Read a 32-bit ISO9660 integer (both endiannesses). */
uint32_t isonum_733(const uint8_t *p);

/* Isofs date to unix. */
uint64_t isofs_date_to_unix(const uint8_t *p, int flags);

/* Rock Ridge / name translation */

int  isofs_rr_translate_name(void *raw_de, char *out, int bufsize);
int  isofs_rr_filename(void *raw_de, char *out, int bufsize, isofs_mount_t *mount);
void isofs_rr_parse_inode(void *raw_de, isofs_handle_t *handle, isofs_mount_t *mount);

/* API */

#if CONFIG_ISO9660_FS
void isofs_regist(void);
#else
static inline void isofs_regist(void) {}
#endif

#endif // INCLUDE_ISOFS_H_
