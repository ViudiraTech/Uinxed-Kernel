/*
 *
 *      jbd2.c
 *      Native JBD2 journal recovery and transaction backend
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/extfs/extfs.h>
#include <fs/extfs/jbd2.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/data/crc32c.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#define JBD2_CRC32C_CHKSUM 4U
#define JBD2_CRC32_CHKSUM  1U
#define JBD2_KNOWN_COMPAT  JBD2_FEATURE_COMPAT_CHECKSUM
#define JBD2_KNOWN_INCOMPAT                                                                                                          \
    (JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT | JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT | JBD2_FEATURE_INCOMPAT_CSUM_V2 \
     | JBD2_FEATURE_INCOMPAT_CSUM_V3)

typedef struct jbd2_record {
        uint64_t            home;
        uint8_t            *data;
        struct jbd2_record *next;
} jbd2_record_t;

typedef struct jbd2_revoke {
        uint64_t            block;
        uint32_t            sequence;
        struct jbd2_revoke *next;
} jbd2_revoke_t;

typedef struct extfs_journal {
        extfs_sb_info_t *sb;
        extfs_handle_t  *inode;
        uint8_t         *super_buffer;
        uint32_t         block_size;
        uint32_t         max_length;
        uint32_t         first;
        uint32_t         usable_end;
        uint32_t         sequence;
        uint32_t         start;
        uint32_t         head;
        uint32_t         compat;
        uint32_t         incompat;
        uint32_t         checksum_seed;
        uint32_t         transaction_sequence;
        jbd2_record_t   *records;
        jbd2_record_t   *tail;
        uint32_t         record_count;
} extfs_journal_t;

static uint16_t jbd2_get_be16(const void *address)
{
    const uint8_t *p = address;
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t jbd2_get_be32(const void *address)
{
    const uint8_t *p = address;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void jbd2_put_be16(void *address, uint16_t value)
{
    uint8_t *p = address;
    p[0]       = (uint8_t)(value >> 8);
    p[1]       = (uint8_t)value;
}

static void jbd2_put_be32(void *address, uint32_t value)
{
    uint8_t *p = address;
    p[0]       = (uint8_t)(value >> 24);
    p[1]       = (uint8_t)(value >> 16);
    p[2]       = (uint8_t)(value >> 8);
    p[3]       = (uint8_t)value;
}

static uint32_t jbd2_crc32_be(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size--) {
        crc ^= (uint32_t)*bytes++ << 24;
        for (uint32_t bit = 0; bit < 8; bit++) crc = (crc << 1) ^ (0x04C11DB7U & (uint32_t) - (int32_t)(crc >> 31));
    }
    return crc;
}

static uint32_t jbd2_next_block(const extfs_journal_t *journal, uint32_t block)
{
    block++;
    return block >= journal->usable_end ? journal->first : block;
}

static int jbd2_logical_io(extfs_journal_t *journal, uint32_t logical, void *data, int write)
{
    uint32_t physical;
    if (!journal || !data || (journal->max_length && logical >= journal->max_length)) return -EINVAL;
    physical = extfs_map_block(journal->inode, logical, 0);
    if (!physical || physical >= journal->sb->blocks_count) return -EIO;
    uint64_t offset = (uint64_t)physical * journal->block_size;
    return write ? blockdev_write_bytes(&journal->sb->device, offset, data, journal->block_size) :
                   blockdev_read_bytes(&journal->sb->device, offset, data, journal->block_size);
}

static int jbd2_read(extfs_journal_t *journal, uint32_t logical, void *data)
{
    return jbd2_logical_io(journal, logical, data, 0);
}

static int jbd2_write(extfs_journal_t *journal, uint32_t logical, void *data)
{
    return jbd2_logical_io(journal, logical, data, 1);
}

static int jbd2_write_super(extfs_journal_t *journal, uint32_t start, uint32_t sequence)
{
    jbd2_superblock_t *super = (jbd2_superblock_t *)journal->super_buffer;
    jbd2_put_be32(&super->start, start);
    jbd2_put_be32(&super->sequence, sequence);
    if (journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
        jbd2_put_be32(&super->checksum, 0);
        jbd2_put_be32(&super->checksum, crc32c_update(~0U, super, sizeof(*super)));
    }
    int status = jbd2_write(journal, 0, journal->super_buffer);
    if (status == EOK) {
        journal->start    = start;
        journal->sequence = sequence;
    }
    return status;
}

static void jbd2_free_records(extfs_journal_t *journal)
{
    jbd2_record_t *record = journal->records;
    while (record) {
        jbd2_record_t *next = record->next;
        free(record->data);
        free(record);
        record = next;
    }
    journal->records      = 0;
    journal->tail         = 0;
    journal->record_count = 0;
}

static size_t jbd2_tag_size(const extfs_journal_t *journal)
{
    if (journal->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) return 16;
    return (journal->incompat & JBD2_FEATURE_INCOMPAT_64BIT) ? 12 : 8;
}

static int jbd2_parse_tag(extfs_journal_t *journal, const uint8_t *tag, uint64_t *home, uint32_t *flags, uint32_t *checksum)
{
    *home = jbd2_get_be32(tag);
    if (journal->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) {
        *flags = jbd2_get_be32(tag + 4);
        *home |= (uint64_t)jbd2_get_be32(tag + 8) << 32;
        *checksum = jbd2_get_be32(tag + 12);
    } else {
        *checksum = jbd2_get_be16(tag + 4);
        *flags    = jbd2_get_be16(tag + 6);
        if (journal->incompat & JBD2_FEATURE_INCOMPAT_64BIT) *home |= (uint64_t)jbd2_get_be32(tag + 8) << 32;
    }
    return *home < journal->sb->blocks_count ? EOK : -EIO;
}

static uint32_t jbd2_data_checksum(extfs_journal_t *journal, uint32_t sequence, const void *data)
{
    uint8_t be_sequence[4];
    jbd2_put_be32(be_sequence, sequence);
    uint32_t checksum = crc32c_update(journal->checksum_seed, be_sequence, sizeof(be_sequence));
    return crc32c_update(checksum, data, journal->block_size);
}

static int jbd2_verify_commit(extfs_journal_t *journal, uint8_t *block, uint32_t sequence)
{
    if (jbd2_get_be32(block) != JBD2_MAGIC_NUMBER || jbd2_get_be32(block + 4) != JBD2_COMMIT_BLOCK || jbd2_get_be32(block + 8) != sequence)
        return -EIO;
    if (!(journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))) return EOK;
    if (block[12] != JBD2_CRC32C_CHKSUM || block[13] != 4) return -EIO;
    uint32_t stored = jbd2_get_be32(block + 16);
    jbd2_put_be32(block + 16, 0);
    uint32_t calculated = crc32c_update(journal->checksum_seed, block, journal->block_size);
    jbd2_put_be32(block + 16, stored);
    if (stored != calculated) {
        plogk("extfs: Drive %u: journal commit block %u checksum mismatch.\n", journal->sb->device.drive, sequence);
        return -EIO;
    }
    return EOK;
}

static int jbd2_verify_descriptor(extfs_journal_t *journal, uint8_t *block)
{
    if (!(journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))) return EOK;
    uint8_t *tail   = block + journal->block_size - sizeof(uint32_t);
    uint32_t stored = jbd2_get_be32(tail);
    jbd2_put_be32(tail, 0);
    uint32_t calculated = crc32c_update(journal->checksum_seed, block, journal->block_size);
    jbd2_put_be32(tail, stored);
    if (stored != calculated) {
        plogk("extfs: Drive %u: journal descriptor block checksum mismatch (sequence %u)\n", journal->sb->device.drive,
              jbd2_get_be32(block + 8));
        return -EIO;
    }
    return EOK;
}

static void jbd2_set_descriptor_checksum(extfs_journal_t *journal, uint8_t *block)
{
    if (!(journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))) return;
    uint8_t *tail = block + journal->block_size - sizeof(uint32_t);
    jbd2_put_be32(tail, 0);
    jbd2_put_be32(tail, crc32c_update(journal->checksum_seed, block, journal->block_size));
}

/* Walk one committed transaction. Descriptor data blocks are checked while walking. */
static int jbd2_walk_transaction(extfs_journal_t *journal, uint32_t start, uint32_t sequence, uint32_t *next, jbd2_revoke_t **revokes,
                                 int replay)
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
        status = jbd2_read(journal, cursor, block);
        if (status != EOK) break;
        uint32_t magic = jbd2_get_be32(block);
        uint32_t type  = jbd2_get_be32(block + 4);
        uint32_t seq   = jbd2_get_be32(block + 8);
        if (magic != JBD2_MAGIC_NUMBER || seq != sequence) {
            status = -EIO;
            break;
        }
        if (type == JBD2_DESCRIPTOR_BLOCK) {
            status = jbd2_verify_descriptor(journal, block);
            if (status != EOK) break;
            if (journal->compat & JBD2_FEATURE_COMPAT_CHECKSUM) transaction_crc = jbd2_crc32_be(transaction_crc, block, journal->block_size);
            size_t tag_size = jbd2_tag_size(journal);
            size_t offset   = sizeof(jbd2_header_t);
            size_t limit    = journal->block_size;
            if (journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)) limit -= 4;
            int last = 0;
            while (!last) {
                uint64_t home;
                uint32_t flags, checksum;
                if (offset + tag_size > limit) {
                    status = -EIO;
                    break;
                }
                status = jbd2_parse_tag(journal, block + offset, &home, &flags, &checksum);
                if (status != EOK || (flags & JBD2_FLAG_DELETED)) {
                    if (status == EOK) status = -EIO;
                    break;
                }
                offset += tag_size;
                if (!(flags & JBD2_FLAG_SAME_UUID)) {
                    if (offset + 16 > limit || memcmp(block + offset, journal->sb->es->s_uuid, 16) != 0) {
                        status = -EIO;
                        break;
                    }
                    offset += 16;
                }
                cursor = jbd2_next_block(journal, cursor);
                status = jbd2_read(journal, cursor, data);
                if (status != EOK) break;
                if (journal->compat & JBD2_FEATURE_COMPAT_CHECKSUM) transaction_crc = jbd2_crc32_be(transaction_crc, data, journal->block_size);
                if (journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
                    uint32_t actual = jbd2_data_checksum(journal, sequence, data);
                    if ((journal->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) ? actual != checksum : (uint16_t)actual != (uint16_t)checksum) {
                        plogk("extfs: Drive %u: journal data block checksum mismatch (sequence %u, home %llu)\n", journal->sb->device.drive,
                              sequence, (unsigned long long)home);
                        status = -EIO;
                        break;
                    }
                }
                if (flags & JBD2_FLAG_ESCAPE) jbd2_put_be32(data, JBD2_MAGIC_NUMBER);
                if (replay) {
                    int revoked = 0;
                    for (jbd2_revoke_t *item = *revokes; item; item = item->next)
                        if (item->block == home && item->sequence >= sequence) {
                            revoked = 1;
                            break;
                        }
                    if (!revoked)
                        status = blockdev_write_bytes(&journal->sb->device, home * (uint64_t)journal->block_size, data, journal->block_size);
                }
                last = !!(flags & JBD2_FLAG_LAST_TAG);
            }
        } else if (type == JBD2_REVOKE_BLOCK) {
            uint32_t bytes      = jbd2_get_be32(block + 12);
            size_t   entry_size = (journal->incompat & JBD2_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
            if (bytes < 16 || bytes > journal->block_size || (bytes - 16) % entry_size) {
                status = -EIO;
                break;
            }
            if (revokes && !replay) {
                for (size_t offset = 16; offset < bytes; offset += entry_size) {
                    uint64_t value = jbd2_get_be32(block + offset);
                    if (entry_size == 8) value = value << 32 | jbd2_get_be32(block + offset + 4);
                    jbd2_revoke_t *item;
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
        } else if (type == JBD2_COMMIT_BLOCK) {
            if (journal->compat & JBD2_FEATURE_COMPAT_CHECKSUM) {
                status = block[12] == JBD2_CRC32_CHKSUM && block[13] == 4 && jbd2_get_be32(block + 16) == transaction_crc ? EOK : -EIO;
            } else {
                status = jbd2_verify_commit(journal, block, sequence);
            }
            if (status == EOK) *next = jbd2_next_block(journal, cursor);
            break;
        } else {
            status = -EIO;
            break;
        }
        cursor = jbd2_next_block(journal, cursor);
    }
    free(data);
    free(block);
    return status;
}

static int jbd2_v1_transaction_checksum(extfs_journal_t *journal, uint32_t start, uint32_t end, uint32_t sequence, uint32_t *result)
{
    uint8_t *block  = malloc(journal->block_size);
    uint8_t *data   = malloc(journal->block_size);
    uint32_t cursor = start, crc = ~0U;
    int      status = block && data ? EOK : -ENOMEM;
    while (status == EOK && cursor != end) {
        status = jbd2_read(journal, cursor, block);
        if (status != EOK || jbd2_get_be32(block) != JBD2_MAGIC_NUMBER || jbd2_get_be32(block + 4) != JBD2_DESCRIPTOR_BLOCK
            || jbd2_get_be32(block + 8) != sequence) {
            if (status == EOK) status = -EIO;
            break;
        }
        crc           = jbd2_crc32_be(crc, block, journal->block_size);
        size_t offset = sizeof(jbd2_header_t), tag_size = jbd2_tag_size(journal);
        int    last = 0;
        while (!last && status == EOK) {
            if (offset + tag_size > journal->block_size) {
                status = -EIO;
                break;
            }
            uint32_t flags
                = (journal->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) ? jbd2_get_be32(block + offset + 4) : jbd2_get_be16(block + offset + 6);
            offset += tag_size + ((flags & JBD2_FLAG_SAME_UUID) ? 0 : 16);
            cursor = jbd2_next_block(journal, cursor);
            if (cursor == end || (status = jbd2_read(journal, cursor, data)) != EOK) { // NOLINT(bugprone-assignment-in-if-condition)
                status = -EIO;
                break;
            }
            crc  = jbd2_crc32_be(crc, data, journal->block_size);
            last = !!(flags & JBD2_FLAG_LAST_TAG);
        }
        cursor = jbd2_next_block(journal, cursor);
    }
    free(data);
    free(block);
    if (status == EOK) *result = crc;
    return status;
}

static void jbd2_free_revokes(jbd2_revoke_t *revoke)
{
    while (revoke) {
        jbd2_revoke_t *next = revoke->next;
        free(revoke);
        revoke = next;
    }
}

static int jbd2_recover(void *context)
{
    extfs_journal_t *journal = context;
    jbd2_revoke_t   *revokes = 0;
    uint32_t         cursor, sequence, end_sequence;
    int              status = EOK;
    if (!journal->start) return EOK;

    /* Pass one: find the first incomplete transaction without changing home blocks. */
    cursor   = journal->start;
    sequence = journal->sequence;
    for (;;) {
        uint32_t next;
        status = jbd2_walk_transaction(journal, cursor, sequence, &next, &revokes, 0);
        if (status != EOK) { break; }
        cursor = next;
        sequence++;
        if (cursor == journal->start) return -EIO;
    }
    end_sequence = sequence;

    /* Pass two: collect revokes from every complete transaction. */
    jbd2_free_revokes(revokes);
    revokes = 0;
    cursor  = journal->start;
    for (sequence = journal->sequence; sequence != end_sequence; sequence++) {
        uint32_t next;
        status = jbd2_walk_transaction(journal, cursor, sequence, &next, &revokes, 0);
        if (status != EOK) goto out;
        cursor = next;
    }

    /* Pass three: replay only committed, non-revoked metadata. */
    cursor = journal->start;
    for (sequence = journal->sequence; sequence != end_sequence; sequence++) {
        uint32_t next;
        status = jbd2_walk_transaction(journal, cursor, sequence, &next, &revokes, 1);
        if (status != EOK) goto out;
        cursor = next;
    }
    status = blockdev_flush(&journal->sb->device);
    if (status == EOK) status = jbd2_write_super(journal, 0, end_sequence);
    if (status == EOK) status = blockdev_flush(&journal->sb->device);
    journal->head = cursor;
out:
    jbd2_free_revokes(revokes);
    return status;
}

static int jbd2_begin(void *context, uint32_t transaction_id, uint32_t buffers)
{
    extfs_journal_t *journal = context;
    (void)transaction_id;
    jbd2_free_records(journal);
    journal->transaction_sequence = journal->sequence;
    size_t descriptor_space       = journal->block_size - sizeof(jbd2_header_t);
    if (journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)) descriptor_space -= 4;
    uint32_t tags = descriptor_space > 16 ? (uint32_t)((descriptor_space - 16) / jbd2_tag_size(journal)) : 0;
    if (!tags) return -ENOSPC;
    uint64_t descriptors = (buffers + tags - 1) / tags;
    uint64_t required    = descriptors + buffers + 1;
    return required < journal->usable_end - journal->first ? EOK : -ENOSPC;
}

