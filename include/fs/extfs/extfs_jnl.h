/*
 *
 *      extfs_jnl.h
 *      Native ext journaling backend for extfs
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EXTFS_JNL_H_
#define INCLUDE_EXTFS_JNL_H_

#include <fs/core/fs_txn.h>
#include <libs/std/stdint.h>

struct extfs_sb_info;
typedef struct extfs_journal extfs_journal_t;

#define EXTFS_JNL_MAGIC_NUMBER     0xC03B3998U
#define EXTFS_JNL_DESCRIPTOR_BLOCK 1U
#define EXTFS_JNL_COMMIT_BLOCK     2U
#define EXTFS_JNL_SUPERBLOCK_V1    3U
#define EXTFS_JNL_SUPERBLOCK_V2    4U
#define EXTFS_JNL_REVOKE_BLOCK     5U

#define EXTFS_JNL_FLAG_ESCAPE    0x0001U
#define EXTFS_JNL_FLAG_SAME_UUID 0x0002U
#define EXTFS_JNL_FLAG_DELETED   0x0004U
#define EXTFS_JNL_FLAG_LAST_TAG  0x0008U

#define EXTFS_JNL_FEATURE_COMPAT_CHECKSUM       0x00000001U
#define EXTFS_JNL_FEATURE_INCOMPAT_REVOKE       0x00000001U
#define EXTFS_JNL_FEATURE_INCOMPAT_64BIT        0x00000002U
#define EXTFS_JNL_FEATURE_INCOMPAT_ASYNC_COMMIT 0x00000004U
#define EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2      0x00000008U
#define EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3      0x00000010U
#define EXTFS_JNL_FEATURE_INCOMPAT_FAST_COMMIT  0x00000020U

typedef struct extfs_jnl_header {
        uint32_t magic;
        uint32_t block_type;
        uint32_t sequence;
} __attribute__((packed)) extfs_jnl_header_t;

typedef struct extfs_jnl_superblock {
        extfs_jnl_header_t header;
        uint32_t           block_size;
        uint32_t           max_length;
        uint32_t           first;
        uint32_t           sequence;
        uint32_t           start;
        uint32_t           error;
        uint32_t           feature_compat;
        uint32_t           feature_incompat;
        uint32_t           feature_ro_compat;
        uint8_t            uuid[16];
        uint32_t           users;
        uint32_t           dynamic_super;
        uint32_t           max_transaction;
        uint32_t           max_transaction_data;
        uint8_t            checksum_type;
        uint8_t            padding2[3];
        uint32_t           fast_commit_blocks;
        uint32_t           head;
        uint32_t           padding[40];
        uint32_t           checksum;
        uint8_t            user_uuids[16 * 48];
} __attribute__((packed)) extfs_jnl_superblock_t;

int                         extfs_jnl_open(struct extfs_sb_info *sb, extfs_journal_t **journal);
void                        extfs_jnl_close(extfs_journal_t *journal);
const fs_txn_backend_ops_t *extfs_jnl_backend_ops(void);

#endif // INCLUDE_EXTFS_JNL_H_
