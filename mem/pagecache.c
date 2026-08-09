/*
 *
 *      pagecache.c
 *      Unified file page cache
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <mem/pagecache.h>

#define PAGECACHE_HASH_MIN_BITS 6U
#define PAGECACHE_HASH_MAX_BITS 12U
#define PAGECACHE_HASH_MIN_SIZE (1U << PAGECACHE_HASH_MIN_BITS)
#define PAGECACHE_HASH_MAX_SIZE (1U << PAGECACHE_HASH_MAX_BITS)
#define PAGECACHE_HASH_LOAD     4U
#define PAGECACHE_READAHEAD_MIN 2U
#define PAGECACHE_READAHEAD_MAX 32U

#define PC_PAGE_UPTODATE   (1U << 0)
#define PC_PAGE_DIRTY      (1U << 1)
#define PC_PAGE_WRITEBACK  (1U << 2)
#define PC_PAGE_ERROR      (1U << 3)
#define PC_PAGE_REFERENCED (1U << 4)
#define PC_PAGE_ACTIVE     (1U << 5)
#define PC_PAGE_EVICTING   (1U << 6)
#define PC_PAGE_READAHEAD  (1U << 7)
#define PC_PAGE_WAS_DIRTY  (1U << 8)

typedef struct {
        volatile uint32_t value;
} pc_lock_t;

typedef struct pagecache_page {
        pagecache_mapping_t *mapping;
        pagecache_page_t    *hash_next;
        pagecache_page_t    *lru_prev;
        pagecache_page_t    *lru_next;
        uint64_t             index;
        uint64_t             physical;
        void                *data;
        volatile uint32_t    flags;
        volatile uint32_t    references;
        pc_lock_t            lock;
} pagecache_page_t;

typedef struct pagecache_mapping {
        void                *context;
        pagecache_ops_t      ops;
        pagecache_page_t    *inline_buckets[PAGECACHE_HASH_MIN_SIZE];
        pagecache_page_t   **buckets;
        size_t               bucket_count;
        pc_lock_t            lock;
        volatile uint64_t    size;
        volatile int         error;
        uint32_t             flags;
        size_t               pages;
        pagecache_mapping_t *global_prev;
        pagecache_mapping_t *global_next;
        volatile uint32_t    references;
        volatile uint32_t    dying;
        volatile uint32_t    pins;
        uint64_t             readahead_last;
        uint64_t             readahead_end;
        uint32_t             readahead_window;
        uint32_t             readahead_valid;
} pagecache_mapping_t;

typedef struct {
        pc_lock_t             lock;
        pagecache_allocator_t allocator;
        pagecache_page_t     *lru_head;
        pagecache_page_t     *lru_tail;
        pagecache_mapping_t  *mappings;
        size_t                max_pages;
        volatile uint32_t     initialized;
        pagecache_stats_t     stats;
} pagecache_state_t;

static pagecache_state_t pagecache;

static inline void pc_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

static void pc_lock(pc_lock_t *lock)
{
    while (__atomic_exchange_n(&lock->value, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&lock->value, __ATOMIC_RELAXED)) pc_relax();
}

static int pc_trylock(pc_lock_t *lock)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&lock->value, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void pc_unlock(pc_lock_t *lock)
{
    __atomic_store_n(&lock->value, 0, __ATOMIC_RELEASE);
}

static inline size_t pc_hash(uint64_t index, size_t bucket_count)
{
    index ^= index >> 33;
    index *= 0xff51afd7ed558ccdULL;
    index ^= index >> 33;
    return (size_t)index & (bucket_count - 1);
}

static inline void pc_stat_inc(uint64_t *value)
{
    __atomic_add_fetch(value, 1, __ATOMIC_RELAXED);
}

static inline void pc_stat_dec(uint64_t *value)
{
    __atomic_sub_fetch(value, 1, __ATOMIC_RELAXED);
}

static void pc_lru_remove_locked(pagecache_page_t *page)
{
    if (page->lru_prev)
        page->lru_prev->lru_next = page->lru_next;
    else
        pagecache.lru_head = page->lru_next;
    if (page->lru_next)
        page->lru_next->lru_prev = page->lru_prev;
    else
        pagecache.lru_tail = page->lru_prev;
    page->lru_prev = page->lru_next = NULL;
}

static void pc_lru_add_head_locked(pagecache_page_t *page)
{
    page->lru_prev = NULL;
    page->lru_next = pagecache.lru_head;
    if (pagecache.lru_head) pagecache.lru_head->lru_prev = page;
    pagecache.lru_head = page;
    if (!pagecache.lru_tail) pagecache.lru_tail = page;
}

static void pc_lru_add_tail_locked(pagecache_page_t *page)
{
    page->lru_next = NULL;
    page->lru_prev = pagecache.lru_tail;
    if (pagecache.lru_tail) pagecache.lru_tail->lru_next = page;
    pagecache.lru_tail = page;
    if (!pagecache.lru_head) pagecache.lru_head = page;
}

static void pc_touch(pagecache_page_t *page)
{
    pc_lock(&page->lock);
    pc_lock(&pagecache.lock);
    if (!(page->flags & PC_PAGE_EVICTING)) {
        if (page->flags & PC_PAGE_READAHEAD) {
            page->flags &= ~PC_PAGE_READAHEAD;
            pc_stat_dec(&pagecache.stats.readahead_pages);
            pc_stat_inc(&pagecache.stats.readahead_hits);
        }
        if (page->flags & PC_PAGE_REFERENCED) {
            if (!(page->flags & PC_PAGE_ACTIVE)) {
                page->flags |= PC_PAGE_ACTIVE;
                pc_stat_inc(&pagecache.stats.active);
                pc_stat_dec(&pagecache.stats.inactive);
            }
        } else {
            page->flags |= PC_PAGE_REFERENCED;
        }
        if (pagecache.lru_head != page) {
            pc_lru_remove_locked(page);
            pc_lru_add_head_locked(page);
        }
    }
    pc_unlock(&pagecache.lock);
    pc_unlock(&page->lock);
}

static pagecache_page_t *pc_find_locked(pagecache_mapping_t *mapping, uint64_t index)
{
    for (pagecache_page_t *page = mapping->buckets[pc_hash(index, mapping->bucket_count)]; page; page = page->hash_next)
        if (page->index == index && !(__atomic_load_n(&page->flags, __ATOMIC_ACQUIRE) & PC_PAGE_EVICTING)) return page;
    return NULL;
}

/* Keep mappings for the many small files compact, but do not let a large
 * tmpfs file turn every page lookup into a walk over a long collision chain.
 * Growth is best-effort: allocation failure only keeps the old valid table. */
