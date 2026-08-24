/*
 * VFS pathname-component cache.
 *
 * The cache is an index over the authoritative namespace tree.  Positive
 * entries never own a vnode and may therefore be discarded at any time;
 * negative entries are qualified by the parent directory generation.
 */

#ifndef INCLUDE_FS_CORE_DCACHE_H_
#define INCLUDE_FS_CORE_DCACHE_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

struct vfs_node;

typedef struct vfs_dcache_stats {
        uint64_t positive_entries;
        uint64_t negative_entries;
        uint64_t positive_hits;
        uint64_t negative_hits;
        uint64_t misses;
        uint64_t insertions;
        uint64_t invalidations;
        uint64_t evictions;
} vfs_dcache_stats_t;

enum vfs_dcache_result {
    VFS_DCACHE_MISS     = 0,
    VFS_DCACHE_POSITIVE = 1,
    VFS_DCACHE_NEGATIVE = 2,
};

void vfs_dcache_init(void);

/*
 * The caller must hold VFS namespace serialization (or an equivalent parent
 * and child lifetime pin) until it has finished using a positive result.
 */
enum vfs_dcache_result vfs_dcache_lookup(struct vfs_node *parent, const char *name, struct vfs_node **node);

/* Positive entries use storage embedded in the vnode and cannot fail. */
void vfs_dcache_add(struct vfs_node *node);
void vfs_dcache_remove(struct vfs_node *node);

/* Remember an unsuccessful lookup at the parent's current generation. */
void vfs_dcache_add_negative(struct vfs_node *parent, const char *name);

/* Invalidate one name or every negative result below a directory. */
void vfs_dcache_invalidate(struct vfs_node *parent, const char *name);
void vfs_dcache_invalidate_parent(struct vfs_node *parent);

/* Reclaim up to target cold negative entries without touching the namespace. */
size_t vfs_dcache_reclaim(size_t target);
void   vfs_dcache_get_stats(vfs_dcache_stats_t *stats);

#endif // INCLUDE_FS_CORE_DCACHE_H_
