/*
 *
 *      extfs_jnl.c
 *      Native ext journal recovery and transaction backend
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/extfs/extfs.h>
#include <fs/extfs/extfs_jnl.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <libs/util/crc32c.h>
#include <mem/heap.h>

#define EXTFS_JNL_CRC32C_CHKSUM 4U
#define EXTFS_JNL_CRC32_CHKSUM  1U
#define EXTFS_JNL_KNOWN_COMPAT  EXTFS_JNL_FEATURE_COMPAT_CHECKSUM
#define EXTFS_JNL_KNOWN_INCOMPAT \
    (EXTFS_JNL_FEATURE_INCOMPAT_REVOKE | EXTFS_JNL_FEATURE_INCOMPAT_64BIT | EXTFS_JNL_FEATURE_INCOMPAT_ASYNC_COMMIT | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)

typedef struct extfs_jnl_record {
        uint64_t                 home;
        uint8_t                 *data;
        struct extfs_jnl_record *next;
} extfs_jnl_record_t;

/*
 * Overview
 * extfs_jnl.c implements the ext4 journal. Metadata writes are
 * staged into a transaction and written to the journal before the
 * real blocks are updated, so a crash mid-update can be recovered
 * by replaying the journal.
 */

typedef struct extfs_jnl_revoke {
        uint64_t                 block;
        uint32_t                 sequence;
        struct extfs_jnl_revoke *next;
} extfs_jnl_revoke_t;

typedef struct extfs_journal {
        extfs_sb_info_t    *sb;
        extfs_handle_t     *inode;
        uint8_t            *super_buffer;
        uint32_t            block_size;
        uint32_t            max_length;
        uint32_t            first;
        uint32_t            usable_end;
        uint32_t            sequence;
        uint32_t            start;
        uint32_t            head;
        uint32_t            compat;
        uint32_t            incompat;
        uint32_t            checksum_seed;
        uint32_t            transaction_sequence;
        extfs_jnl_record_t *records;
        extfs_jnl_record_t *tail;
        uint32_t            record_count;
} extfs_journal_t;

/* Read a big-endian 16-bit value. */
static uint16_t extfs_jnl_get_be16(const void *address)
{
    const uint8_t *p = address;
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* Read a big-endian 32-bit value. */
static uint32_t extfs_jnl_get_be32(const void *address)
{
    const uint8_t *p = address;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Write a big-endian 16-bit value. */
static void extfs_jnl_put_be16(void *address, uint16_t value)
{
    uint8_t *p = address;
    p[0]       = (uint8_t)(value >> 8);
    p[1]       = (uint8_t)value;
}

/* Write a big-endian 32-bit value. */
static void extfs_jnl_put_be32(void *address, uint32_t value)
{
    uint8_t *p = address;
    p[0]       = (uint8_t)(value >> 24);
    p[1]       = (uint8_t)(value >> 16);
    p[2]       = (uint8_t)(value >> 8);
    p[3]       = (uint8_t)value;
}

/* Update a legacy CRC32 over the journal in big-endian byte order. */
static uint32_t extfs_jnl_crc32_be(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size--) {
        crc ^= (uint32_t)*bytes++ << 24;
        for (uint32_t bit = 0; bit < 8; bit++) crc = (crc << 1) ^ (0x04C11DB7U & (uint32_t) - (int32_t)(crc >> 31));
    }
    return crc;
}

/* Advance to the next journal block, wrapping at the usable end. */
static uint32_t extfs_jnl_next_block(const extfs_journal_t *journal, uint32_t block)
{
    block++;
    return block >= journal->usable_end ? journal->first : block;
}

/* Read or write one journal block through the journal inode's mapping. */
static int extfs_jnl_logical_io(extfs_journal_t *journal, uint32_t logical, void *data, int write)
{
    uint32_t physical;
    if (!journal || !data || (journal->max_length && logical >= journal->max_length)) return -EINVAL;
    physical = extfs_map_block(journal->inode, logical, 0);
    if (!physical || physical >= journal->sb->blocks_count) return -EIO;
    uint64_t offset = (uint64_t)physical * journal->block_size;
    return write ? blockdev_write_bytes(&journal->sb->device, offset, data, journal->block_size) : blockdev_read_bytes(&journal->sb->device, offset, data, journal->block_size);
}

/* Read one journal block. */
static int extfs_jnl_read(extfs_journal_t *journal, uint32_t logical, void *data)
{
    return extfs_jnl_logical_io(journal, logical, data, 0);
}

/* Write one journal block. */
static int extfs_jnl_write(extfs_journal_t *journal, uint32_t logical, void *data)
{
    return extfs_jnl_logical_io(journal, logical, data, 1);
}

/* Write the journal superblock, updating its checksum when enabled. */
static int extfs_jnl_write_super(extfs_journal_t *journal, uint32_t start, uint32_t sequence)
{
    extfs_jnl_superblock_t *super = (extfs_jnl_superblock_t *)journal->super_buffer;
    extfs_jnl_put_be32(&super->start, start);
    extfs_jnl_put_be32(&super->sequence, sequence);
    if (journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)) {
        extfs_jnl_put_be32(&super->checksum, 0);
        extfs_jnl_put_be32(&super->checksum, crc32c_update(~0U, super, sizeof(*super)));
    }
    int status = extfs_jnl_write(journal, 0, journal->super_buffer);
    if (status == EOK) {
        journal->start    = start;
        journal->sequence = sequence;
    }
    return status;
}

