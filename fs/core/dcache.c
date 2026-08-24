/*
 * Concurrent pathname-component cache for the VFS namespace.
 *
 * Positive records are intrusive, so adding/removing a namespace node never
 * allocates.  Negative records carry the directory generation at which the
 * miss was observed.  Any namespace mutation advances that generation,
 * making old misses unusable before they are physically reclaimed.
 */

#include <fs/core/dcache.h>
#include <fs/core/vfs.h>
#include <libs/std/stdbool.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

#define VFS_DCACHE_BUCKETS      1024U
#define VFS_DCACHE_MAX_NEGATIVE 4096U

typedef struct vfs_negative_dentry {
        struct vfs_negative_dentry *next;
        vfs_node_t                  parent;
        uint64_t                    hash;
        uint64_t                    generation;
        uint64_t                    age;
        char                        name[];
} vfs_negative_dentry_t;

typedef struct vfs_dcache_bucket {
        spinlock_t             lock;
        vfs_node_t             positive;
        vfs_negative_dentry_t *negative;
} vfs_dcache_bucket_t;

static vfs_dcache_bucket_t dcache_buckets[VFS_DCACHE_BUCKETS];
static vfs_dcache_stats_t  dcache_stats;
static uint64_t            dcache_clock;

#define DCACHE_INC(field) ((void)__atomic_add_fetch(&dcache_stats.field, 1, __ATOMIC_RELAXED))
#define DCACHE_DEC(field) ((void)__atomic_sub_fetch(&dcache_stats.field, 1, __ATOMIC_RELAXED))
#define DCACHE_TICK()     __atomic_add_fetch(&dcache_clock, 1, __ATOMIC_RELAXED)

static uint64_t dcache_hash_name(vfs_node_t parent, const char *name)
{
    uint64_t hash = 1469598103934665603ULL ^ (uint64_t)(uintptr_t)parent;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 32;
    return hash;
}

static vfs_dcache_bucket_t *dcache_bucket(uint64_t hash)
{
    return &dcache_buckets[hash & (VFS_DCACHE_BUCKETS - 1)];
}

void vfs_dcache_init(void)
{
    memset(dcache_buckets, 0, sizeof(dcache_buckets));
    memset(&dcache_stats, 0, sizeof(dcache_stats));
    dcache_clock = 0;
}

enum vfs_dcache_result vfs_dcache_lookup(vfs_node_t parent, const char *name, vfs_node_t *node)
{
    if (node) *node = NULL;
    if (!parent || !name || !name[0]) return VFS_DCACHE_MISS;

    uint64_t             hash   = dcache_hash_name(parent, name);
    vfs_dcache_bucket_t *bucket = dcache_bucket(hash);
    spin_lock(&bucket->lock);
    for (vfs_node_t item = bucket->positive; item; item = item->dcache_next) {
        if (item->dcache_hash != hash || item->parent != parent || !item->name || strcmp(item->name, name)) continue;
        if (item->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_INITIALIZING) || (item->type & file_delete)) break;
        DCACHE_INC(positive_hits);
        if (node) *node = item;
        spin_unlock(&bucket->lock);
        return VFS_DCACHE_POSITIVE;
    }
    for (vfs_negative_dentry_t *item = bucket->negative; item; item = item->next) {
        if (item->hash != hash || item->parent != parent || strcmp(item->name, name)) continue;
        if (item->generation == __atomic_load_n(&parent->dcache_generation, __ATOMIC_ACQUIRE)) {
            item->age = DCACHE_TICK();
            DCACHE_INC(negative_hits);
            spin_unlock(&bucket->lock);
            return VFS_DCACHE_NEGATIVE;
        }
        break;
    }
    DCACHE_INC(misses);
    spin_unlock(&bucket->lock);
    return VFS_DCACHE_MISS;
}

void vfs_dcache_add(vfs_node_t node)
{
    if (!node || !node->parent || !node->name || !node->name[0] || node->dcache_hashed) return;
    uint64_t             hash   = dcache_hash_name(node->parent, node->name);
    vfs_dcache_bucket_t *bucket = dcache_bucket(hash);
    spin_lock(&bucket->lock);
    if (!node->dcache_hashed) {
        node->dcache_hash   = hash;
        node->dcache_next   = bucket->positive;
        node->dcache_hashed = true;
        bucket->positive    = node;
        DCACHE_INC(positive_entries);
        DCACHE_INC(insertions);
    }
    spin_unlock(&bucket->lock);
    vfs_dcache_invalidate(node->parent, node->name);
}

void vfs_dcache_remove(vfs_node_t node)
{
    if (!node || !node->dcache_hashed) return;
    vfs_dcache_bucket_t *bucket = dcache_bucket(node->dcache_hash);
    spin_lock(&bucket->lock);
    vfs_node_t *link = &bucket->positive;
    while (*link && *link != node) link = &(*link)->dcache_next;
    if (*link) {
        *link = node->dcache_next;
        DCACHE_DEC(positive_entries);
    }
    node->dcache_next   = NULL;
    node->dcache_hashed = false;
    spin_unlock(&bucket->lock);
}