static void pc_grow_hash_locked(pagecache_mapping_t *mapping)
{
    if (mapping->bucket_count >= PAGECACHE_HASH_MAX_SIZE
        || mapping->pages < mapping->bucket_count * PAGECACHE_HASH_LOAD)
        return;

    size_t new_count = mapping->bucket_count << 1;
    if (new_count > PAGECACHE_HASH_MAX_SIZE) new_count = PAGECACHE_HASH_MAX_SIZE;
    pagecache_page_t **new_buckets = calloc(new_count, sizeof(*new_buckets));
    if (!new_buckets) return;

    for (size_t i = 0; i < mapping->bucket_count; i++) {
        pagecache_page_t *page = mapping->buckets[i];
        while (page) {
            pagecache_page_t *next = page->hash_next;
            size_t            hash = pc_hash(page->index, new_count);
            page->hash_next        = new_buckets[hash];
            new_buckets[hash]      = page;
            page                   = next;
        }
    }

    if (mapping->buckets != mapping->inline_buckets) free(mapping->buckets);
    mapping->buckets      = new_buckets;
    mapping->bucket_count = new_count;
}

static void pc_free_page(pagecache_page_t *page)
{
    if (page->flags & PC_PAGE_READAHEAD) pc_stat_dec(&pagecache.stats.readahead_pages);
    if (page->flags & PC_PAGE_DIRTY) pc_stat_dec(&pagecache.stats.dirty);
    if (page->flags & PC_PAGE_WRITEBACK) pc_stat_dec(&pagecache.stats.writeback);
    if (page->flags & PC_PAGE_ACTIVE)
        pc_stat_dec(&pagecache.stats.active);
    else
        pc_stat_dec(&pagecache.stats.inactive);
    pc_stat_dec(&pagecache.stats.pages);
    if (page->flags & PC_PAGE_WAS_DIRTY)
        pc_stat_inc(&pagecache.stats.dirty_evicted);
    else
        pc_stat_inc(&pagecache.stats.clean_evicted);
    pagecache.allocator.free(page->data, page->physical);
    free(page);
}

static int pc_unlink_page(pagecache_page_t *page)
{
    pagecache_mapping_t *mapping = page->mapping;
    pc_lock(&mapping->lock);
    pagecache_page_t **link = &mapping->buckets[pc_hash(page->index, mapping->bucket_count)];
    while (*link && *link != page) link = &(*link)->hash_next;
    if (*link != page) {
        pc_unlock(&mapping->lock);
        return -ENOENT;
    }
    *link = page->hash_next;
    mapping->pages--;
    pc_unlock(&mapping->lock);

    pc_lock(&pagecache.lock);
    pc_lru_remove_locked(page);
    pc_unlock(&pagecache.lock);
    return EOK;
}

static int pc_load_locked(pagecache_page_t *page)
{
    pagecache_mapping_t *mapping = page->mapping;
    if (page->flags & PC_PAGE_UPTODATE) return EOK;
    if (!mapping->ops.read) return -EIO;

    uint64_t start = page->index * PAGECACHE_PAGE_SIZE;
    uint64_t limit = __atomic_load_n(&mapping->size, __ATOMIC_ACQUIRE);
    size_t   count = start >= limit ? 0 : PAGECACHE_PAGE_SIZE;
    if (count > limit - start) count = (size_t)(limit - start);
    memset(page->data, 0, PAGECACHE_PAGE_SIZE);
    int64_t result = count ? mapping->ops.read(mapping->context, page->data, start, count) : 0;
    pc_stat_inc(&pagecache.stats.reads);
    if (result < 0) {
        page->flags |= PC_PAGE_ERROR;
        __atomic_store_n(&mapping->error, (int)result, __ATOMIC_RELEASE);
        return (int)result;
    }
    if ((uint64_t)result > count) result = (int64_t)count;
    if ((size_t)result < count) memset((char *)page->data + result, 0, count - (size_t)result);
    page->flags |= PC_PAGE_UPTODATE;
    page->flags &= ~PC_PAGE_ERROR;
    return EOK;
}