/* Free all staged journal records. */
static void extfs_jnl_free_records(extfs_journal_t *journal)
{
    extfs_jnl_record_t *record = journal->records;
    while (record) {
        extfs_jnl_record_t *next = record->next;
        free(record->data);
        free(record);
        record = next;
    }
    journal->records      = 0;
    journal->tail         = 0;
    journal->record_count = 0;
}

/* Size of one descriptor tag for the journal's feature set. */
static size_t extfs_jnl_tag_size(const extfs_journal_t *journal)
{
    if (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3) return 16;
    return (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_64BIT) ? 12 : 8;
}

/* Parse a descriptor tag into home block, flags and checksum. */
static int extfs_jnl_parse_tag(extfs_journal_t *journal, const uint8_t *tag, uint64_t *home, uint32_t *flags, uint32_t *checksum)
{
    *home = extfs_jnl_get_be32(tag);
    if (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3) {
        *flags = extfs_jnl_get_be32(tag + 4);
        *home |= (uint64_t)extfs_jnl_get_be32(tag + 8) << 32;
        *checksum = extfs_jnl_get_be32(tag + 12);
    } else {
        *checksum = extfs_jnl_get_be16(tag + 4);
        *flags    = extfs_jnl_get_be16(tag + 6);
        if (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_64BIT) *home |= (uint64_t)extfs_jnl_get_be32(tag + 8) << 32;
    }
    return *home < journal->sb->blocks_count ? EOK : -EIO;
}

/* Compute the CRC32C checksum of a journaled data block. */
static uint32_t extfs_jnl_data_checksum(extfs_journal_t *journal, uint32_t sequence, const void *data)
{
    uint8_t be_sequence[4];
    extfs_jnl_put_be32(be_sequence, sequence);
    uint32_t checksum = crc32c_update(journal->checksum_seed, be_sequence, sizeof(be_sequence));
    return crc32c_update(checksum, data, journal->block_size);
}

/* Validate a commit block's magic, type, sequence and checksum. */
static int extfs_jnl_verify_commit(extfs_journal_t *journal, uint8_t *block, uint32_t sequence)
{
    if (extfs_jnl_get_be32(block) != EXTFS_JNL_MAGIC_NUMBER || extfs_jnl_get_be32(block + 4) != EXTFS_JNL_COMMIT_BLOCK || extfs_jnl_get_be32(block + 8) != sequence) return -EIO;
    if (!(journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3))) return EOK;
    if (block[12] != EXTFS_JNL_CRC32C_CHKSUM || block[13] != 4) return -EIO;
    uint32_t stored = extfs_jnl_get_be32(block + 16);
    extfs_jnl_put_be32(block + 16, 0);
    uint32_t calculated = crc32c_update(journal->checksum_seed, block, journal->block_size);
    extfs_jnl_put_be32(block + 16, stored);
    if (stored != calculated) {
        plogk("extfs: Drive %u: journal commit block %u checksum mismatch.\n", journal->sb->device.drive, sequence);
        return -EIO;
    }
    return EOK;
}

