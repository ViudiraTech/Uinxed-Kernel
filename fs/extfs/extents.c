/*
 *
 *      extents.c
 *      ext4 extent tree validation, mapping and transactional rebuilding
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/extfs/extfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <libs/util/crc32c.h>
#include <mem/heap.h>

#define EXT4_EXT_UNWRITTEN 0x8000U
#define EXT4_EXT_MAX_DEPTH 5U

typedef struct ext4_extent_header {
        uint16_t magic;
        uint16_t entries;
        uint16_t max;
        uint16_t depth;
        uint32_t generation;
} __attribute__((packed)) ext4_extent_header_t;

typedef struct ext4_extent {
        uint32_t logical;
        uint16_t length;
        uint16_t start_hi;
        uint32_t start_lo;
} __attribute__((packed)) ext4_extent_t;

typedef struct ext4_extent_index {
        uint32_t logical;
        uint32_t leaf_lo;
        uint16_t leaf_hi;
        uint16_t unused;
} __attribute__((packed)) ext4_extent_index_t;

typedef struct extent_item {
        uint32_t logical;
        uint32_t physical;
        uint16_t length;
        uint8_t  unwritten;
} extent_item_t;

typedef struct extent_vector {
        extent_item_t *items;
        uint32_t       count;
        uint32_t       capacity;
        uint32_t      *metadata;
        uint32_t       metadata_count;
        uint32_t       metadata_capacity;
} extent_vector_t;

typedef struct extent_child {
        uint32_t logical;
        uint32_t block;
} extent_child_t;

static int extent_push(extent_vector_t *vector, extent_item_t item)
{
    if (vector->count == vector->capacity) {
        uint32_t capacity = vector->capacity ? vector->capacity * 2 : 16;
        void    *items    = realloc(vector->items, capacity * sizeof(*vector->items));
        if (!items) return -ENOMEM;
        vector->items    = items;
        vector->capacity = capacity;
    }
    vector->items[vector->count++] = item;
    return EOK;
}

static int extent_push_metadata(extent_vector_t *vector, uint32_t block)
{
    if (vector->metadata_count == vector->metadata_capacity) {
        uint32_t capacity = vector->metadata_capacity ? vector->metadata_capacity * 2 : 16;
        void    *items    = realloc(vector->metadata, capacity * sizeof(*vector->metadata));
        if (!items) return -ENOMEM;
        vector->metadata          = items;
        vector->metadata_capacity = capacity;
    }
    vector->metadata[vector->metadata_count++] = block;
    return EOK;
}

static int extent_header_valid(extfs_sb_info_t *sb, const ext4_extent_header_t *header, uint16_t expected_depth, int root)
{
    uint16_t capacity
        = root ? (uint16_t)((sizeof(((ext2_inode_t *)0)->i_block) - sizeof(*header)) / sizeof(ext4_extent_t)) :
                 (uint16_t)((sb->block_size - sizeof(*header) - ((sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) ? 4 : 0))
                            / sizeof(ext4_extent_t));
    return header->magic == EXT4_EXT_MAGIC && header->depth == expected_depth && header->depth <= EXT4_EXT_MAX_DEPTH
           && header->entries <= header->max && header->max <= capacity;
}

static uint32_t extent_checksum_seed(extfs_handle_t *h)
{
    ext2_inode_t raw;
    if (extfs_read_inode_raw(h->sb, h->inode_no, &raw) != EOK) return 0;
    uint32_t checksum = crc32c_update(h->sb->checksum_seed, &h->inode_no, sizeof(h->inode_no));
    return crc32c_update(checksum, &raw.i_generation, sizeof(raw.i_generation));
}

static size_t extent_tail_offset(extfs_handle_t *h, const uint8_t *block)
{
    const ext4_extent_header_t *header = (const ext4_extent_header_t *)block;
    size_t                      offset = sizeof(*header) + (size_t)header->max * sizeof(ext4_extent_t);
    return offset <= h->sb->block_size - sizeof(uint32_t) ? offset : 0;
}

static int extent_block_checksum_verify(extfs_handle_t *h, uint8_t *block)
{
    if (!(h->sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return 1;
    size_t tail = extent_tail_offset(h, block);
    if (!tail) return 0;
    uint32_t stored;
    memcpy(&stored, block + tail, sizeof(stored));
    return stored == crc32c_update(extent_checksum_seed(h), block, tail);
}

static void extent_block_checksum_set(extfs_handle_t *h, uint8_t *block)
{
    if (!(h->sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return;
    size_t tail = extent_tail_offset(h, block);
    if (!tail) return;
    uint32_t checksum = crc32c_update(extent_checksum_seed(h), block, tail);
    memcpy(block + tail, &checksum, sizeof(checksum));
}

static int extent_collect_node(extfs_handle_t *h, const uint8_t *node, uint16_t depth, int root, extent_vector_t *vector)
{
    ext4_extent_header_t *header = (ext4_extent_header_t *)node;
    if (!extent_header_valid(h->sb, header, depth, root)) {
        plogk("extfs: Drive %u: inode %u invalid extent header (depth %u, entries %u)\n", h->sb->device.drive, h->inode_no, depth,
              header->entries);
        return -EIO;
    }
    if (!depth) {
        ext4_extent_t *entries      = (ext4_extent_t *)(header + 1);
        uint64_t       previous_end = 0;
        for (uint16_t i = 0; i < header->entries; i++) {
            uint32_t length   = entries[i].length & ~EXT4_EXT_UNWRITTEN;
            uint64_t physical = entries[i].start_lo | (uint64_t)entries[i].start_hi << 32;
            if (!length || physical == 0 || physical + length > h->sb->blocks_count || (i && entries[i].logical < previous_end)) {
                plogk("extfs: Drive %u: inode %u invalid extent (logical %u, phys %llu, len %u)\n", h->sb->device.drive, h->inode_no,
                      entries[i].logical, (unsigned long long)physical, length);
                return -EIO;
            }
            extent_item_t item = {
                .logical   = entries[i].logical,
                .physical  = (uint32_t)physical,
                .length    = (uint16_t)length,
                .unwritten = !!(entries[i].length & EXT4_EXT_UNWRITTEN),
            };
            if (physical > UINT32_MAX) return -EOVERFLOW;
            int status = extent_push(vector, item);
            if (status != EOK) return status;
            previous_end = (uint64_t)item.logical + item.length;
        }
        return EOK;
    }

    ext4_extent_index_t *indices = (ext4_extent_index_t *)(header + 1);
    uint8_t             *buffer  = malloc(h->sb->block_size);
    if (!buffer) return -ENOMEM;
    int      status   = EOK;
    uint32_t previous = 0;
    for (uint16_t i = 0; i < header->entries; i++) {
        uint64_t block = indices[i].leaf_lo | (uint64_t)indices[i].leaf_hi << 32;
        if (!block || block > UINT32_MAX || block >= h->sb->blocks_count || (i && indices[i].logical <= previous)) {
            plogk("extfs: Drive %u: inode %u invalid extent index (logical %u, block %llu)\n", h->sb->device.drive, h->inode_no,
                  indices[i].logical, (unsigned long long)block);
            status = -EIO;
            break;
        }
        previous = indices[i].logical;
        status   = extent_push_metadata(vector, (uint32_t)block);
        if (status != EOK || (status = extfs_read_block(h->sb, (uint32_t)block, buffer)) != EOK) // NOLINT(bugprone-assignment-in-if-condition)
            break;
        if (!extent_block_checksum_verify(h, buffer)) {
            plogk("extfs: Drive %u: inode %u extent block %llu checksum mismatch.\n", h->sb->device.drive, h->inode_no,
                  (unsigned long long)block);
            status = -EIO;
            break;
        }
        status = extent_collect_node(h, buffer, depth - 1, 0, vector);
        if (status != EOK) break;
    }
    free(buffer);
    return status;
}

static int extent_collect(extfs_handle_t *h, extent_vector_t *vector)
{
    memset(vector, 0, sizeof(*vector));
    ext4_extent_header_t *root = (ext4_extent_header_t *)h->ei.i_data;
    if (root->magic != EXT4_EXT_MAGIC) {
        plogk("extfs: Drive %u: inode %u extent root magic mismatch (0x%x)\n", h->sb->device.drive, h->inode_no, root->magic);
        return -EIO;
    }
    return extent_collect_node(h, (uint8_t *)h->ei.i_data, root->depth, 1, vector);
}

static void extent_vector_destroy(extent_vector_t *vector)
{
    free(vector->metadata);
    free(vector->items);
    memset(vector, 0, sizeof(*vector));
}

static void extent_sort_and_merge(extent_vector_t *vector)
{
    for (uint32_t i = 1; i < vector->count; i++) {
        extent_item_t item = vector->items[i];
        uint32_t      j    = i;
        while (j && vector->items[j - 1].logical > item.logical) {
            vector->items[j] = vector->items[j - 1];
            j--;
        }
        vector->items[j] = item;
    }
    uint32_t output = 0;
    for (uint32_t i = 0; i < vector->count; i++) {
        extent_item_t item = vector->items[i];
        if (output) {
            extent_item_t *previous = &vector->items[output - 1];
            uint32_t       combined = previous->length + item.length;
            if ((uint64_t)previous->logical + previous->length == item.logical
                && (uint64_t)previous->physical + previous->length == item.physical && previous->unwritten == item.unwritten
                && combined <= 0x7fffU) {
                previous->length = (uint16_t)combined;
                continue;
            }
        }
        vector->items[output++] = item;
    }
    vector->count = output;
}

static void extent_fill_header(ext4_extent_header_t *header, uint16_t entries, uint16_t max, uint16_t depth)
{
    header->magic      = EXT4_EXT_MAGIC;
    header->entries    = entries;
    header->max        = max;
    header->depth      = depth;
    header->generation = 0;
}

static int extent_alloc_node(extfs_handle_t *h, uint32_t *block, uint8_t **buffer, uint32_t **allocated, uint32_t *allocated_count,
                             uint32_t *allocated_capacity)
{
    int status = extfs_alloc_block(h->sb, 0, block);
    if (status != EOK) return status;
    if (*allocated_count == *allocated_capacity) {
        uint32_t capacity = *allocated_capacity ? *allocated_capacity * 2 : 16;
        void    *items    = realloc(*allocated, capacity * sizeof(**allocated));
        if (!items) {
            extfs_free_block(h->sb, *block);
            return -ENOMEM;
        }
        *allocated          = items;
        *allocated_capacity = capacity;
    }
    (*allocated)[(*allocated_count)++] = *block;
    *buffer                            = calloc(1, h->sb->block_size);
    return *buffer ? EOK : -ENOMEM;
}

static int extent_rebuild(extfs_handle_t *h, extent_vector_t *vector)
{
    uint16_t root_capacity = (sizeof(h->ei.i_data) - sizeof(ext4_extent_header_t)) / sizeof(ext4_extent_t);
    uint16_t node_capacity
        = (h->sb->block_size - sizeof(ext4_extent_header_t) - ((h->sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) ? 4 : 0))
          / sizeof(ext4_extent_t);
    uint32_t       *allocated = 0, allocated_count = 0, allocated_capacity = 0;
    extent_child_t *children    = 0;
    uint32_t        child_count = 0;
    int             status      = EOK;
    extent_sort_and_merge(vector);

    if (vector->count <= root_capacity) {
        memset(h->ei.i_data, 0, sizeof(h->ei.i_data));
        ext4_extent_header_t *header = (ext4_extent_header_t *)h->ei.i_data;
        extent_fill_header(header, (uint16_t)vector->count, root_capacity, 0);
        ext4_extent_t *entries = (ext4_extent_t *)(header + 1);
        for (uint32_t i = 0; i < vector->count; i++) {
            entries[i].logical  = vector->items[i].logical;
            entries[i].length   = vector->items[i].length | (vector->items[i].unwritten ? EXT4_EXT_UNWRITTEN : 0);
            entries[i].start_lo = vector->items[i].physical;
        }
        goto replace;
    }

    child_count = (vector->count + node_capacity - 1) / node_capacity;
    children    = calloc(child_count, sizeof(*children));
    if (!children) {
        status = -ENOMEM;
        goto fail;
    }
    for (uint32_t child = 0; child < child_count; child++) {
        uint32_t begin = child * node_capacity;
        uint32_t count = vector->count - begin;
        uint32_t block;
        uint8_t *buffer;
        if (count > node_capacity) count = node_capacity;
        status = extent_alloc_node(h, &block, &buffer, &allocated, &allocated_count, &allocated_capacity);
        if (status != EOK) goto fail;
        ext4_extent_header_t *header = (ext4_extent_header_t *)buffer;
        extent_fill_header(header, (uint16_t)count, node_capacity, 0);
        ext4_extent_t *entries = (ext4_extent_t *)(header + 1);
        for (uint32_t i = 0; i < count; i++) {
            extent_item_t *item = &vector->items[begin + i];
            entries[i].logical  = item->logical;
            entries[i].length   = item->length | (item->unwritten ? EXT4_EXT_UNWRITTEN : 0);
            entries[i].start_lo = item->physical;
        }
        extent_block_checksum_set(h, buffer);
        status = extfs_write_block(h->sb, block, buffer);
        free(buffer);
        if (status != EOK) goto fail;
        children[child].logical = vector->items[begin].logical;
        children[child].block   = block;
    }

    uint16_t depth = 1;
    while (child_count > root_capacity) {
        uint32_t        parent_count = (child_count + node_capacity - 1) / node_capacity;
        extent_child_t *parents      = calloc(parent_count, sizeof(*parents));
        if (!parents) {
            status = -ENOMEM;
            goto fail;
        }
        for (uint32_t parent = 0; parent < parent_count; parent++) {
            uint32_t begin = parent * node_capacity;
            uint32_t count = child_count - begin;
            uint32_t block;
            uint8_t *buffer;
            if (count > node_capacity) count = node_capacity;
            status = extent_alloc_node(h, &block, &buffer, &allocated, &allocated_count, &allocated_capacity);
            if (status != EOK) {
                free(parents);
                goto fail;
            }
            ext4_extent_header_t *header = (ext4_extent_header_t *)buffer;
            extent_fill_header(header, (uint16_t)count, node_capacity, depth);
            ext4_extent_index_t *indices = (ext4_extent_index_t *)(header + 1);
            for (uint32_t i = 0; i < count; i++) {
                indices[i].logical = children[begin + i].logical;
                indices[i].leaf_lo = children[begin + i].block;
            }
            extent_block_checksum_set(h, buffer);
            status = extfs_write_block(h->sb, block, buffer);
            free(buffer);
            if (status != EOK) {
                free(parents);
                goto fail;
            }
            parents[parent].logical = children[begin].logical;
            parents[parent].block   = block;
        }
        free(children);
        children    = parents;
        child_count = parent_count;
        if (++depth > EXT4_EXT_MAX_DEPTH) {
            status = -EFBIG;
            goto fail;
        }
    }

    memset(h->ei.i_data, 0, sizeof(h->ei.i_data));
    ext4_extent_header_t *root = (ext4_extent_header_t *)h->ei.i_data;
    extent_fill_header(root, (uint16_t)child_count, root_capacity, depth);
    ext4_extent_index_t *indices = (ext4_extent_index_t *)(root + 1);
    for (uint32_t i = 0; i < child_count; i++) {
        indices[i].logical = children[i].logical;
        indices[i].leaf_lo = children[i].block;
    }

replace:
    for (uint32_t i = 0; i < vector->metadata_count; i++) extfs_free_block(h->sb, vector->metadata[i]);
    free(children);
    free(allocated);
    return EOK;
fail:
    for (uint32_t i = 0; i < allocated_count; i++) extfs_free_block(h->sb, allocated[i]);
    free(children);
    free(allocated);
    return status;
}

uint32_t extfs_extent_map_block(extfs_handle_t *h, uint32_t logical, int create)
{
    extent_vector_t vector;
    if (!h || extent_collect(h, &vector) != EOK) return 0;
    for (uint32_t i = 0; i < vector.count; i++) {
        extent_item_t item = vector.items[i];
        if (logical < item.logical || logical >= item.logical + item.length) continue;
        if (!item.unwritten) {
            uint32_t result = item.physical + logical - item.logical;
            extent_vector_destroy(&vector);
            return result;
        }
        if (!create) {
            extent_vector_destroy(&vector);
            return 0;
        }
        vector.items[i].length = (uint16_t)(logical - item.logical);
        extent_item_t written  = {.logical = logical, .physical = item.physical + logical - item.logical, .length = 1};
        extent_item_t suffix   = {.logical   = logical + 1,
                                  .physical  = written.physical + 1,
                                  .length    = (uint16_t)(item.logical + item.length - logical - 1),
                                  .unwritten = 1};
        if (!vector.items[i].length) {
            vector.items[i] = written;
        } else if (extent_push(&vector, written) != EOK) {
            extent_vector_destroy(&vector);
            return 0;
        }
        if (suffix.length && extent_push(&vector, suffix) != EOK) {
            extent_vector_destroy(&vector);
            return 0;
        }
        uint32_t result = written.physical;
        if (extent_rebuild(h, &vector) != EOK) result = 0;
        extent_vector_destroy(&vector);
        return result;
    }
    if (!create) {
        extent_vector_destroy(&vector);
        return 0;
    }
    uint32_t physical;
    if (extfs_alloc_block(h->sb, 0, &physical) != EOK) {
        extent_vector_destroy(&vector);
        return 0;
    }
    uint8_t      *zero   = calloc(1, h->sb->block_size);
    extent_item_t item   = {.logical = logical, .physical = physical, .length = 1};
    int           status = zero ? extfs_write_data_block(h->sb, physical, zero) : -ENOMEM;
    free(zero);
    if (status == EOK) status = extent_push(&vector, item);
    if (status == EOK) status = extent_rebuild(h, &vector);
    if (status != EOK) {
        extfs_free_block(h->sb, physical);
        physical = 0;
    }
    extent_vector_destroy(&vector);
    return physical;
}

int extfs_extent_remove_space(extfs_handle_t *h, uint32_t first, uint32_t last)
{
    extent_vector_t old, replacement;
    int             status = extent_collect(h, &old);
    if (status != EOK) return status;
    memset(&replacement, 0, sizeof(replacement));
    replacement.metadata          = old.metadata;
    replacement.metadata_count    = old.metadata_count;
    replacement.metadata_capacity = old.metadata_capacity;
    old.metadata                  = 0;
    for (uint32_t i = 0; i < old.count && status == EOK; i++) {
        extent_item_t item = old.items[i];
        uint64_t      end  = (uint64_t)item.logical + item.length;
        if (end <= first || item.logical >= last) {
            status = extent_push(&replacement, item);
            continue;
        }
        uint32_t cut_first = item.logical > first ? item.logical : first;
        uint32_t cut_last  = end < last ? (uint32_t)end : last;
        if (cut_first > item.logical) {
            extent_item_t prefix = item;
            prefix.length        = (uint16_t)(cut_first - item.logical);
            status               = extent_push(&replacement, prefix);
        }
        for (uint32_t logical = cut_first; status == EOK && logical < cut_last; logical++)
            extfs_free_block(h->sb, item.physical + logical - item.logical);
        if (status == EOK && cut_last < end) {
            extent_item_t suffix = item;
            suffix.logical       = cut_last;
            suffix.physical += cut_last - item.logical;
            suffix.length = (uint16_t)(end - cut_last);
            status        = extent_push(&replacement, suffix);
        }
    }
    if (status == EOK) status = extent_rebuild(h, &replacement);
    extent_vector_destroy(&replacement);
    extent_vector_destroy(&old);
    return status;
}

int extfs_extent_free_all(extfs_handle_t *h)
{
    return extfs_extent_remove_space(h, 0, UINT32_MAX);
}

int extfs_extent_count_blocks(extfs_handle_t *h, uint64_t *blocks)
{
    extent_vector_t vector;
    int             status = extent_collect(h, &vector);
    if (status != EOK) {
        extent_vector_destroy(&vector);
        return status;
    }
    *blocks = vector.metadata_count;
    for (uint32_t i = 0; i < vector.count; i++) *blocks += vector.items[i].length;
    extent_vector_destroy(&vector);
    return EOK;
}