static int jbd2_log_block(void *context, uint32_t transaction_id, uint64_t home_block, const void *data, uint32_t flags)
{
    extfs_journal_t *journal = context;
    jbd2_record_t   *record;
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

static int jbd2_commit(void *context, uint32_t transaction_id)
{
    extfs_journal_t *journal    = context;
    jbd2_record_t   *record     = journal->records;
    uint8_t         *descriptor = malloc(journal->block_size);
    uint8_t         *copy       = malloc(journal->block_size);
    uint32_t         cursor     = journal->head;
    uint32_t         sequence   = journal->transaction_sequence;
    int              status     = descriptor && copy ? EOK : -ENOMEM;
    (void)transaction_id;
    if (status != EOK) goto out;
    if (!record) goto out;

    status = jbd2_write_super(journal, cursor, sequence);
    if (status == EOK) status = blockdev_flush(&journal->sb->device);
    while (record && status == EOK) {
        memset(descriptor, 0, journal->block_size);
        jbd2_put_be32(descriptor, JBD2_MAGIC_NUMBER);
        jbd2_put_be32(descriptor + 4, JBD2_DESCRIPTOR_BLOCK);
        jbd2_put_be32(descriptor + 8, sequence);
        uint32_t descriptor_block = cursor;
        cursor                    = jbd2_next_block(journal, cursor);
        size_t offset             = sizeof(jbd2_header_t);
        size_t limit              = journal->block_size;
        if (journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)) limit -= 4;
        int first = 1;
        while (record) {
            size_t required = jbd2_tag_size(journal) + (first ? 16 : 0);
            if (offset + required > limit) break;
            uint32_t flags = first ? 0 : JBD2_FLAG_SAME_UUID;
            if (!record->next || offset + required + jbd2_tag_size(journal) > limit) flags |= JBD2_FLAG_LAST_TAG;
            memcpy(copy, record->data, journal->block_size);
            if (jbd2_get_be32(copy) == JBD2_MAGIC_NUMBER) {
                jbd2_put_be32(copy, 0);
                flags |= JBD2_FLAG_ESCAPE;
            }
            uint32_t checksum = jbd2_data_checksum(journal, sequence, copy);
            jbd2_put_be32(descriptor + offset, (uint32_t)record->home);
            if (journal->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) {
                jbd2_put_be32(descriptor + offset + 4, flags);
                jbd2_put_be32(descriptor + offset + 8, (uint32_t)(record->home >> 32));
                jbd2_put_be32(descriptor + offset + 12, checksum);
            } else {
                jbd2_put_be16(descriptor + offset + 4, (uint16_t)checksum);
                jbd2_put_be16(descriptor + offset + 6, (uint16_t)flags);
                if (journal->incompat & JBD2_FEATURE_INCOMPAT_64BIT) jbd2_put_be32(descriptor + offset + 8, (uint32_t)(record->home >> 32));
            }
            offset += jbd2_tag_size(journal);
            if (first) {
                memcpy(descriptor + offset, journal->sb->es->s_uuid, 16);
                offset += 16;
            }
            status = jbd2_write(journal, cursor, copy);
            if (status != EOK) break;
            cursor = jbd2_next_block(journal, cursor);
            record = record->next;
            first  = 0;
            if (flags & JBD2_FLAG_LAST_TAG) break;
        }
        if (status == EOK) {
            jbd2_set_descriptor_checksum(journal, descriptor);
            status = jbd2_write(journal, descriptor_block, descriptor);
        }
    }
    if (status == EOK) status = blockdev_flush(&journal->sb->device);
    if (status == EOK) {
        uint32_t transaction_crc = 0;
        if (journal->compat & JBD2_FEATURE_COMPAT_CHECKSUM)
            status = jbd2_v1_transaction_checksum(journal, journal->head, cursor, sequence, &transaction_crc);
        if (status != EOK) goto out;
        memset(copy, 0, journal->block_size);
        jbd2_put_be32(copy, JBD2_MAGIC_NUMBER);
        jbd2_put_be32(copy + 4, JBD2_COMMIT_BLOCK);
        jbd2_put_be32(copy + 8, sequence);
        if (journal->compat & JBD2_FEATURE_COMPAT_CHECKSUM) {
            copy[12] = JBD2_CRC32_CHKSUM;
            copy[13] = 4;
            jbd2_put_be32(copy + 16, transaction_crc);
        } else if (journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
            copy[12] = JBD2_CRC32C_CHKSUM;
            copy[13] = 4;
            jbd2_put_be32(copy + 16, crc32c_update(journal->checksum_seed, copy, journal->block_size));
        }
        status = jbd2_write(journal, cursor, copy);
        cursor = jbd2_next_block(journal, cursor);
    }
    if (status == EOK) status = blockdev_flush(&journal->sb->device);
    if (status == EOK) journal->head = cursor;
out:
    free(copy);
    free(descriptor);
    return status;
}