/* Verify the descriptor block tail checksum. */
static int extfs_jnl_verify_descriptor(extfs_journal_t *journal, uint8_t *block)
{
    if (!(journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3))) return EOK;
    uint8_t *tail   = block + journal->block_size - sizeof(uint32_t);
    uint32_t stored = extfs_jnl_get_be32(tail);
    extfs_jnl_put_be32(tail, 0);
    uint32_t calculated = crc32c_update(journal->checksum_seed, block, journal->block_size);
    extfs_jnl_put_be32(tail, stored);
    if (stored != calculated) {
        plogk("extfs: Drive %u: journal descriptor block checksum mismatch (sequence %u)\n", journal->sb->device.drive, extfs_jnl_get_be32(block + 8));
        return -EIO;
    }
    return EOK;
}

/* Store the descriptor block tail checksum. */
static void extfs_jnl_set_descriptor_checksum(extfs_journal_t *journal, uint8_t *block)
{
    if (!(journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3))) return;
    uint8_t *tail = block + journal->block_size - sizeof(uint32_t);
    extfs_jnl_put_be32(tail, 0);
    extfs_jnl_put_be32(tail, crc32c_update(journal->checksum_seed, block, journal->block_size));
}

/* Walk one committed transaction. Descriptor data blocks are checked while walking. */
static int extfs_jnl_walk_transaction(extfs_journal_t *journal, uint32_t start, uint32_t sequence, uint32_t *next, extfs_jnl_revoke_t **revokes, int replay)
{
    uint8_t *block           = malloc(journal->block_size);
    uint8_t *data            = malloc(journal->block_size);
    uint32_t cursor          = start;
    uint32_t transaction_crc = ~0U;
    uint32_t walked          = 0;
    int      status          = block && data ? EOK : -ENOMEM;
    while (status == EOK) {
        if (++walked > journal->usable_end - journal->first) {
            status = -EIO;
            break;
        }
        status = extfs_jnl_read(journal, cursor, block);
        if (status != EOK) break;
        uint32_t magic = extfs_jnl_get_be32(block);
        uint32_t type  = extfs_jnl_get_be32(block + 4);
        uint32_t seq   = extfs_jnl_get_be32(block + 8);
        if (magic != EXTFS_JNL_MAGIC_NUMBER || seq != sequence) {
            status = -EIO;
            break;
        }
        if (type == EXTFS_JNL_DESCRIPTOR_BLOCK) {
            status = extfs_jnl_verify_descriptor(journal, block);
            if (status != EOK) break;
            if (journal->compat & EXTFS_JNL_FEATURE_COMPAT_CHECKSUM) transaction_crc = extfs_jnl_crc32_be(transaction_crc, block, journal->block_size);
            size_t tag_size = extfs_jnl_tag_size(journal);
            size_t offset   = sizeof(extfs_jnl_header_t);
            size_t limit    = journal->block_size;
            if (journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)) limit -= 4;
            int last = 0;
            while (!last) {
                uint64_t home;
                uint32_t flags, checksum;
                if (offset + tag_size > limit) {
                    status = -EIO;
                    break;
                }
                status = extfs_jnl_parse_tag(journal, block + offset, &home, &flags, &checksum);
                if (status != EOK || (flags & EXTFS_JNL_FLAG_DELETED)) {
                    if (status == EOK) status = -EIO;
                    break;
                }
                offset += tag_size;
                if (!(flags & EXTFS_JNL_FLAG_SAME_UUID)) {
                    if (offset + 16 > limit || memcmp(block + offset, journal->sb->es->s_uuid, 16) != 0) {
                        status = -EIO;
                        break;
                    }
                    offset += 16;
                }
                cursor = extfs_jnl_next_block(journal, cursor);
                status = extfs_jnl_read(journal, cursor, data);
                if (status != EOK) break;
                if (journal->compat & EXTFS_JNL_FEATURE_COMPAT_CHECKSUM) transaction_crc = extfs_jnl_crc32_be(transaction_crc, data, journal->block_size);
                if (journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)) {
                    uint32_t actual = extfs_jnl_data_checksum(journal, sequence, data);
                    if ((journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3) ? actual != checksum : (uint16_t)actual != (uint16_t)checksum) {
                        plogk("extfs: Drive %u: journal data block checksum mismatch (sequence %u, home %llu)\n", journal->sb->device.drive, sequence, (unsigned long long)home);
                        status = -EIO;
                        break;
                    }
                }
                if (flags & EXTFS_JNL_FLAG_ESCAPE) extfs_jnl_put_be32(data, EXTFS_JNL_MAGIC_NUMBER);
                if (replay) {
                    int revoked = 0;
                    for (extfs_jnl_revoke_t *item = *revokes; item; item = item->next)
                        if (item->block == home && item->sequence >= sequence) {
                            revoked = 1;
                            break;
                        }
                    if (!revoked) status = blockdev_write_bytes(&journal->sb->device, home * (uint64_t)journal->block_size, data, journal->block_size);
                }
                last = !!(flags & EXTFS_JNL_FLAG_LAST_TAG);
            }
        } else if (type == EXTFS_JNL_REVOKE_BLOCK) {
            uint32_t bytes      = extfs_jnl_get_be32(block + 12);
            size_t   entry_size = (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
            if (bytes < 16 || bytes > journal->block_size || (bytes - 16) % entry_size) {
                status = -EIO;
                break;
            }
            if (revokes && !replay) {
                for (size_t offset = 16; offset < bytes; offset += entry_size) {
                    uint64_t value = extfs_jnl_get_be32(block + offset);
                    if (entry_size == 8) value = value << 32 | extfs_jnl_get_be32(block + offset + 4);
                    extfs_jnl_revoke_t *item;
                    for (item = *revokes; item && item->block != value; item = item->next) {}
                    if (!item) {
                        item = calloc(1, sizeof(*item));
                        if (!item) {
                            status = -ENOMEM;
                            break;
                        }
                        item->block = value;
                        item->next  = *revokes;
                        *revokes    = item;
                    }
                    item->sequence = sequence;
                }
            }
        } else if (type == EXTFS_JNL_COMMIT_BLOCK) {
            if (journal->compat & EXTFS_JNL_FEATURE_COMPAT_CHECKSUM) {
                status = block[12] == EXTFS_JNL_CRC32_CHKSUM && block[13] == 4 && extfs_jnl_get_be32(block + 16) == transaction_crc ? EOK : -EIO;
            } else {
                status = extfs_jnl_verify_commit(journal, block, sequence);
            }
            if (status == EOK) *next = extfs_jnl_next_block(journal, cursor);
            break;
        } else {
            status = -EIO;
            break;
        }
        cursor = extfs_jnl_next_block(journal, cursor);
    }
    free(data);
    free(block);
    return status;
}

