/*
 *
 *      slab.h
 *      Kernel slab-cache interface.
 *
 *      2026/8/1 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MEM_SLAB_H_
#define INCLUDE_MEM_SLAB_H_

#include <libs/std/stddef.h>

typedef struct slab_cache slab_cache_t;
typedef void (*slab_ctor_t)(void *object);
typedef void (*slab_dtor_t)(void *object);

typedef struct {
        size_t object_size;
        size_t alignment;
        size_t objects;
        size_t slabs;
        size_t partial_slabs;
        size_t full_slabs;
        size_t empty_slabs;
        size_t allocations;
        size_t frees;
} slab_cache_stats_t;

/* Create/destroy a typed object cache.  Alignment must be a power of two. */
slab_cache_t *slab_cache_create(const char *name, size_t object_size, size_t alignment, slab_ctor_t ctor, slab_dtor_t dtor);
int           slab_cache_destroy(slab_cache_t *cache);

void *slab_cache_alloc(slab_cache_t *cache);
int   slab_cache_free(slab_cache_t *cache, void *object);

/* Return completely empty slabs to the heap page buddy. */
size_t slab_cache_shrink(slab_cache_t *cache);
void   slab_cache_get_stats(slab_cache_t *cache, slab_cache_stats_t *stats);

#endif // INCLUDE_MEM_SLAB_H_