static int pc_writeback_page_locked(pagecache_page_t *page)
{
    pagecache_mapping_t *mapping = page->mapping;
    if (!(page->flags & PC_PAGE_DIRTY)) return EOK;
    if (!mapping->ops.write) return -EROFS;

    uint64_t start = page->index * PAGECACHE_PAGE_SIZE;
    uint64_t limit = __atomic_load_n(&mapping->size, __ATOMIC_ACQUIRE);
    size_t   count = start >= limit ? 0 : PAGECACHE_PAGE_SIZE;
    if (count > limit - start) count = (size_t)(limit - start);

    page->flags |= PC_PAGE_WRITEBACK;
    pc_stat_inc(&pagecache.stats.writeback);
    int64_t result = count ? mapping->ops.write(mapping->context, page->data, start, count) : 0;
    pc_stat_inc(&pagecache.stats.writes);
    page->flags &= ~PC_PAGE_WRITEBACK;
    pc_stat_dec(&pagecache.stats.writeback);
    if (result < 0 || (size_t)result != count) {
        int error = result < 0 ? (int)result : -EIO;
        page->flags |= PC_PAGE_ERROR;
        __atomic_store_n(&mapping->error, error, __ATOMIC_RELEASE);
        pc_stat_inc(&pagecache.stats.writeback_errors);
        return error;
    }
    page->flags &= ~(PC_PAGE_DIRTY | PC_PAGE_ERROR);
    pc_stat_dec(&pagecache.stats.dirty);
    return EOK;
}

int pagecache_init(const pagecache_allocator_t *allocator, size_t max_pages)
{
    if (!allocator || !allocator->alloc || !allocator->free || !max_pages) return -EINVAL;
    if (__atomic_load_n(&pagecache.initialized, __ATOMIC_ACQUIRE)) return -EBUSY;
    memset(&pagecache, 0, sizeof(pagecache));
    pagecache.allocator = *allocator;
    pagecache.max_pages = max_pages;
    __atomic_store_n(&pagecache.initialized, 1, __ATOMIC_RELEASE);
    return EOK;
}

void pagecache_shutdown(void)
{
    if (!__atomic_load_n(&pagecache.initialized, __ATOMIC_ACQUIRE)) return;
    (void)pagecache_reclaim((size_t)-1);
    __atomic_store_n(&pagecache.initialized, 0, __ATOMIC_RELEASE);
}

pagecache_mapping_t *pagecache_mapping_create(void *context, const pagecache_ops_t *ops, uint64_t size, uint32_t flags)
{
    if (!ops || !ops->read || !__atomic_load_n(&pagecache.initialized, __ATOMIC_ACQUIRE)) return NULL;
    pagecache_mapping_t *mapping = calloc(1, sizeof(*mapping));
    if (!mapping) return NULL;
    mapping->context    = context;
    mapping->ops        = *ops;
    mapping->buckets    = mapping->inline_buckets;
    mapping->bucket_count = PAGECACHE_HASH_MIN_SIZE;
    mapping->size       = size;
    mapping->flags      = flags;
    mapping->references = 1;
    pc_lock(&pagecache.lock);
    mapping->global_next = pagecache.mappings;
    if (pagecache.mappings) pagecache.mappings->global_prev = mapping;
    pagecache.mappings = mapping;
    pc_unlock(&pagecache.lock);
    return mapping;
}

void pagecache_mapping_destroy(pagecache_mapping_t *mapping)
{
    if (!mapping) return;
    pc_lock(&pagecache.lock);
    mapping->dying = 1;
    if (mapping->global_prev)
        mapping->global_prev->global_next = mapping->global_next;
    else
        pagecache.mappings = mapping->global_next;
    if (mapping->global_next) mapping->global_next->global_prev = mapping->global_prev;
    pc_unlock(&pagecache.lock);
    while (__atomic_load_n(&mapping->references, __ATOMIC_ACQUIRE) != 1) pc_relax();
    (void)pagecache_writeback(mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC | PAGECACHE_WB_KEEP_ERROR);
    (void)pagecache_invalidate(mapping, 0, UINT64_MAX, PAGECACHE_INVALIDATE_DISCARD_DIRTY);
    if (mapping->buckets != mapping->inline_buckets) free(mapping->buckets);
    free(mapping);
}