/* Recompute the legacy v1 transaction checksum over descriptor and data blocks. */
static int extfs_jnl_v1_transaction_checksum(extfs_journal_t *journal, uint32_t start, uint32_t end, uint32_t sequence, uint32_t *result)
{
    uint8_t *block  = malloc(journal->block_size);
    uint8_t *data   = malloc(journal->block_size);
    uint32_t cursor = start, crc = ~0U;
    int      status = block && data ? EOK : -ENOMEM;
    while (status == EOK && cursor != end) {
        status = extfs_jnl_read(journal, cursor, block);
        if (status != EOK || extfs_jnl_get_be32(block) != EXTFS_JNL_MAGIC_NUMBER || extfs_jnl_get_be32(block + 4) != EXTFS_JNL_DESCRIPTOR_BLOCK || extfs_jnl_get_be32(block + 8) != sequence) {
            if (status == EOK) status = -EIO;
            break;
        }
        crc           = extfs_jnl_crc32_be(crc, block, journal->block_size);
        size_t offset = sizeof(extfs_jnl_header_t), tag_size = extfs_jnl_tag_size(journal);
        int    last = 0;
        while (!last && status == EOK) {
            if (offset + tag_size > journal->block_size) {
                status = -EIO;
                break;
            }
            uint32_t flags = (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3) ? extfs_jnl_get_be32(block + offset + 4) : extfs_jnl_get_be16(block + offset + 6);
            offset += tag_size + ((flags & EXTFS_JNL_FLAG_SAME_UUID) ? 0 : 16);
            cursor = extfs_jnl_next_block(journal, cursor);
            if (cursor == end || (status = extfs_jnl_read(journal, cursor, data)) != EOK) { // NOLINT(bugprone-assignment-in-if-condition)
                status = -EIO;
                break;
            }
            crc  = extfs_jnl_crc32_be(crc, data, journal->block_size);
            last = !!(flags & EXTFS_JNL_FLAG_LAST_TAG);
        }
        cursor = extfs_jnl_next_block(journal, cursor);
    }
    free(data);
    free(block);
    if (status == EOK) *result = crc;
    return status;
}

