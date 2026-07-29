/*
 *
 *      pagecache.h
 *      Unified file page cache
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FS_PAGECACHE_H_
#define INCLUDE_FS_PAGECACHE_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define PAGECACHE_PAGE_SIZE 4096UL

#define PAGECACHE_MAPPING_UNEVICTABLE (1U << 0)

#define PAGECACHE_WB_SYNC       (1U << 0)
#define PAGECACHE_WB_KEEP_ERROR (1U << 1)

#define PAGECACHE_INVALIDATE_DISCARD_DIRTY (1U << 0)

#define PAGECACHE_EVICT_WRITEBACK     (1U << 0)
#define PAGECACHE_EVICT_DISCARD_DIRTY (1U << 1)

typedef struct pagecache_mapping pagecache_mapping_t;
typedef struct pagecache_page    pagecache_page_t;

/* Operation */
typedef int64_t (*pagecache_read_op_t)(void *context, void *buffer, uint64_t offset, size_t size);
typedef int64_t (*pagecache_write_op_t)(void *context, const void *buffer, uint64_t offset, size_t size);
typedef int (*pagecache_resize_op_t)(void *context, uint64_t size);
typedef int (*pagecache_sync_op_t)(void *context);

typedef struct {
        pagecache_read_op_t   read;
        pagecache_write_op_t  write;
        pagecache_resize_op_t resize;
        pagecache_sync_op_t   sync;
} pagecache_ops_t;

typedef void *(*pagecache_alloc_page_t)(uint64_t *physical);
typedef void (*pagecache_free_page_t)(void *page, uint64_t physical);

typedef struct {
        pagecache_alloc_page_t alloc;
        pagecache_free_page_t  free;
} pagecache_allocator_t;

typedef struct {
        uint64_t pages;
        uint64_t dirty;
        uint64_t writeback;
        uint64_t active;
        uint64_t inactive;
        uint64_t hits;
        uint64_t misses;
        uint64_t reads;
        uint64_t writes;
        uint64_t reclaimed;
        uint64_t writeback_errors;
        uint64_t readahead_pages;
        uint64_t readahead_hits;
        uint64_t clean_evicted;
        uint64_t dirty_evicted;
} pagecache_stats_t;

/* Init and cleanup shit */
int  pagecache_init(const pagecache_allocator_t *allocator, size_t max_pages);
void pagecache_shutdown(void);

/* Mapping operations */
pagecache_mapping_t *pagecache_mapping_create(void *context, const pagecache_ops_t *ops, uint64_t size, uint32_t flags);
void                 pagecache_mapping_destroy(pagecache_mapping_t *mapping);

/* Core Functions */
int64_t  pagecache_read(pagecache_mapping_t *mapping, void *buffer, uint64_t offset, size_t size);
int64_t  pagecache_write(pagecache_mapping_t *mapping, const void *buffer, uint64_t offset, size_t size);
int      pagecache_writeback(pagecache_mapping_t *mapping, uint64_t start, uint64_t end, uint32_t flags);
int      pagecache_writeback_all(uint32_t flags);
int      pagecache_invalidate(pagecache_mapping_t *mapping, uint64_t start, uint64_t end, uint32_t flags);
int      pagecache_evict(pagecache_mapping_t *mapping, uint64_t start, uint64_t end, uint32_t flags);
int      pagecache_truncate(pagecache_mapping_t *mapping, uint64_t size);
uint64_t pagecache_size(const pagecache_mapping_t *mapping);
int      pagecache_mapping_error(pagecache_mapping_t *mapping);
void     pagecache_mapping_pin(pagecache_mapping_t *mapping);
void     pagecache_mapping_unpin(pagecache_mapping_t *mapping);
int      pagecache_readahead(pagecache_mapping_t *mapping, uint64_t offset, size_t size);

/* Page operations */
pagecache_page_t *pagecache_get_page(pagecache_mapping_t *mapping, uint64_t index, int create);
void              pagecache_put_page(pagecache_page_t *page);
int               pagecache_lock_page(pagecache_page_t *page, int populate);
void              pagecache_unlock_page(pagecache_page_t *page);
void             *pagecache_page_data(pagecache_page_t *page);
uint64_t          pagecache_page_physical(pagecache_page_t *page);
uint64_t          pagecache_page_index(pagecache_page_t *page);
void              pagecache_mark_dirty(pagecache_page_t *page);

/* Some shit it is very ugly */
size_t pagecache_reclaim(size_t target);
void   pagecache_get_stats(pagecache_stats_t *stats);

#endif // INCLUDE_FS_PAGECACHE_H_
