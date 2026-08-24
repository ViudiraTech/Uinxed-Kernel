/*
 * Mount-scoped inode identity and alias cache.
 *
 * VFS nodes are dentries in this kernel.  This layer supplies the missing
 * inode identity object beneath them, so hard links converge on one cached
 * object and metadata changes are made visible to every live alias.
 */

#include <fs/core/icache.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <libs/std/stdbool.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

#define VFS_ICACHE_BUCKETS    512U
#define VFS_ICACHE_MAX_UNUSED 4096U

typedef struct vfs_inode_key {
        uintptr_t mount;
        uint64_t  mount_id;
        uint64_t  ino;
        uint64_t  dev;
        uint16_t  fsid;
} vfs_inode_key_t;

struct vfs_inode {
        vfs_inode_t    *hash_next;
        vfs_inode_key_t key;
        vfs_node_t      aliases;
        uint32_t        refs;
        uint64_t        age;
        bool            stale;
        uint16_t        type;
        uint64_t        size;
        uint64_t        realsize;
        uint64_t        blksz;
        uint64_t        dev;
        uint64_t        rdev;
        uint32_t        owner;
        uint32_t        group;
        uint32_t        mode;
        uint32_t        permissions;
        uint32_t        nlink;
        int64_t         createtime;
        int64_t         readtime;
        int64_t         writetime;
};

typedef struct vfs_icache_bucket {
        spinlock_t   lock;
        vfs_inode_t *head;
} vfs_icache_bucket_t;

static vfs_icache_bucket_t icache_buckets[VFS_ICACHE_BUCKETS];
static vfs_icache_stats_t  icache_stats;
static uint64_t            icache_clock;

#define ICACHE_INC(field) ((void)__atomic_add_fetch(&icache_stats.field, 1, __ATOMIC_RELAXED))
#define ICACHE_DEC(field) ((void)__atomic_sub_fetch(&icache_stats.field, 1, __ATOMIC_RELAXED))
#define ICACHE_TICK()     __atomic_add_fetch(&icache_clock, 1, __ATOMIC_RELAXED)

static vfs_inode_key_t icache_key(vfs_node_t node)
{
    vfs_node_t      root = node->root ? node->root : node;
    vfs_inode_key_t key  = {
         .mount    = (uintptr_t)root,
         .mount_id = root ? root->mount_id : 0,
         .ino      = node->inode,
         .dev      = node->dev,
         .fsid     = node->fsid,
    };
    return key;
}

static uint64_t icache_hash(const vfs_inode_key_t *key)
{
    uint64_t hash = key->ino * 11400714819323198485ULL;
    hash ^= (uint64_t)key->mount + (key->mount_id << 17);
    hash ^= key->dev * 0x9e3779b97f4a7c15ULL;
    hash ^= (uint64_t)key->fsid << 48;
    hash ^= hash >> 31;
    return hash;
}

static bool icache_key_equal(const vfs_inode_key_t *left, const vfs_inode_key_t *right)
{
    return left->mount == right->mount && left->mount_id == right->mount_id && left->ino == right->ino && left->dev == right->dev && left->fsid == right->fsid;
}

static vfs_icache_bucket_t *icache_bucket(const vfs_inode_key_t *key)
{
    return &icache_buckets[icache_hash(key) & (VFS_ICACHE_BUCKETS - 1)];
}

static void icache_snapshot(vfs_inode_t *inode, vfs_node_t node)
{
    inode->type        = node->type & ~file_delete;
    inode->size        = node->size;
    inode->realsize    = node->realsize;
    inode->blksz       = node->blksz;
    inode->dev         = node->dev;
    inode->rdev        = node->rdev;
    inode->owner       = node->owner;
    inode->group       = node->group;
    inode->mode        = node->mode;
    inode->permissions = node->permissions;
    inode->nlink       = node->nlink;
    inode->createtime  = node->createtime;
    inode->readtime    = node->readtime;
    inode->writetime   = node->writetime;
}