/* Free a revoke list. */
static void extfs_jnl_free_revokes(extfs_jnl_revoke_t *revoke)
{
    while (revoke) {
        extfs_jnl_revoke_t *next = revoke->next;
        free(revoke);
        revoke = next;
    }
}

/* Recover the journal by replaying committed transactions. */
static int extfs_jnl_recover(void *context)
{
    extfs_journal_t    *journal = context;
    extfs_jnl_revoke_t *revokes = 0;
    uint32_t            cursor, sequence, end_sequence;
    int                 status = EOK;
    if (!journal->start) return EOK;

    /* Pass one: find the first incomplete transaction without changing home blocks. */
    cursor   = journal->start;
    sequence = journal->sequence;
    for (;;) {
        uint32_t next;
        status = extfs_jnl_walk_transaction(journal, cursor, sequence, &next, &revokes, 0);
        if (status != EOK) break;
        cursor = next;
        sequence++;
        if (cursor == journal->start) return -EIO;
    }
    end_sequence = sequence;

    /* Pass two: collect revokes from every complete transaction. */
    extfs_jnl_free_revokes(revokes);
    revokes = 0;
    cursor  = journal->start;
    for (sequence = journal->sequence; sequence != end_sequence; sequence++) {
        uint32_t next;
        status = extfs_jnl_walk_transaction(journal, cursor, sequence, &next, &revokes, 0);
        if (status != EOK) goto out;
        cursor = next;
    }

    /* Pass three: replay only committed, non-revoked metadata. */
    cursor = journal->start;
    for (sequence = journal->sequence; sequence != end_sequence; sequence++) {
        uint32_t next;
        status = extfs_jnl_walk_transaction(journal, cursor, sequence, &next, &revokes, 1);
        if (status != EOK) goto out;
        cursor = next;
    }
    status = blockdev_flush(&journal->sb->device);
    if (status == EOK) status = extfs_jnl_write_super(journal, 0, end_sequence);
    if (status == EOK) status = blockdev_flush(&journal->sb->device);
    journal->head = cursor;
out:
    extfs_jnl_free_revokes(revokes);
    return status;
}

/* Begin a journal transaction, checking the ring has space. */
static int extfs_jnl_begin(void *context, uint32_t transaction_id, uint32_t buffers)
{
    extfs_journal_t *journal = context;
    (void)transaction_id;
    extfs_jnl_free_records(journal);
    journal->transaction_sequence = journal->sequence;
    size_t descriptor_space       = journal->block_size - sizeof(extfs_jnl_header_t);
    if (journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)) descriptor_space -= 4;
    uint32_t tags = descriptor_space > 16 ? (uint32_t)((descriptor_space - 16) / extfs_jnl_tag_size(journal)) : 0;
    if (!tags) return -ENOSPC;
    uint64_t descriptors = (buffers + tags - 1) / tags;
    uint64_t required    = descriptors + buffers + 1;
    return required < journal->usable_end - journal->first ? EOK : -ENOSPC;
}