void vfs_dcache_add_negative(vfs_node_t parent, const char *name)
{
    if (!parent || !name || !name[0]) return;
    if (parent->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED)) return;
    size_t length = strlen(name);
    if (length > VFS_NAME_MAX) return;

    uint64_t               hash  = dcache_hash_name(parent, name);
    vfs_negative_dentry_t *fresh = malloc(sizeof(*fresh) + length + 1);
    if (!fresh) return;
    fresh->parent     = parent;
    fresh->hash       = hash;
    fresh->generation = __atomic_load_n(&parent->dcache_generation, __ATOMIC_ACQUIRE);
    fresh->age        = DCACHE_TICK();
    memcpy(fresh->name, name, length + 1);

    vfs_dcache_bucket_t *bucket = dcache_bucket(hash);
    spin_lock(&bucket->lock);
    for (vfs_negative_dentry_t *item = bucket->negative; item; item = item->next) {
        if (item->hash == hash && item->parent == parent && !strcmp(item->name, name)) {
            item->generation = __atomic_load_n(&parent->dcache_generation, __ATOMIC_ACQUIRE);
            item->age        = fresh->age;
            spin_unlock(&bucket->lock);
            free(fresh);
            return;
        }
    }
    fresh->next      = bucket->negative;
    bucket->negative = fresh;
    DCACHE_INC(negative_entries);
    DCACHE_INC(insertions);
    spin_unlock(&bucket->lock);

    if (__atomic_load_n(&dcache_stats.negative_entries, __ATOMIC_RELAXED) > VFS_DCACHE_MAX_NEGATIVE) (void)vfs_dcache_reclaim(64);
}

void vfs_dcache_invalidate(vfs_node_t parent, const char *name)
{
    if (!parent || !name || !name[0]) return;
    uint64_t               hash   = dcache_hash_name(parent, name);
    vfs_dcache_bucket_t   *bucket = dcache_bucket(hash);
    vfs_negative_dentry_t *dead   = NULL;

    spin_lock(&bucket->lock);
    vfs_negative_dentry_t **link = &bucket->negative;
    while (*link) {
        vfs_negative_dentry_t *item = *link;
        if (item->hash == hash && item->parent == parent && !strcmp(item->name, name)) {
            *link      = item->next;
            item->next = dead;
            dead       = item;
            DCACHE_DEC(negative_entries);
            DCACHE_INC(invalidations);
            continue;
        }
        link = &item->next;
    }
    spin_unlock(&bucket->lock);
    while (dead) {
        vfs_negative_dentry_t *next = dead->next;
        free(dead);
        dead = next;
    }
}

void vfs_dcache_invalidate_parent(vfs_node_t parent)
{
    if (!parent) return;
    if (!__atomic_add_fetch(&parent->dcache_generation, 1, __ATOMIC_ACQ_REL)) __atomic_store_n(&parent->dcache_generation, 1, __ATOMIC_RELEASE);

    for (size_t index = 0; index < VFS_DCACHE_BUCKETS; index++) {
        vfs_dcache_bucket_t   *bucket = &dcache_buckets[index];
        vfs_negative_dentry_t *dead   = NULL;
        spin_lock(&bucket->lock);
        vfs_negative_dentry_t **link = &bucket->negative;
        while (*link) {
            vfs_negative_dentry_t *item = *link;
            if (item->parent == parent) {
                *link      = item->next;
                item->next = dead;
                dead       = item;
                DCACHE_DEC(negative_entries);
                DCACHE_INC(invalidations);
                continue;
            }
            link = &item->next;
        }
        spin_unlock(&bucket->lock);
        while (dead) {
            vfs_negative_dentry_t *next = dead->next;
            free(dead);
            dead = next;
        }
    }
}

size_t vfs_dcache_reclaim(size_t target)
{
    size_t reclaimed = 0;
    if (!target) return 0;

    /* Stale generations are always preferred; otherwise evict bucket tails. */
    for (size_t index = 0; index < VFS_DCACHE_BUCKETS && reclaimed < target; index++) {
        vfs_dcache_bucket_t   *bucket = &dcache_buckets[index];
        vfs_negative_dentry_t *dead   = NULL;
        spin_lock(&bucket->lock);
        vfs_negative_dentry_t **link = &bucket->negative;
        while (*link && reclaimed < target) {
            vfs_negative_dentry_t *item = *link;
            if (item->generation != __atomic_load_n(&item->parent->dcache_generation, __ATOMIC_ACQUIRE) || !item->next) {
                *link      = item->next;
                item->next = dead;
                dead       = item;
                reclaimed++;
                DCACHE_DEC(negative_entries);
                DCACHE_INC(evictions);
                continue;
            }
            link = &item->next;
        }
        spin_unlock(&bucket->lock);
        while (dead) {
            vfs_negative_dentry_t *next = dead->next;
            free(dead);
            dead = next;
        }
    }
    return reclaimed;
}

void vfs_dcache_get_stats(vfs_dcache_stats_t *stats)
{
    if (!stats) return;
    stats->positive_entries = __atomic_load_n(&dcache_stats.positive_entries, __ATOMIC_RELAXED);
    stats->negative_entries = __atomic_load_n(&dcache_stats.negative_entries, __ATOMIC_RELAXED);
    stats->positive_hits    = __atomic_load_n(&dcache_stats.positive_hits, __ATOMIC_RELAXED);
    stats->negative_hits    = __atomic_load_n(&dcache_stats.negative_hits, __ATOMIC_RELAXED);
    stats->misses           = __atomic_load_n(&dcache_stats.misses, __ATOMIC_RELAXED);
    stats->insertions       = __atomic_load_n(&dcache_stats.insertions, __ATOMIC_RELAXED);
    stats->invalidations    = __atomic_load_n(&dcache_stats.invalidations, __ATOMIC_RELAXED);
    stats->evictions        = __atomic_load_n(&dcache_stats.evictions, __ATOMIC_RELAXED);
}