static void icache_apply(vfs_node_t node, const vfs_inode_t *inode)
{
    uint16_t dentry_bits = node->type & file_delete;
    node->type           = inode->type | dentry_bits;
    node->size           = inode->size;
    node->realsize       = inode->realsize;
    node->blksz          = inode->blksz;
    node->dev            = inode->dev;
    node->rdev           = inode->rdev;
    node->owner          = inode->owner;
    node->group          = inode->group;
    node->mode           = inode->mode;
    node->permissions    = inode->permissions;
    node->nlink          = inode->nlink;
    node->createtime     = inode->createtime;
    node->readtime       = inode->readtime;
    node->writetime      = inode->writetime;
}

void vfs_icache_init(void)
{
    memset(icache_buckets, 0, sizeof(icache_buckets));
    memset(&icache_stats, 0, sizeof(icache_stats));
    icache_clock = 0;
}

static int icache_bind(vfs_node_t node, bool authoritative)
{
    if (!node || !node->inode) return -EINVAL;
    vfs_inode_key_t key = icache_key(node);
    if (node->cache_inode && icache_key_equal(&node->cache_inode->key, &key) && !node->cache_inode->stale) {
        if (authoritative) {
            vfs_icache_publish(node);
        } else {
            vfs_inode_t         *inode  = node->cache_inode;
            vfs_icache_bucket_t *bucket = icache_bucket(&inode->key);
            spin_lock(&bucket->lock);
            if (node->cache_inode == inode && !inode->stale) {
                icache_apply(node, inode);
                inode->age = ICACHE_TICK();
            }
            spin_unlock(&bucket->lock);
        }
        return EOK;
    }
    if (node->cache_inode) {
        vfs_icache_unbind(node);
        ICACHE_INC(rebinds);
    }

    vfs_inode_t *fresh = calloc(1, sizeof(*fresh));
    if (!fresh) return -ENOMEM;
    fresh->key = key;

    vfs_icache_bucket_t *bucket = icache_bucket(&key);
    spin_lock(&bucket->lock);
    bool         created = false;
    vfs_inode_t *inode;
    for (inode = bucket->head; inode; inode = inode->hash_next)
        if (!inode->stale && icache_key_equal(&inode->key, &key)) break;
    if (inode && inode->refs == UINT32_MAX) {
        spin_unlock(&bucket->lock);
        free(fresh);
        return -EOVERFLOW;
    }
    if (!inode) {
        inode            = fresh;
        fresh            = NULL;
        created          = true;
        inode->hash_next = bucket->head;
        bucket->head     = inode;
        icache_snapshot(inode, node);
        ICACHE_INC(entries);
        ICACHE_INC(misses);
    } else {
        ICACHE_INC(hits);
        if (authoritative) icache_snapshot(inode, node);
    }
    inode->age = ICACHE_TICK();
    inode->refs++;
    if (inode->refs == 1) ICACHE_INC(active_entries);
    node->cache_inode      = inode;
    node->inode_alias_prev = NULL;
    node->inode_alias_next = inode->aliases;
    if (inode->aliases) inode->aliases->inode_alias_prev = node;
    inode->aliases = node;
    ICACHE_INC(aliases);
    if (authoritative || created)
        for (vfs_node_t alias = inode->aliases; alias; alias = alias->inode_alias_next) icache_apply(alias, inode);
    else
        icache_apply(node, inode);
    spin_unlock(&bucket->lock);
    free(fresh);
    return EOK;
}

int vfs_icache_bind(vfs_node_t node)
{
    return icache_bind(node, false);
}

int vfs_icache_refresh(vfs_node_t node)
{
    return icache_bind(node, true);
}