/* Stage one metadata block for the current transaction. */
static int extfs_jnl_log_block(void *context, uint32_t transaction_id, uint64_t home_block, const void *data, uint32_t flags)
{
    extfs_journal_t    *journal = context;
    extfs_jnl_record_t *record;
    (void)transaction_id;
    (void)flags;
    if (home_block >= journal->sb->blocks_count) return -EIO;
    record = calloc(1, sizeof(*record));
    if (!record) return -ENOMEM;
    record->data = malloc(journal->block_size);
    if (!record->data) {
        free(record);
        return -ENOMEM;
    }
    memcpy(record->data, data, journal->block_size);
    record->home = home_block;
    if (journal->tail)
        journal->tail->next = record;
    else
        journal->records = record;
    journal->tail = record;
    journal->record_count++;
    return EOK;
}

/* Append the descriptor, data and commit blocks to the journal ring. */
static int extfs_jnl_commit(void *context, uint32_t transaction_id)
{
    extfs_journal_t    *journal    = context;
    extfs_jnl_record_t *record     = journal->records;
    uint8_t            *descriptor = malloc(journal->block_size);
    uint8_t            *copy       = malloc(journal->block_size);
    uint32_t            cursor     = journal->head;
    uint32_t            sequence   = journal->transaction_sequence;
    int                 status     = descriptor && copy ? EOK : -ENOMEM;
    (void)transaction_id;
    if (status != EOK) goto out;
    if (!record) goto out;

    status = extfs_jnl_write_super(journal, cursor, sequence);
    if (status == EOK) status = blockdev_flush(&journal->sb->device);
    while (record && status == EOK) {
        memset(descriptor, 0, journal->block_size);
        extfs_jnl_put_be32(descriptor, EXTFS_JNL_MAGIC_NUMBER);
        extfs_jnl_put_be32(descriptor + 4, EXTFS_JNL_DESCRIPTOR_BLOCK);
        extfs_jnl_put_be32(descriptor + 8, sequence);
        uint32_t descriptor_block = cursor;
        cursor                    = extfs_jnl_next_block(journal, cursor);
        size_t offset             = sizeof(extfs_jnl_header_t);
        size_t limit              = journal->block_size;
        if (journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)) limit -= 4;
        int first = 1;
        while (record) {
            size_t required = extfs_jnl_tag_size(journal) + (first ? 16 : 0);
            if (offset + required > limit) break;
            uint32_t flags = first ? 0 : EXTFS_JNL_FLAG_SAME_UUID;
            if (!record->next || offset + required + extfs_jnl_tag_size(journal) > limit) flags |= EXTFS_JNL_FLAG_LAST_TAG;
            memcpy(copy, record->data, journal->block_size);
            if (extfs_jnl_get_be32(copy) == EXTFS_JNL_MAGIC_NUMBER) {
                extfs_jnl_put_be32(copy, 0);
                flags |= EXTFS_JNL_FLAG_ESCAPE;
            }
            uint32_t checksum = extfs_jnl_data_checksum(journal, sequence, copy);
            extfs_jnl_put_be32(descriptor + offset, (uint32_t)record->home);
            if (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3) {
                extfs_jnl_put_be32(descriptor + offset + 4, flags);
                extfs_jnl_put_be32(descriptor + offset + 8, (uint32_t)(record->home >> 32));
                extfs_jnl_put_be32(descriptor + offset + 12, checksum);
            } else {
                extfs_jnl_put_be16(descriptor + offset + 4, (uint16_t)checksum);
                extfs_jnl_put_be16(descriptor + offset + 6, (uint16_t)flags);
                if (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_64BIT) extfs_jnl_put_be32(descriptor + offset + 8, (uint32_t)(record->home >> 32));
            }
            offset += extfs_jnl_tag_size(journal);
            if (first) {
                memcpy(descriptor + offset, journal->sb->es->s_uuid, 16);
                offset += 16;
            }
            status = extfs_jnl_write(journal, cursor, copy);
            if (status != EOK) break;
            cursor = extfs_jnl_next_block(journal, cursor);
            record = record->next;
            first  = 0;
            if (flags & EXTFS_JNL_FLAG_LAST_TAG) break;
        }
        if (status == EOK) {
            extfs_jnl_set_descriptor_checksum(journal, descriptor);
            status = extfs_jnl_write(journal, descriptor_block, descriptor);
        }
    }
    if (status == EOK) status = blockdev_flush(&journal->sb->device);
    if (status == EOK) {
        uint32_t transaction_crc = 0;
        if (journal->compat & EXTFS_JNL_FEATURE_COMPAT_CHECKSUM) status = extfs_jnl_v1_transaction_checksum(journal, journal->head, cursor, sequence, &transaction_crc);
        if (status != EOK) goto out;
        memset(copy, 0, journal->block_size);
        extfs_jnl_put_be32(copy, EXTFS_JNL_MAGIC_NUMBER);
        extfs_jnl_put_be32(copy + 4, EXTFS_JNL_COMMIT_BLOCK);
        extfs_jnl_put_be32(copy + 8, sequence);
        if (journal->compat & EXTFS_JNL_FEATURE_COMPAT_CHECKSUM) {
            copy[12] = EXTFS_JNL_CRC32_CHKSUM;
            copy[13] = 4;
            extfs_jnl_put_be32(copy + 16, transaction_crc);
        } else if (journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)) {
            copy[12] = EXTFS_JNL_CRC32C_CHKSUM;
            copy[13] = 4;
            extfs_jnl_put_be32(copy + 16, crc32c_update(journal->checksum_seed, copy, journal->block_size));
        }
        status = extfs_jnl_write(journal, cursor, copy);
        cursor = extfs_jnl_next_block(journal, cursor);
    }
    if (status == EOK) status = blockdev_flush(&journal->sb->device);
    if (status == EOK) journal->head = cursor;