static int jbd2_checkpoint(void *context, uint32_t transaction_id)
{
    extfs_journal_t *journal = context;
    (void)transaction_id;
    int status = jbd2_write_super(journal, 0, journal->transaction_sequence + 1);
    if (status == EOK) {
        journal->sequence = journal->transaction_sequence + 1;
        jbd2_free_records(journal);
    }
    return status;
}

static void jbd2_abort(void *context, uint32_t transaction_id, int error)
{
    extfs_journal_t   *journal = context;
    jbd2_superblock_t *super   = (jbd2_superblock_t *)journal->super_buffer;
    (void)transaction_id;
    jbd2_put_be32(&super->error, (uint32_t)(error < 0 ? -error : error));
    (void)jbd2_write_super(journal, journal->start, journal->sequence);
    (void)blockdev_flush(&journal->sb->device);
    jbd2_free_records(journal);
}

static const fs_txn_backend_ops_t jbd2_ops = {
    .recover    = jbd2_recover,
    .begin      = jbd2_begin,
    .log_block  = jbd2_log_block,
    .commit     = jbd2_commit,
    .checkpoint = jbd2_checkpoint,
    .abort      = jbd2_abort,
};

int extfs_jbd2_open(struct extfs_sb_info *sb, extfs_journal_t **out)
{
    extfs_journal_t   *journal;
    jbd2_superblock_t *super;
    ext2_inode_t       inode;
    int                status;
    if (!sb || !out || !sb->es->s_journal_inum) return -EINVAL;
    *out    = 0;
    journal = calloc(1, sizeof(*journal));
    if (!journal) return -ENOMEM;
    journal->sb           = sb;
    journal->inode        = extfs_alloc_handle(sb, sb->es->s_journal_inum);
    journal->super_buffer = malloc(sb->block_size);
    if (!journal->inode || !journal->super_buffer) {
        extfs_jbd2_close(journal);
        return -ENOMEM;
    }
    status = extfs_read_inode_raw(sb, sb->es->s_journal_inum, &inode);
    if (status != EOK || (inode.i_mode & 0xF000) != EXT2_S_IFREG) {
        extfs_jbd2_close(journal);
        return -EINVAL;
    }
    journal->block_size = sb->block_size;
    status              = jbd2_read(journal, 0, journal->super_buffer);
    if (status != EOK) {
        extfs_jbd2_close(journal);
        return status;
    }
    super                = (jbd2_superblock_t *)journal->super_buffer;
    uint32_t type        = jbd2_get_be32(&super->header.block_type);
    journal->max_length  = jbd2_get_be32(&super->max_length);
    journal->first       = jbd2_get_be32(&super->first);
    journal->sequence    = jbd2_get_be32(&super->sequence);
    journal->start       = jbd2_get_be32(&super->start);
    journal->compat      = jbd2_get_be32(&super->feature_compat);
    journal->incompat    = jbd2_get_be32(&super->feature_incompat);
    uint32_t fast_blocks = jbd2_get_be32(&super->fast_commit_blocks);
    uint64_t inode_size  = inode.i_size | ((uint64_t)inode.i_dir_acl << 32);
    if (jbd2_get_be32(&super->header.magic) != JBD2_MAGIC_NUMBER || (type != JBD2_SUPERBLOCK_V1 && type != JBD2_SUPERBLOCK_V2)
        || jbd2_get_be32(&super->block_size) != sb->block_size || journal->max_length > inode_size / sb->block_size || journal->first == 0
        || journal->first >= journal->max_length || journal->start >= journal->max_length || journal->compat & ~JBD2_KNOWN_COMPAT
        || journal->incompat & ~JBD2_KNOWN_INCOMPAT || fast_blocks >= journal->max_length - journal->first
        || ((journal->incompat & JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT)
            && !(journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)))
        || ((journal->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) && (journal->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3))) {
        extfs_jbd2_close(journal);
        return -EOPNOTSUPP;
    }
    if ((journal->compat & JBD2_FEATURE_COMPAT_CHECKSUM)
        && (journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))) {
        extfs_jbd2_close(journal);
        return -EINVAL;
    }
    if (memcmp(super->uuid, sb->es->s_uuid, 16) != 0) {
        extfs_jbd2_close(journal);
        return -EINVAL;
    }
    journal->checksum_seed = crc32c_update(~0U, super->uuid, 16);
    if (journal->incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
        if (super->checksum_type != JBD2_CRC32C_CHKSUM) {
            extfs_jbd2_close(journal);
            return -EOPNOTSUPP;
        }
        uint32_t stored = jbd2_get_be32(&super->checksum);
        jbd2_put_be32(&super->checksum, 0);
        uint32_t calculated = crc32c_update(~0U, super, sizeof(*super));
        jbd2_put_be32(&super->checksum, stored);
        if (stored != calculated) {
            extfs_jbd2_close(journal);
            return -EIO;
        }
    }
    journal->usable_end = journal->max_length - fast_blocks;
    journal->head       = jbd2_get_be32(&super->head);
    if (journal->head < journal->first || journal->head >= journal->usable_end) journal->head = journal->first;
    *out = journal;
    return EOK;
}

void extfs_jbd2_close(extfs_journal_t *journal)
{
    if (!journal) return;
    jbd2_free_records(journal);
    free(journal->super_buffer);
    free(journal->inode);
    free(journal);
}

const fs_txn_backend_ops_t *extfs_jbd2_backend_ops(void)
{
    return &jbd2_ops;
}
