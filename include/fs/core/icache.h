/*
 * Mount-scoped inode identity cache.
 *
 * A vfs_inode object is unique for (mounted filesystem instance, inode
 * number).  VFS nodes remain directory entries and attach as aliases.
 */

#ifndef INCLUDE_FS_CORE_ICACHE_H_
#define INCLUDE_FS_CORE_ICACHE_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

struct vfs_node;
typedef struct vfs_inode vfs_inode_t;

typedef struct vfs_icache_stats {
        uint64_t entries;
        uint64_t active_entries;
        uint64_t aliases;
        uint64_t hits;
        uint64_t misses;
        uint64_t rebinds;
        uint64_t invalidations;
        uint64_t evictions;
} vfs_icache_stats_t;

void vfs_icache_init(void);

/* Bind a new alias, applying canonical metadata on an existing cache hit. */
int vfs_icache_bind(struct vfs_node *node);

/* Bind after an authoritative filesystem stat/mutation and publish its data. */
int  vfs_icache_refresh(struct vfs_node *node);
void vfs_icache_unbind(struct vfs_node *node);

/* Publish inode metadata to every currently attached hard-link alias. */
void vfs_icache_publish(struct vfs_node *node);

/* Mark all cached identities belonging to one mount instance stale. */
void vfs_icache_invalidate_mount(struct vfs_node *mount_root);

/* Reclaim inactive identities; active aliases are never evicted. */
size_t vfs_icache_reclaim(size_t target);
void   vfs_icache_get_stats(vfs_icache_stats_t *stats);

#endif // INCLUDE_FS_CORE_ICACHE_H_