out:
    free(copy);
    free(descriptor);
    return status;
}

/* Advance the journal sequence past a fully committed transaction. */
static int extfs_jnl_checkpoint(void *context, uint32_t transaction_id)
{
    extfs_journal_t *journal = context;
    (void)transaction_id;
    int status = extfs_jnl_write_super(journal, 0, journal->transaction_sequence + 1);
    if (status == EOK) {
        journal->sequence = journal->transaction_sequence + 1;
        extfs_jnl_free_records(journal);
    }
    return status;
}

/* Record the abort error in the journal superblock and drop staged blocks. */
static void extfs_jnl_abort(void *context, uint32_t transaction_id, int error)
{
    extfs_journal_t        *journal = context;
    extfs_jnl_superblock_t *super   = (extfs_jnl_superblock_t *)journal->super_buffer;
    (void)transaction_id;
    extfs_jnl_put_be32(&super->error, (uint32_t)(error < 0 ? -error : error));
    (void)extfs_jnl_write_super(journal, journal->start, journal->sequence);
    (void)blockdev_flush(&journal->sb->device);
    extfs_jnl_free_records(journal);
}

static const fs_txn_backend_ops_t extfs_jnl_ops = {
    .recover    = extfs_jnl_recover,
    .begin      = extfs_jnl_begin,
    .log_block  = extfs_jnl_log_block,
    .commit     = extfs_jnl_commit,
    .checkpoint = extfs_jnl_checkpoint,
    .abort      = extfs_jnl_abort,
};