static pagecache_page_t *pc_get_page(pagecache_mapping_t *mapping, uint64_t index, int create, int accessed, int reclaim)
{
    if (!mapping) return NULL;
    pc_lock(&mapping->lock);
    pagecache_page_t *page = pc_find_locked(mapping, index);
    if (page) {
        __atomic_add_fetch(&page->references, 1, __ATOMIC_ACQ_REL);
        pc_unlock(&mapping->lock);
        pc_stat_inc(&pagecache.stats.hits);
        if (accessed) pc_touch(page);
        return page;
    }
    pc_unlock(&mapping->lock);
    if (!create) return NULL;

    if (__atomic_load_n(&pagecache.stats.pages, __ATOMIC_RELAXED) >= pagecache.max_pages)
        if (!reclaim || !pagecache_reclaim(1)) return NULL;
    page = calloc(1, sizeof(*page));
    if (!page) return NULL;
    page->data = pagecache.allocator.alloc(&page->physical);
    if (!page->data && reclaim && pagecache_reclaim(PAGECACHE_READAHEAD_MIN)) page->data = pagecache.allocator.alloc(&page->physical);
    if (!page->data) {
        free(page);
        return NULL;
    }
    page->mapping    = mapping;
    page->index      = index;
    page->references = 1;
    page->flags      = accessed ? PC_PAGE_REFERENCED : PC_PAGE_READAHEAD;

    pc_lock(&mapping->lock);
    pagecache_page_t *existing = pc_find_locked(mapping, index);
    if (existing) {
        __atomic_add_fetch(&existing->references, 1, __ATOMIC_ACQ_REL);
        pc_unlock(&mapping->lock);
        pagecache.allocator.free(page->data, page->physical);
        free(page);
        pc_stat_inc(&pagecache.stats.hits);
        if (accessed) pc_touch(existing);
        return existing;
    }
    pc_grow_hash_locked(mapping);
    size_t bucket            = pc_hash(index, mapping->bucket_count);
    page->hash_next          = mapping->buckets[bucket];
    mapping->buckets[bucket] = page;
    mapping->pages++;
    pc_unlock(&mapping->lock);

    pc_lock(&pagecache.lock);
    if (accessed)
        pc_lru_add_head_locked(page);
    else
        pc_lru_add_tail_locked(page);
    pc_unlock(&pagecache.lock);
    pc_stat_inc(&pagecache.stats.pages);
    pc_stat_inc(&pagecache.stats.inactive);
    if (!accessed) pc_stat_inc(&pagecache.stats.readahead_pages);
    pc_stat_inc(&pagecache.stats.misses);
    return page;
}

pagecache_page_t *pagecache_get_page(pagecache_mapping_t *mapping, uint64_t index, int create)
{
    return pc_get_page(mapping, index, create, 1, 1);
}

void pagecache_put_page(pagecache_page_t *page)
{
    if (!page) return;
    __atomic_sub_fetch(&page->references, 1, __ATOMIC_ACQ_REL);
}

int pagecache_lock_page(pagecache_page_t *page, int populate)
{
    if (!page) return -EINVAL;
    pc_lock(&page->lock);
    if (page->flags & PC_PAGE_EVICTING) {
        pc_unlock(&page->lock);
        return -ENOENT;
    }
    if (populate) {
        int result = pc_load_locked(page);
        if (result) {
            pc_unlock(&page->lock);
            return result;
        }
    }
    return EOK;
}

void pagecache_unlock_page(pagecache_page_t *page)
{
    if (page) pc_unlock(&page->lock);
}

void *pagecache_page_data(pagecache_page_t *page)
{
    return page ? page->data : NULL;
}

uint64_t pagecache_page_physical(pagecache_page_t *page)
{
    return page ? page->physical : 0;
}

uint64_t pagecache_page_index(pagecache_page_t *page)
{
    return page ? page->index : 0;
}

void pagecache_mark_dirty(pagecache_page_t *page)
{
    if (!page) return;
    if (!(page->flags & PC_PAGE_DIRTY)) {
        page->flags |= PC_PAGE_DIRTY | PC_PAGE_WAS_DIRTY;
        pc_stat_inc(&pagecache.stats.dirty);
    }
    page->flags |= PC_PAGE_UPTODATE | PC_PAGE_REFERENCED;
}

static int pc_readahead_pages(pagecache_mapping_t *mapping, uint64_t first, uint32_t count, int strict)
{
    if (__atomic_load_n(&mapping->pins, __ATOMIC_ACQUIRE)) return EOK;
    uint64_t size       = __atomic_load_n(&mapping->size, __ATOMIC_ACQUIRE);
    uint64_t file_pages = size / PAGECACHE_PAGE_SIZE + (size % PAGECACHE_PAGE_SIZE != 0);
    if (!file_pages || first >= file_pages) return EOK;
    uint64_t available = file_pages - first;
    if ((uint64_t)count > available) count = (uint32_t)available;

    for (uint32_t offset = 0; offset < count; offset++) {
        pagecache_page_t *page = pc_get_page(mapping, first + offset, 1, 0, 0);
        if (!page) return EOK;
        int result = pagecache_lock_page(page, 1);
        if (!result) pagecache_unlock_page(page);
        pagecache_put_page(page);
        if (result) return strict ? result : EOK;
    }
    return EOK;
}

static void pc_adaptive_readahead(pagecache_mapping_t *mapping, uint64_t first, uint64_t last)
{
    uint64_t prefetch_first = 0;
    uint32_t prefetch_count = 0;

    pc_lock(&mapping->lock);
    int sequential = mapping->readahead_valid && mapping->readahead_last != UINT64_MAX
                     && first == mapping->readahead_last + 1;
    if (!sequential) {
        mapping->readahead_window = 1;
        mapping->readahead_end    = last;
    } else if (last >= mapping->readahead_end) {
        uint32_t window = mapping->readahead_window;
        if (window < PAGECACHE_READAHEAD_MIN)
            window = PAGECACHE_READAHEAD_MIN;
        else if (window < PAGECACHE_READAHEAD_MAX / 2)
            window *= 2;
        else
            window = PAGECACHE_READAHEAD_MAX;
        mapping->readahead_window = window;
        prefetch_first            = last + 1;
        prefetch_count            = window;
        mapping->readahead_end    = UINT64_MAX - last < window ? UINT64_MAX : last + window;
    } else {
        /* The current request consumed pages which are already inside the
         * last prefetched window.  Do not rescan that window on every read. */
        prefetch_count = 0;
    }
    mapping->readahead_last  = last;
    mapping->readahead_valid = 1;
    pc_unlock(&mapping->lock);

    if (prefetch_count && last != UINT64_MAX) (void)pc_readahead_pages(mapping, prefetch_first, prefetch_count, 0);
}