void vfs_icache_unbind(vfs_node_t node)
{
    if (!node || !node->cache_inode) return;
    vfs_inode_t         *inode  = node->cache_inode;
    vfs_icache_bucket_t *bucket = icache_bucket(&inode->key);
    spin_lock(&bucket->lock);
    if (node->cache_inode == inode) {
        if (node->inode_alias_prev)
            node->inode_alias_prev->inode_alias_next = node->inode_alias_next;
        else if (inode->aliases == node)
            inode->aliases = node->inode_alias_next;
        if (node->inode_alias_next) node->inode_alias_next->inode_alias_prev = node->inode_alias_prev;
        node->inode_alias_prev = node->inode_alias_next = NULL;
        node->cache_inode                               = NULL;
        if (inode->refs) inode->refs--;
        ICACHE_DEC(aliases);
        if (!inode->refs) {
            ICACHE_DEC(active_entries);
            if (!inode->nlink && !inode->stale) {
                inode->stale = true;
                ICACHE_INC(invalidations);
            }
        }
        inode->age = ICACHE_TICK();
    }
    spin_unlock(&bucket->lock);

    uint64_t entries = __atomic_load_n(&icache_stats.entries, __ATOMIC_RELAXED);
    uint64_t active  = __atomic_load_n(&icache_stats.active_entries, __ATOMIC_RELAXED);
    if (entries > active + VFS_ICACHE_MAX_UNUSED) (void)vfs_icache_reclaim(64);
}

void vfs_icache_publish(vfs_node_t node)
{
    if (!node || !node->cache_inode) return;
    vfs_inode_t         *inode  = node->cache_inode;
    vfs_icache_bucket_t *bucket = icache_bucket(&inode->key);
    spin_lock(&bucket->lock);
    if (node->cache_inode == inode && !inode->stale) {
        icache_snapshot(inode, node);
        inode->age = ICACHE_TICK();
        for (vfs_node_t alias = inode->aliases; alias; alias = alias->inode_alias_next) icache_apply(alias, inode);
    }
    spin_unlock(&bucket->lock);
}

void vfs_icache_invalidate_mount(vfs_node_t mount_root)
{
    if (!mount_root) return;
    uintptr_t mount    = (uintptr_t)mount_root;
    uint64_t  mount_id = mount_root->mount_id;
    for (size_t index = 0; index < VFS_ICACHE_BUCKETS; index++) {
        vfs_icache_bucket_t *bucket = &icache_buckets[index];
        spin_lock(&bucket->lock);
        for (vfs_inode_t *inode = bucket->head; inode; inode = inode->hash_next) {
            if (inode->key.mount == mount && (!mount_id || inode->key.mount_id == mount_id)) {
                inode->stale = true;
                ICACHE_INC(invalidations);
            }
        }
        spin_unlock(&bucket->lock);
    }
}

size_t vfs_icache_reclaim(size_t target)
{
    size_t reclaimed = 0;
    if (!target) return 0;
    for (size_t index = 0; index < VFS_ICACHE_BUCKETS && reclaimed < target; index++) {
        vfs_icache_bucket_t *bucket = &icache_buckets[index];
        vfs_inode_t         *dead   = NULL;
        spin_lock(&bucket->lock);
        vfs_inode_t **link = &bucket->head;
        while (*link && reclaimed < target) {
            vfs_inode_t *inode = *link;
            if (!inode->refs) {
                *link            = inode->hash_next;
                inode->hash_next = dead;
                dead             = inode;
                reclaimed++;
                ICACHE_DEC(entries);
                ICACHE_INC(evictions);
                continue;
            }
            link = &inode->hash_next;
        }
        spin_unlock(&bucket->lock);
        while (dead) {
            vfs_inode_t *next = dead->hash_next;
            free(dead);
            dead = next;
        }
    }
    return reclaimed;
}

void vfs_icache_get_stats(vfs_icache_stats_t *stats)
{
    if (!stats) return;
    stats->entries        = __atomic_load_n(&icache_stats.entries, __ATOMIC_RELAXED);
    stats->active_entries = __atomic_load_n(&icache_stats.active_entries, __ATOMIC_RELAXED);
    stats->aliases        = __atomic_load_n(&icache_stats.aliases, __ATOMIC_RELAXED);
    stats->hits           = __atomic_load_n(&icache_stats.hits, __ATOMIC_RELAXED);
    stats->misses         = __atomic_load_n(&icache_stats.misses, __ATOMIC_RELAXED);
    stats->rebinds        = __atomic_load_n(&icache_stats.rebinds, __ATOMIC_RELAXED);
    stats->invalidations  = __atomic_load_n(&icache_stats.invalidations, __ATOMIC_RELAXED);
    stats->evictions      = __atomic_load_n(&icache_stats.evictions, __ATOMIC_RELAXED);
}