/* Open and validate the journal inode, reading its superblock. */
int extfs_jnl_open(struct extfs_sb_info *sb, extfs_journal_t **out)
{
    extfs_journal_t        *journal;
    extfs_jnl_superblock_t *super;
    ext2_inode_t            inode;
    int                     status;
    if (!sb || !out || !sb->es->s_journal_inum) return -EINVAL;
    *out    = 0;
    journal = calloc(1, sizeof(*journal));
    if (!journal) return -ENOMEM;
    journal->sb           = sb;
    journal->inode        = extfs_alloc_handle(sb, sb->es->s_journal_inum);
    journal->super_buffer = malloc(sb->block_size);
    if (!journal->inode || !journal->super_buffer) {
        extfs_jnl_close(journal);
        return -ENOMEM;
    }
    status = extfs_read_inode_raw(sb, sb->es->s_journal_inum, &inode);
    if (status != EOK || (inode.i_mode & 0xF000) != EXT2_S_IFREG) {
        extfs_jnl_close(journal);
        return -EINVAL;
    }
    journal->block_size = sb->block_size;
    status              = extfs_jnl_read(journal, 0, journal->super_buffer);
    if (status != EOK) {
        extfs_jnl_close(journal);
        return status;
    }
    super                = (extfs_jnl_superblock_t *)journal->super_buffer;
    uint32_t type        = extfs_jnl_get_be32(&super->header.block_type);
    journal->max_length  = extfs_jnl_get_be32(&super->max_length);
    journal->first       = extfs_jnl_get_be32(&super->first);
    journal->sequence    = extfs_jnl_get_be32(&super->sequence);
    journal->start       = extfs_jnl_get_be32(&super->start);
    journal->compat      = extfs_jnl_get_be32(&super->feature_compat);
    journal->incompat    = extfs_jnl_get_be32(&super->feature_incompat);
    uint32_t fast_blocks = extfs_jnl_get_be32(&super->fast_commit_blocks);
    uint64_t inode_size  = inode.i_size | ((uint64_t)inode.i_dir_acl << 32);
    if (extfs_jnl_get_be32(&super->header.magic) != EXTFS_JNL_MAGIC_NUMBER || (type != EXTFS_JNL_SUPERBLOCK_V1 && type != EXTFS_JNL_SUPERBLOCK_V2)
        || extfs_jnl_get_be32(&super->block_size) != sb->block_size || journal->max_length > inode_size / sb->block_size || journal->first == 0 || journal->first >= journal->max_length
        || journal->start >= journal->max_length || journal->compat & ~EXTFS_JNL_KNOWN_COMPAT || journal->incompat & ~EXTFS_JNL_KNOWN_INCOMPAT || fast_blocks >= journal->max_length - journal->first
        || ((journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_ASYNC_COMMIT) && !(journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)))
        || ((journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2) && (journal->incompat & EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3))) {
        extfs_jnl_close(journal);
        return -EOPNOTSUPP;
    }
    if ((journal->compat & EXTFS_JNL_FEATURE_COMPAT_CHECKSUM) && (journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3))) {
        extfs_jnl_close(journal);
        return -EINVAL;
    }
    if (memcmp(super->uuid, sb->es->s_uuid, 16) != 0) {
        extfs_jnl_close(journal);
        return -EINVAL;
    }
    journal->checksum_seed = crc32c_update(~0U, super->uuid, 16);
    if (journal->incompat & (EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V2 | EXTFS_JNL_FEATURE_INCOMPAT_CSUM_V3)) {
        if (super->checksum_type != EXTFS_JNL_CRC32C_CHKSUM) {
            extfs_jnl_close(journal);
            return -EOPNOTSUPP;
        }
        uint32_t stored = extfs_jnl_get_be32(&super->checksum);
        extfs_jnl_put_be32(&super->checksum, 0);
        uint32_t calculated = crc32c_update(~0U, super, sizeof(*super));
        extfs_jnl_put_be32(&super->checksum, stored);
        if (stored != calculated) {
            extfs_jnl_close(journal);
            return -EIO;
        }
    }
    journal->usable_end = journal->max_length - fast_blocks;
    journal->head       = extfs_jnl_get_be32(&super->head);
    if (journal->head < journal->first || journal->head >= journal->usable_end) journal->head = journal->first;
    *out = journal;
    return EOK;
}

/* Release all journal resources. */
void extfs_jnl_close(extfs_journal_t *journal)
{
    if (!journal) return;
    extfs_jnl_free_records(journal);
    free(journal->super_buffer);
    free(journal->inode);
    free(journal);
}

/* Expose the journal transaction backend operations. */
const fs_txn_backend_ops_t *extfs_jnl_backend_ops(void)
{
    return &extfs_jnl_ops;
}