int64_t pagecache_read(pagecache_mapping_t *mapping, void *buffer, uint64_t offset, size_t size)
{
    if (!mapping || (!buffer && size)) return -EINVAL;
    uint64_t limit = __atomic_load_n(&mapping->size, __ATOMIC_ACQUIRE);
    if (offset >= limit || !size) return 0;
    if (size > limit - offset) size = (size_t)(limit - offset);

    size_t done = 0;
    while (done < size) {
        uint64_t page_offset = offset + done;
        uint64_t index       = page_offset / PAGECACHE_PAGE_SIZE;
        size_t   inside      = (size_t)(page_offset % PAGECACHE_PAGE_SIZE);
        size_t   count       = PAGECACHE_PAGE_SIZE - inside;
        if (count > size - done) count = size - done;
        pagecache_page_t *page = pagecache_get_page(mapping, index, 1);
        if (!page) return done ? (int64_t)done : -ENOMEM;
        int result = pagecache_lock_page(page, 1);
        if (result) {
            pagecache_put_page(page);
            return done ? (int64_t)done : result;
        }
        memcpy((char *)buffer + done, (char *)page->data + inside, count);
        pagecache_unlock_page(page);
        pagecache_put_page(page);
        done += count;
    }
    pc_adaptive_readahead(mapping, offset / PAGECACHE_PAGE_SIZE, (offset + done - 1) / PAGECACHE_PAGE_SIZE);
    return (int64_t)done;
}

static void pc_extend_size(pagecache_mapping_t *mapping, uint64_t end)
{
    uint64_t old = __atomic_load_n(&mapping->size, __ATOMIC_ACQUIRE);
    while (old < end && !__atomic_compare_exchange_n(&mapping->size, &old, end, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

int64_t pagecache_write(pagecache_mapping_t *mapping, const void *buffer, uint64_t offset, size_t size)
{
    if (!mapping || (!buffer && size)) return -EINVAL;
    if (!mapping->ops.write) return -EROFS;
    if (!size) return 0;
    if (offset > UINT64_MAX - size) return -EFBIG;
    pc_lock(&mapping->lock);
    mapping->readahead_valid = 0;
    pc_unlock(&mapping->lock);

    size_t done = 0;
    while (done < size) {
        uint64_t page_offset = offset + done;
        uint64_t index       = page_offset / PAGECACHE_PAGE_SIZE;
        size_t   inside      = (size_t)(page_offset % PAGECACHE_PAGE_SIZE);
        size_t   count       = PAGECACHE_PAGE_SIZE - inside;
        if (count > size - done) count = size - done;
        pagecache_page_t *page = pagecache_get_page(mapping, index, 1);
        if (!page) return done ? (int64_t)done : -ENOMEM;

        pc_lock(&page->lock);
        uint64_t old_size   = __atomic_load_n(&mapping->size, __ATOMIC_ACQUIRE);
        uint64_t page_start = index * PAGECACHE_PAGE_SIZE;
        int      result     = EOK;
        if (!(page->flags & PC_PAGE_UPTODATE)) {
            if ((inside || count != PAGECACHE_PAGE_SIZE) && page_start < old_size) {
                result = pc_load_locked(page);
            } else {
                memset(page->data, 0, PAGECACHE_PAGE_SIZE);
                page->flags |= PC_PAGE_UPTODATE;
            }
        }
        if (!result) {
            uint64_t old_in_page = old_size > page_start ? old_size - page_start : 0;
            if (old_in_page > PAGECACHE_PAGE_SIZE) old_in_page = PAGECACHE_PAGE_SIZE;
            if (inside > old_in_page) memset((char *)page->data + old_in_page, 0, inside - (size_t)old_in_page);
            memcpy((char *)page->data + inside, (const char *)buffer + done, count);
            pagecache_mark_dirty(page);
            pc_extend_size(mapping, page_offset + count);
        }
        pc_unlock(&page->lock);
        pagecache_put_page(page);
        if (result) return done ? (int64_t)done : result;
        done += count;
    }
    return (int64_t)done;
}

static void pc_sort_sift_down(pagecache_page_t **pages, size_t root, size_t count)
{
    for (;;) {
        size_t child = root * 2 + 1;
        if (child >= count) return;
        if (child + 1 < count && pages[child]->index < pages[child + 1]->index) child++;
        if (pages[root]->index >= pages[child]->index) return;
        pagecache_page_t *swap = pages[root];
        pages[root]            = pages[child];
        pages[child]           = swap;
        root                   = child;
    }
}

static void pc_sort_pages(pagecache_page_t **pages, size_t count)
{
    /* In-place heapsort keeps writeback ordered without the quadratic close
     * time of insertion sort on files containing tens of thousands of pages. */
    for (size_t root = count / 2; root; root--) pc_sort_sift_down(pages, root - 1, count);
    for (size_t end = count; end > 1; end--) {
        pagecache_page_t *swap = pages[0];
        pages[0]               = pages[end - 1];
        pages[end - 1]         = swap;
        pc_sort_sift_down(pages, 0, end - 1);
    }
}

int pagecache_writeback(pagecache_mapping_t *mapping, uint64_t start, uint64_t end, uint32_t flags)
{
    if (!mapping) return -EINVAL;
    if (end < start) return EOK;
    pc_lock(&mapping->lock);
    size_t capacity = 0;
    for (size_t i = 0; i < mapping->bucket_count; i++) {
        for (pagecache_page_t *page = mapping->buckets[i]; page; page = page->hash_next) {
            uint64_t page_start = page->index * PAGECACHE_PAGE_SIZE;
            uint64_t page_end   = page_start + PAGECACHE_PAGE_SIZE - 1;
            uint32_t state      = __atomic_load_n(&page->flags, __ATOMIC_ACQUIRE);
            if (page_end >= start && page_start <= end && (state & PC_PAGE_DIRTY) && !(state & PC_PAGE_EVICTING)) capacity++;
        }
    }
    if (!capacity) {
        pc_unlock(&mapping->lock);
        int result = mapping->ops.sync && (flags & PAGECACHE_WB_SYNC) ? mapping->ops.sync(mapping->context) : EOK;
        if (!result && !(flags & PAGECACHE_WB_KEEP_ERROR)) __atomic_store_n(&mapping->error, 0, __ATOMIC_RELEASE);
        return result;
    }

    size_t             slots = capacity;
    pagecache_page_t **pages = (pagecache_page_t **)malloc(slots * sizeof(*pages)); // NOLINT(bugprone-sizeof-expression)
    if (!pages) {
        pc_unlock(&mapping->lock);
        return -ENOMEM;
    }
    size_t count = 0;
    for (size_t i = 0; i < mapping->bucket_count; i++) {
        for (pagecache_page_t *page = mapping->buckets[i]; page; page = page->hash_next) {
            uint64_t page_start = page->index * PAGECACHE_PAGE_SIZE;
            uint64_t page_end   = page_start + PAGECACHE_PAGE_SIZE - 1;
            uint32_t state      = __atomic_load_n(&page->flags, __ATOMIC_ACQUIRE);
            if (count < capacity && page_end >= start && page_start <= end && (state & PC_PAGE_DIRTY) && !(state & PC_PAGE_EVICTING)) {
                __atomic_add_fetch(&page->references, 1, __ATOMIC_ACQ_REL);
                pages[count++] = page;
            }
        }
    }
    pc_unlock(&mapping->lock);

    pc_sort_pages(pages, count);

    int first_error = EOK;
    for (size_t i = 0; i < count; i++) {
        pc_lock(&pages[i]->lock);
        int result = pc_writeback_page_locked(pages[i]);
        pc_unlock(&pages[i]->lock);
        pagecache_put_page(pages[i]);
        if (result && !first_error) first_error = result;
    }
    free((void *)pages);
    if (!first_error && mapping->ops.sync && (flags & PAGECACHE_WB_SYNC)) first_error = mapping->ops.sync(mapping->context);
    if (!first_error && !(flags & PAGECACHE_WB_KEEP_ERROR)) __atomic_store_n(&mapping->error, 0, __ATOMIC_RELEASE);
    return first_error;
}

int pagecache_writeback_all(uint32_t flags)
{
    int first_error = EOK;
    pc_lock(&pagecache.lock);
    size_t count = 0;
    for (pagecache_mapping_t *mapping = pagecache.mappings; mapping; mapping = mapping->global_next)
        if (!mapping->dying) count++;
    size_t                slots    = count ? count : 1;
    pagecache_mapping_t **mappings = (pagecache_mapping_t **)malloc(slots * sizeof(*mappings)); // NOLINT(bugprone-sizeof-expression)
    if (!mappings) {
        pc_unlock(&pagecache.lock);
        return -ENOMEM;
    }
    size_t used = 0;
    for (pagecache_mapping_t *mapping = pagecache.mappings; mapping; mapping = mapping->global_next) {
        if (mapping->dying) continue;
        __atomic_add_fetch(&mapping->references, 1, __ATOMIC_ACQ_REL);
        mappings[used++] = mapping;
    }
    pc_unlock(&pagecache.lock);

    for (size_t i = 0; i < used; i++) {
        int result = pagecache_writeback(mappings[i], 0, UINT64_MAX, flags);
        if (result && !first_error) first_error = result;
        __atomic_sub_fetch(&mappings[i]->references, 1, __ATOMIC_ACQ_REL);
    }
    free((void *)mappings);
    return first_error;
}

int pagecache_invalidate(pagecache_mapping_t *mapping, uint64_t start, uint64_t end, uint32_t flags)
{
    if (!mapping) return -EINVAL;
    if (end < start) return EOK;
    if (__atomic_load_n(&mapping->pins, __ATOMIC_ACQUIRE) && !(flags & PAGECACHE_INVALIDATE_DISCARD_DIRTY)) return -EBUSY;
    for (;;) {
        pagecache_page_t *victim = NULL;
        pc_lock(&mapping->lock);
        for (size_t i = 0; i < mapping->bucket_count && !victim; i++) {
            for (pagecache_page_t *page = mapping->buckets[i]; page; page = page->hash_next) {
                uint64_t page_start = page->index * PAGECACHE_PAGE_SIZE;
                uint64_t page_end   = page_start + PAGECACHE_PAGE_SIZE - 1;
                if (page_end >= start && page_start <= end && !(page->flags & PC_PAGE_EVICTING)) {
                    __atomic_add_fetch(&page->references, 1, __ATOMIC_ACQ_REL);
                    victim = page;
                    break;
                }
            }
        }
        pc_unlock(&mapping->lock);
        if (!victim) return EOK;

        pc_lock(&victim->lock);
        if ((victim->flags & PC_PAGE_DIRTY) && !(flags & PAGECACHE_INVALIDATE_DISCARD_DIRTY)) {
            pc_unlock(&victim->lock);
            pagecache_put_page(victim);
            return -EBUSY;
        }
        victim->flags |= PC_PAGE_EVICTING;
        if (pc_unlink_page(victim)) {
            victim->flags &= ~PC_PAGE_EVICTING;
            pc_unlock(&victim->lock);
            pagecache_put_page(victim);
            continue;
        }
        pc_unlock(&victim->lock);
        while (__atomic_load_n(&victim->references, __ATOMIC_ACQUIRE) != 1) pc_relax();
        pagecache_put_page(victim);
        pc_free_page(victim);
    }
}

int pagecache_evict(pagecache_mapping_t *mapping, uint64_t start, uint64_t end, uint32_t flags)
{
    if (!mapping || end < start || (flags & ~(PAGECACHE_EVICT_WRITEBACK | PAGECACHE_EVICT_DISCARD_DIRTY))) return -EINVAL;
    if ((flags & PAGECACHE_EVICT_WRITEBACK) && (flags & PAGECACHE_EVICT_DISCARD_DIRTY)) return -EINVAL;

    int dirty = 0;
    pc_lock(&mapping->lock);
    for (size_t i = 0; i < mapping->bucket_count && !dirty; i++) {
        for (pagecache_page_t *page = mapping->buckets[i]; page; page = page->hash_next) {
            uint64_t page_start = page->index * PAGECACHE_PAGE_SIZE;
            uint64_t page_end   = page_start + PAGECACHE_PAGE_SIZE - 1;
            if (page_end >= start && page_start <= end && (page->flags & PC_PAGE_DIRTY)) {
                dirty = 1;
                break;
            }
        }
    }
    pc_unlock(&mapping->lock);

    if (dirty && !(flags & (PAGECACHE_EVICT_WRITEBACK | PAGECACHE_EVICT_DISCARD_DIRTY))) return -EBUSY;
    if (dirty && (flags & PAGECACHE_EVICT_WRITEBACK)) {
        int result = pagecache_writeback(mapping, start, end, PAGECACHE_WB_SYNC | PAGECACHE_WB_KEEP_ERROR);
        if (result) return result;
    }
    uint32_t invalidate = (flags & PAGECACHE_EVICT_DISCARD_DIRTY) ? PAGECACHE_INVALIDATE_DISCARD_DIRTY : 0;
    return pagecache_invalidate(mapping, start, end, invalidate);
}

int pagecache_truncate(pagecache_mapping_t *mapping, uint64_t size)
{
    if (!mapping || !mapping->ops.resize) return -EOPNOTSUPP;
    uint64_t old_size = __atomic_load_n(&mapping->size, __ATOMIC_ACQUIRE);
    if (size == old_size) return EOK;
    pc_lock(&mapping->lock);
    mapping->readahead_valid = 0;
    pc_unlock(&mapping->lock);
    int result = mapping->ops.resize(mapping->context, size);
    if (result) return result;
    __atomic_store_n(&mapping->size, size, __ATOMIC_RELEASE);

    if (size < old_size) {
        uint64_t first_dead = (size + PAGECACHE_PAGE_SIZE - 1) / PAGECACHE_PAGE_SIZE;
        if (first_dead <= UINT64_MAX / PAGECACHE_PAGE_SIZE)
            (void)pagecache_invalidate(mapping, first_dead * PAGECACHE_PAGE_SIZE, UINT64_MAX, PAGECACHE_INVALIDATE_DISCARD_DIRTY);
        if (size % PAGECACHE_PAGE_SIZE) {
            pagecache_page_t *page = pagecache_get_page(mapping, size / PAGECACHE_PAGE_SIZE, 0);
            if (page) {
                pc_lock(&page->lock);
                memset((char *)page->data + size % PAGECACHE_PAGE_SIZE, 0, PAGECACHE_PAGE_SIZE - size % PAGECACHE_PAGE_SIZE);
                pc_unlock(&page->lock);
                pagecache_put_page(page);
            }
        }
    }
    return EOK;
}

uint64_t pagecache_size(const pagecache_mapping_t *mapping)
{
    return mapping ? __atomic_load_n(&mapping->size, __ATOMIC_ACQUIRE) : 0;
}

int pagecache_mapping_error(pagecache_mapping_t *mapping)
{
    return mapping ? __atomic_load_n(&mapping->error, __ATOMIC_ACQUIRE) : -EINVAL;
}

void pagecache_mapping_pin(pagecache_mapping_t *mapping)
{
    if (mapping) __atomic_add_fetch(&mapping->pins, 1, __ATOMIC_ACQ_REL);
}

void pagecache_mapping_unpin(pagecache_mapping_t *mapping)
{
    if (mapping) __atomic_sub_fetch(&mapping->pins, 1, __ATOMIC_ACQ_REL);
}

int pagecache_readahead(pagecache_mapping_t *mapping, uint64_t offset, size_t size)
{
    if (!mapping || (size && offset > UINT64_MAX - size)) return -EINVAL;
    if (!size) return EOK;
    uint64_t first = offset / PAGECACHE_PAGE_SIZE;
    uint64_t last  = (offset + size - 1) / PAGECACHE_PAGE_SIZE;
    while (first <= last) {
        uint64_t remaining = last - first + 1;
        uint32_t window    = remaining > PAGECACHE_READAHEAD_MAX ? PAGECACHE_READAHEAD_MAX : (uint32_t)remaining;
        int      result    = pc_readahead_pages(mapping, first, window, 1);
        if (result) return result;
        if (remaining <= window) break;
        first += window;
    }
    return EOK;
}

size_t pagecache_reclaim(size_t target)
{
    size_t reclaimed = 0;
    while (reclaimed < target) {
        pagecache_page_t *victim = NULL;
        pagecache_page_t *dirty  = NULL;
        pc_lock(&pagecache.lock);
        for (pagecache_page_t *page = pagecache.lru_tail; page; page = page->lru_prev) {
            if ((page->mapping->flags & PAGECACHE_MAPPING_UNEVICTABLE) || __atomic_load_n(&page->mapping->pins, __ATOMIC_ACQUIRE)) continue;
            if (__atomic_load_n(&page->references, __ATOMIC_ACQUIRE)) continue;
            if (!pc_trylock(&page->lock)) continue;
            if (page->flags & (PC_PAGE_WRITEBACK | PC_PAGE_EVICTING)) {
                pc_unlock(&page->lock);
                continue;
            }
            /* A lookup can acquire a reference between the unlocked test
             * above and this page lock.  Publish EVICTING first so no new
             * lookup can succeed, then recheck the references acquired by
             * lookups which won that race. */
            __atomic_fetch_or(&page->flags, PC_PAGE_EVICTING, __ATOMIC_RELEASE);
            if (__atomic_load_n(&page->references, __ATOMIC_ACQUIRE)) {
                __atomic_fetch_and(&page->flags, ~PC_PAGE_EVICTING, __ATOMIC_RELEASE);
                pc_unlock(&page->lock);
                continue;
            }
            if (page->flags & PC_PAGE_DIRTY) {
                __atomic_fetch_and(&page->flags, ~PC_PAGE_EVICTING, __ATOMIC_RELEASE);
                if (!dirty)
                    dirty = page;
                else
                    pc_unlock(&page->lock);
                continue;
            }
            victim = page;
            break;
        }
        pc_unlock(&pagecache.lock);
        if (!victim && dirty) {
            int result = pc_writeback_page_locked(dirty);
            pc_unlock(&dirty->lock);
            if (result) break;
            continue;
        }
        if (dirty) pc_unlock(&dirty->lock);
        if (!victim) break;
        if (pc_unlink_page(victim)) {
            victim->flags &= ~PC_PAGE_EVICTING;
            pc_unlock(&victim->lock);
            continue;
        }
        pc_unlock(&victim->lock);
        pc_free_page(victim);
        reclaimed++;
        pc_stat_inc(&pagecache.stats.reclaimed);
    }
    return reclaimed;
}

void pagecache_get_stats(pagecache_stats_t *stats)
{
    if (!stats) return;
    stats->pages            = __atomic_load_n(&pagecache.stats.pages, __ATOMIC_RELAXED);
    stats->dirty            = __atomic_load_n(&pagecache.stats.dirty, __ATOMIC_RELAXED);
    stats->writeback        = __atomic_load_n(&pagecache.stats.writeback, __ATOMIC_RELAXED);
    stats->active           = __atomic_load_n(&pagecache.stats.active, __ATOMIC_RELAXED);
    stats->inactive         = __atomic_load_n(&pagecache.stats.inactive, __ATOMIC_RELAXED);
    stats->hits             = __atomic_load_n(&pagecache.stats.hits, __ATOMIC_RELAXED);
    stats->misses           = __atomic_load_n(&pagecache.stats.misses, __ATOMIC_RELAXED);
    stats->reads            = __atomic_load_n(&pagecache.stats.reads, __ATOMIC_RELAXED);
    stats->writes           = __atomic_load_n(&pagecache.stats.writes, __ATOMIC_RELAXED);
    stats->reclaimed        = __atomic_load_n(&pagecache.stats.reclaimed, __ATOMIC_RELAXED);
    stats->writeback_errors = __atomic_load_n(&pagecache.stats.writeback_errors, __ATOMIC_RELAXED);
    stats->readahead_pages  = __atomic_load_n(&pagecache.stats.readahead_pages, __ATOMIC_RELAXED);
    stats->readahead_hits   = __atomic_load_n(&pagecache.stats.readahead_hits, __ATOMIC_RELAXED);
    stats->clean_evicted    = __atomic_load_n(&pagecache.stats.clean_evicted, __ATOMIC_RELAXED);
    stats->dirty_evicted    = __atomic_load_n(&pagecache.stats.dirty_evicted, __ATOMIC_RELAXED);
}
