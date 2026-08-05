/*
 *
 *      alloc.c
 *      Kernel slab allocator backed by a page-granularity binary buddy.
 *
 *      2026/8/1 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/page.h>
#include <mem/slab.h>
#include <sync/spin_lock.h>

#define HEAP_MIN_ALIGNMENT 16U
#define SLAB_MAX_ORDER     8U
#define SLAB_NAME_LENGTH   32U
#define SLAB_MAGIC         0x534c414255494e58ULL
#define SLAB_OBJECT_MAGIC  0x4f424a55494e5844ULL
#define LARGE_MAGIC        0x4c41524755494e58ULL
#define OBJECT_FREE        0U
#define OBJECT_ALLOCATED   1U
#define LARGE_ALLOCATED    1U
#define SLAB_LIST_NONE     0U
#define SLAB_LIST_PARTIAL  1U
#define SLAB_LIST_FULL     2U
#define SLAB_LIST_EMPTY    3U
#define FREE_POISON        0x6b

typedef struct slab_header slab_header_t;

typedef struct slab_object_header {
        uint64_t                   magic;
        slab_header_t             *slab;
        struct slab_object_header *next_free;
        size_t                     requested;
        uint32_t                   state;
        uint32_t                   cookie;
} slab_object_header_t;

struct slab_header {
        uint64_t              magic;
        uint64_t              cookie;
        slab_cache_t         *cache;
        slab_header_t        *previous;
        slab_header_t        *next;
        slab_object_header_t *free_list;
        size_t                page_index;
        uint32_t              object_count;
        uint32_t              inuse;
        uint16_t              list;
        uint8_t               order;
        uint8_t               reserved;
        uintptr_t             object_start;
};

struct slab_cache {
        spinlock_t     lock;
        char           name[SLAB_NAME_LENGTH];
        size_t         object_size;
        size_t         alignment;
        size_t         payload_offset;
        size_t         stride;
        unsigned       slab_order;
        size_t         color_next;
        slab_header_t *partial;
        slab_header_t *full;
        slab_header_t *empty;
        size_t         slab_count;
        size_t         partial_count;
        size_t         full_count;
        size_t         empty_count;
        size_t         objects;
        size_t         allocations;
        size_t         frees;
        slab_ctor_t    ctor;
        slab_dtor_t    dtor;
        uint8_t        dynamic;
        uint8_t        destroying;
};

typedef struct {
        uint64_t magic;
        uint64_t cookie;
        void    *user;
        size_t   requested;
        size_t   usable;
        size_t   page_index;
        size_t   alignment;
        uint8_t  order;
        uint8_t  state;
        uint16_t reserved16;
        uint32_t reserved32;
} large_header_t;

typedef struct {
        uint8_t          *base;
        size_t            size;
        size_t            page_count;
        size_t            metadata_pages;
        buddy_allocator_t pages;
        spinlock_t        page_lock;
        uintptr_t         cookie;
        volatile uint8_t  online;
        error_handler     onerror;
        size_t            live_allocations;
        size_t            allocated_bytes;
        size_t            allocation_calls;
        size_t            free_calls;
        size_t            failed_allocations;
} heap_state_t;

static heap_state_t heap;

static const size_t size_classes[] = {16, 32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192};
#define SIZE_CACHE_COUNT (sizeof(size_classes) / sizeof(size_classes[0]))
static slab_cache_t size_caches[SIZE_CACHE_COUNT];

static size_t align_up_size(size_t value, size_t alignment)
{
    if (value > SIZE_MAX - (alignment - 1)) return SIZE_MAX;
    return (value + alignment - 1) & ~(alignment - 1);
}

static int valid_alignment(size_t alignment)
{
    return alignment && !(alignment & (alignment - 1));
}

static uint32_t object_cookie(const slab_object_header_t *object)
{
    return (uint32_t)(((uintptr_t)object >> 4) ^ heap.cookie ^ 0xa55a31c7U);
}

static uint64_t slab_cookie(const slab_header_t *slab)
{
    return ((uint64_t)(uintptr_t)slab ^ (uint64_t)heap.cookie ^ 0x91e10da5c79e7b1dULL);
}

static uint64_t large_cookie(const large_header_t *header)
{
    return ((uint64_t)(uintptr_t)header ^ (uint64_t)heap.cookie ^ 0xd6e8feb86659fd93ULL);
}

static void report_error(heap_error_t error, void *pointer)
{
    error_handler handler = heap.onerror;
    if (handler) handler(error, pointer);
}

static void stat_add(size_t *value, size_t amount)
{
    __atomic_add_fetch(value, amount, __ATOMIC_RELAXED);
}

static void stat_sub(size_t *value, size_t amount)
{
    __atomic_sub_fetch(value, amount, __ATOMIC_RELAXED);
}

static unsigned heap_max_order(size_t pages)
{
    unsigned order = buddy_order_for_units(pages);
    if (order > BUDDY_MAX_ORDER || ((size_t)1 << order) > pages) order--;
    return order;
}

static void *page_address(size_t index)
{
    return heap.base + index * PAGE_4K_SIZE;
}

/* Tag allocated pages so pointer_owner() can locate the owning block. */
static size_t page_alloc(unsigned order)
{
    uint64_t rflags = spin_lock_irqsave(&heap.page_lock);
    size_t   index  = buddy_alloc(&heap.pages, order);
    if (index != SIZE_MAX) {
        size_t pages = (size_t)1 << order;
        for (size_t i = 0; i < pages; i++) heap.pages.pages[index + i].tag = (uint32_t)(index + 1);
    }
    spin_unlock_irqrestore(&heap.page_lock, rflags);
    return index;
}

static int page_free(size_t index, unsigned order)
{
    uint64_t rflags = spin_lock_irqsave(&heap.page_lock);
    int      result = buddy_free(&heap.pages, index, order);
    spin_unlock_irqrestore(&heap.page_lock, rflags);
    return result;
}

static void list_insert(slab_cache_t *cache, slab_header_t *slab, unsigned list)
{
    slab_header_t **head;
    size_t         *count;
    if (list == SLAB_LIST_PARTIAL) {
        head  = &cache->partial;
        count = &cache->partial_count;
    } else if (list == SLAB_LIST_FULL) {
        head  = &cache->full;
        count = &cache->full_count;
    } else {
        head  = &cache->empty;
        count = &cache->empty_count;
    }

    slab->previous = NULL;
    slab->next     = *head;
    slab->list     = (uint16_t)list;
    if (*head) (*head)->previous = slab;
    *head = slab;
    (*count)++;
}

static void list_remove(slab_cache_t *cache, slab_header_t *slab)
{
    slab_header_t **head;
    size_t         *count;
    if (slab->list == SLAB_LIST_PARTIAL) {
        head  = &cache->partial;
        count = &cache->partial_count;
    } else if (slab->list == SLAB_LIST_FULL) {
        head  = &cache->full;
        count = &cache->full_count;
    } else if (slab->list == SLAB_LIST_EMPTY) {
        head  = &cache->empty;
        count = &cache->empty_count;
    } else {
        return;
    }

    if (slab->previous)
        slab->previous->next = slab->next;
    else
        *head = slab->next;
    if (slab->next) slab->next->previous = slab->previous;
    slab->previous = slab->next = NULL;
    slab->list                  = SLAB_LIST_NONE;
    (*count)--;
}

/* Choose the slab order that wastes the least space per slab. */
static int cache_layout(slab_cache_t *cache)
{
    cache->payload_offset = align_up_size(sizeof(slab_object_header_t), cache->alignment);
    if (cache->payload_offset == SIZE_MAX || cache->object_size > SIZE_MAX - cache->payload_offset) return -1;
    cache->stride = align_up_size(cache->payload_offset + cache->object_size, cache->alignment);
    if (cache->stride == SIZE_MAX) return -1;

    unsigned best_order = BUDDY_MAX_ORDER + 1;
    size_t   best_waste = SIZE_MAX;
    for (unsigned order = 0; order <= SLAB_MAX_ORDER; order++) {
        size_t bytes = PAGE_4K_SIZE << order;
        size_t base  = align_up_size(sizeof(slab_header_t), cache->alignment);
        if (base >= bytes) continue;
        size_t objects = (bytes - base) / cache->stride;
        if (!objects) continue;
        size_t waste = bytes - base - objects * cache->stride;
        if (waste < best_waste) {
            best_waste = waste;
            best_order = order;
        }
        if (objects >= 8 && waste <= bytes / 8) {
            best_order = order;
            break;
        }
    }
    if (best_order > SLAB_MAX_ORDER) return -1;
    cache->slab_order = best_order;
    return 0;
}

static int cache_init(slab_cache_t *cache, const char *name, size_t size, size_t alignment, slab_ctor_t ctor, slab_dtor_t dtor, int dynamic)
{
    if (!cache || !size || !valid_alignment(alignment) || alignment < sizeof(void *) || alignment > PAGE_4K_SIZE) return -1;
    memset(cache, 0, sizeof(*cache));
    cache->object_size = size;
    cache->alignment   = alignment < HEAP_MIN_ALIGNMENT ? HEAP_MIN_ALIGNMENT : alignment;
    cache->ctor        = ctor;
    cache->dtor        = dtor;
    cache->dynamic     = dynamic ? 1 : 0;
    if (name) {
        size_t i = 0;
        while (i + 1 < sizeof(cache->name) && name[i]) {
            cache->name[i] = name[i];
            i++;
        }
        cache->name[i] = '\0';
    }
    return cache_layout(cache);
}

/* Allocate a new slab and carve it into free objects. */
static slab_header_t *slab_create_locked(slab_cache_t *cache)
{
    size_t page_index = page_alloc(cache->slab_order);
    if (page_index == SIZE_MAX) return NULL;

    slab_header_t *slab         = page_address(page_index);
    size_t         bytes        = PAGE_4K_SIZE << cache->slab_order;
    size_t         base_offset  = align_up_size(sizeof(*slab), cache->alignment);
    size_t         object_count = (bytes - base_offset) / cache->stride;
    size_t         leftover     = bytes - base_offset - object_count * cache->stride;
    size_t         colors       = leftover / cache->alignment + 1;
    size_t         color        = (cache->color_next++ % colors) * cache->alignment;

    memset(slab, 0, sizeof(*slab));
    slab->magic        = SLAB_MAGIC;
    slab->cookie       = slab_cookie(slab);
    slab->cache        = cache;
    slab->page_index   = page_index;
    slab->object_count = (uint32_t)object_count;
    slab->order        = (uint8_t)cache->slab_order;
    slab->object_start = (uintptr_t)slab + base_offset + color;

    for (size_t i = object_count; i > 0; i--) {
        slab_object_header_t *object = (void *)(slab->object_start + (i - 1) * cache->stride);
        object->magic                = SLAB_OBJECT_MAGIC;
        object->slab                 = slab;
        object->requested            = 0;
        object->state                = OBJECT_FREE;
        object->cookie               = object_cookie(object);
        object->next_free            = slab->free_list;
        slab->free_list              = object;
        memset((uint8_t *)object + cache->payload_offset, FREE_POISON, cache->object_size);
    }

    cache->slab_count++;
    list_insert(cache, slab, SLAB_LIST_EMPTY);
    return slab;
}

/* Return a slab's pages to the buddy allocator. */
static void slab_release_locked(slab_cache_t *cache, slab_header_t *slab)
{
    size_t   page_index = slab->page_index;
    unsigned order      = slab->order;
    list_remove(cache, slab);
    cache->slab_count--;
    slab->magic = 0;
    (void)page_free(page_index, order);
}

/* Take one object from a cache, growing a new slab when empty. */
static void *cache_alloc_requested(slab_cache_t *cache, size_t requested)
{
    if (!cache || cache->destroying) return NULL;
    uint64_t rflags = spin_lock_irqsave(&cache->lock);

    slab_header_t *slab = cache->partial ? cache->partial : cache->empty;
    if (!slab) slab = slab_create_locked(cache);
    if (!slab || !slab->free_list) {
        spin_unlock_irqrestore(&cache->lock, rflags);
        return NULL;
    }

    list_remove(cache, slab);
    slab_object_header_t *object = slab->free_list;
    slab->free_list              = object->next_free;
    object->next_free            = NULL;
    object->requested            = requested;
    object->state                = OBJECT_ALLOCATED;
    slab->inuse++;
    cache->objects++;
    cache->allocations++;
    list_insert(cache, slab, slab->inuse == slab->object_count ? SLAB_LIST_FULL : SLAB_LIST_PARTIAL);

    void *result = (uint8_t *)object + cache->payload_offset;
    if (cache->ctor) cache->ctor(result);
    spin_unlock_irqrestore(&cache->lock, rflags);
    return result;
}

/* Put an object back on its slab and update the cache bookkeeping. */
static int cache_free_object(slab_cache_t *expected, slab_header_t *slab, void *pointer, size_t *released)
{
    slab_cache_t *cache = slab ? slab->cache : NULL;
    if (!cache || (expected && expected != cache)) return -1;

    uint64_t rflags = spin_lock_irqsave(&cache->lock);
    if (slab->magic != SLAB_MAGIC || slab->cookie != slab_cookie(slab) || slab->cache != cache || slab->list == SLAB_LIST_NONE) {
        spin_unlock_irqrestore(&cache->lock, rflags);
        return -2;
    }

    uintptr_t payload_start = slab->object_start + cache->payload_offset;
    uintptr_t address       = (uintptr_t)pointer;
    if (address < payload_start || (address - payload_start) % cache->stride
        || (address - payload_start) / cache->stride >= slab->object_count) {
        spin_unlock_irqrestore(&cache->lock, rflags);
        return -1;
    }

    slab_object_header_t *object = (void *)(address - cache->payload_offset);
    if (object->magic != SLAB_OBJECT_MAGIC || object->slab != slab || object->cookie != object_cookie(object)) {
        spin_unlock_irqrestore(&cache->lock, rflags);
        return -2;
    }
    if (object->state != OBJECT_ALLOCATED) {
        spin_unlock_irqrestore(&cache->lock, rflags);
        return -1;
    }
    if (!object->requested || object->requested > cache->object_size) {
        spin_unlock_irqrestore(&cache->lock, rflags);
        return -2;
    }

    size_t requested = object->requested;
    if (cache->dtor) cache->dtor(pointer);
    memset(pointer, FREE_POISON, cache->object_size);
    list_remove(cache, slab);
    object->requested = 0;
    object->state     = OBJECT_FREE;
    object->next_free = slab->free_list;
    slab->free_list   = object;
    slab->inuse--;
    cache->objects--;
    cache->frees++;
    list_insert(cache, slab, slab->inuse ? SLAB_LIST_PARTIAL : SLAB_LIST_EMPTY);

    /* Retain one warm empty slab per cache and reclaim surplus immediately. */
    if (!slab->inuse && cache->empty_count > 1) slab_release_locked(cache, slab);
    spin_unlock_irqrestore(&cache->lock, rflags);
    if (released) *released = requested;
    return 0;
}

/* Find the smallest size class that fits size. */
static slab_cache_t *cache_for_size(size_t size)
{
    for (size_t i = 0; i < SIZE_CACHE_COUNT; i++) {
        if (size <= size_classes[i]) return &size_caches[i];
    }
    return NULL;
}

/* Resolve a heap pointer to the page index and owner of its block. */
static int pointer_owner(void *pointer, size_t *owner_index, void **owner)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t base    = (uintptr_t)heap.base;
    if (!heap.online || address < base || address >= base + heap.size) return -1;
    size_t page = (address - base) / PAGE_4K_SIZE;

    uint64_t rflags = spin_lock_irqsave(&heap.page_lock);
    uint32_t tag    = heap.pages.pages[page].tag;
    if (!tag || tag > heap.page_count) {
        spin_unlock_irqrestore(&heap.page_lock, rflags);
        return -1;
    }
    *owner_index = (size_t)tag - 1;
    *owner       = page_address(*owner_index);
    spin_unlock_irqrestore(&heap.page_lock, rflags);
    return 0;
}

/* Allocate a page-multiple block with an embedded large header. */
static void *large_alloc(size_t alignment, size_t size)
{
    if (alignment > heap.size || size > SIZE_MAX - sizeof(large_header_t) - alignment) return NULL;
    size_t   needed = sizeof(large_header_t) + size + alignment - 1;
    size_t   pages  = (needed + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    unsigned order  = buddy_order_for_units(pages);
    if (order > heap.pages.max_order) return NULL;

    size_t page_index = page_alloc(order);
    if (page_index == SIZE_MAX) return NULL;
    large_header_t *header      = page_address(page_index);
    uintptr_t       user        = align_up_size((uintptr_t)header + sizeof(*header), alignment);
    size_t          block_bytes = PAGE_4K_SIZE << order;

    memset(header, 0, sizeof(*header));
    header->magic      = LARGE_MAGIC;
    header->user       = (void *)user;
    header->requested  = size;
    header->usable     = (uintptr_t)header + block_bytes - user;
    header->page_index = page_index;
    header->alignment  = alignment;
    header->order      = (uint8_t)order;
    header->state      = LARGE_ALLOCATED;
    header->cookie     = large_cookie(header);
    return (void *)user;
}

/* Initialize the heap: buddy allocator plus size-class caches. */
int heap_init(uint8_t *address, size_t size)
{
    if (!address || ((uintptr_t)address & (PAGE_4K_SIZE - 1)) || size < PAGE_4K_SIZE * 16) return -1;
    size &= ~(PAGE_4K_SIZE - 1);
    size_t page_count = size / PAGE_4K_SIZE;
    if (page_count > 0x7fffffffU) return -1;
    size_t metadata_bytes = page_count * sizeof(buddy_page_t);
    size_t metadata_pages = (metadata_bytes + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (metadata_pages >= page_count) return -1;

    memset(&heap, 0, sizeof(heap));
    heap.base           = address;
    heap.size           = size;
    heap.page_count     = page_count;
    heap.metadata_pages = metadata_pages;
    heap.cookie         = (uintptr_t)address ^ size ^ 0x9e3779b97f4a7c15ULL;
    if (buddy_init(&heap.pages, (buddy_page_t *)address, page_count, heap_max_order(page_count))) return -1;
    if (buddy_add_range(&heap.pages, metadata_pages, page_count - metadata_pages)) return -1;

    for (size_t i = 0; i < SIZE_CACHE_COUNT; i++) {
        if (cache_init(&size_caches[i], "kmalloc", size_classes[i], HEAP_MIN_ALIGNMENT, NULL, NULL, 0)) return -1;
    }
    __atomic_store_n(&heap.online, 1, __ATOMIC_RELEASE);
    return 0;
}

void heap_onerror(error_handler handler)
{
    heap.onerror = handler;
}

/* Allocate size bytes from the fitting size class or large path. */
void *malloc(size_t size)
{
    if (!size || !__atomic_load_n(&heap.online, __ATOMIC_ACQUIRE)) return NULL;
    stat_add(&heap.allocation_calls, 1);

    slab_cache_t *cache  = cache_for_size(size);
    void         *result = cache ? cache_alloc_requested(cache, size) : large_alloc(HEAP_MIN_ALIGNMENT, size);
    if (!result) {
        stat_add(&heap.failed_allocations, 1);
        if (size > size_classes[SIZE_CACHE_COUNT - 1]) plogk("alloc: failed to allocate %llu bytes (heap limit reached)\n", (uint64_t)size);
        return NULL;
    }
    stat_add(&heap.live_allocations, 1);
    stat_add(&heap.allocated_bytes, size);
    return result;
}

/* Allocate with an explicit alignment, via the large path when needed. */
void *aligned_alloc(size_t alignment, size_t size)
{
    if (!size || !valid_alignment(alignment) || alignment < sizeof(void *) || !__atomic_load_n(&heap.online, __ATOMIC_ACQUIRE)) return NULL;
    if (alignment <= HEAP_MIN_ALIGNMENT) return malloc(size);

    stat_add(&heap.allocation_calls, 1);
    void *result = large_alloc(alignment, size);
    if (!result) {
        stat_add(&heap.failed_allocations, 1);
        if (size > size_classes[SIZE_CACHE_COUNT - 1])
            plogk("alloc: failed to allocate %llu bytes aligned to %llu (heap limit reached)\n", (uint64_t)size, (uint64_t)alignment);
        return NULL;
    }
    stat_add(&heap.live_allocations, 1);
    stat_add(&heap.allocated_bytes, size);
    return result;
}

/* Return the usable capacity of an allocation. */
size_t usable_size(void *pointer)
{
    if (!pointer) return 0;
    size_t owner_index;
    void  *owner;
    if (pointer_owner(pointer, &owner_index, &owner)) return 0;
    (void)owner_index;

    slab_header_t *slab = owner;
    if (slab->magic == SLAB_MAGIC && slab->cookie == slab_cookie(slab)) {
        slab_cache_t *cache         = slab->cache;
        uintptr_t     payload_start = slab->object_start + cache->payload_offset;
        uintptr_t     address       = (uintptr_t)pointer;
        if (address < payload_start || (address - payload_start) % cache->stride) return 0;
        slab_object_header_t *object = (void *)(address - cache->payload_offset);
        if (object->magic != SLAB_OBJECT_MAGIC || object->cookie != object_cookie(object) || object->state != OBJECT_ALLOCATED) return 0;
        return cache->object_size;
    }

    large_header_t *large = owner;
    if (large->magic == LARGE_MAGIC && large->cookie == large_cookie(large) && large->state == LARGE_ALLOCATED && large->user == pointer)
        return large->usable;
    return 0;
}

/* Return the size the caller originally requested. */
static size_t requested_size(void *pointer)
{
    size_t owner_index;
    void  *owner;
    if (pointer_owner(pointer, &owner_index, &owner)) return 0;
    (void)owner_index;
    slab_header_t *slab = owner;
    if (slab->magic == SLAB_MAGIC && slab->cookie == slab_cookie(slab)) {
        slab_cache_t *cache         = slab->cache;
        uintptr_t     payload_start = slab->object_start + cache->payload_offset;
        uintptr_t     address       = (uintptr_t)pointer;
        if (address < payload_start || (address - payload_start) % cache->stride
            || (address - payload_start) / cache->stride >= slab->object_count)
            return 0;
        slab_object_header_t *object = (void *)((uintptr_t)pointer - cache->payload_offset);
        if (object->magic == SLAB_OBJECT_MAGIC && object->cookie == object_cookie(object) && object->state == OBJECT_ALLOCATED)
            return object->requested <= cache->object_size ? object->requested : 0;
    }
    large_header_t *large = owner;
    if (large->magic == LARGE_MAGIC && large->cookie == large_cookie(large) && large->state == LARGE_ALLOCATED && large->user == pointer)
        return large->requested;
    return 0;
}

/* Release an allocation back to its slab or the buddy allocator. */
void free(void *pointer)
{
    if (!pointer) return;
    size_t owner_index;
    void  *owner;
    if (pointer_owner(pointer, &owner_index, &owner)) {
        plogk("alloc: invalid free of pointer 0x%016llx (not owned by the heap)\n", (uint64_t)(uintptr_t)pointer);
        report_error(invalid_free, pointer);
        return;
    }

    size_t         released = 0;
    slab_header_t *slab     = owner;
    if (slab->magic == SLAB_MAGIC) {
        int result = cache_free_object(NULL, slab, pointer, &released);
        if (result) {
            plogk("alloc: free of 0x%016llx rejected (slab integrity check failed, err=%d)\n", (uint64_t)(uintptr_t)pointer, result);
            report_error(result == -2 ? layout_error : invalid_free, pointer);
            return;
        }
    } else {
        large_header_t *large = owner;
        if (large->magic != LARGE_MAGIC || large->cookie != large_cookie(large) || large->state != LARGE_ALLOCATED || large->user != pointer
            || large->page_index != owner_index) {
            plogk("alloc: free of 0x%016llx rejected (large block header corrupt)\n", (uint64_t)(uintptr_t)pointer);
            report_error(invalid_free, pointer);
            return;
        }
        released       = large->requested;
        unsigned order = large->order;
        large->state   = 0;
        large->magic   = 0;
        if (page_free(owner_index, order)) {
            plogk("alloc: buddy release failed for 0x%016llx (order %u)\n", (uint64_t)(uintptr_t)pointer, order);
            report_error(layout_error, pointer);
            return;
        }
    }

    stat_sub(&heap.live_allocations, 1);
    stat_sub(&heap.allocated_bytes, released);
    stat_add(&heap.free_calls, 1);
}

/* Resize in place when capacity allows, otherwise move and copy. */
void *realloc(void *pointer, size_t new_size)
{
    if (!pointer) return malloc(new_size);
    if (!new_size) {
        free(pointer);
        return NULL;
    }

    size_t old_size = requested_size(pointer);
    size_t capacity = usable_size(pointer);
    if (!old_size || !capacity) {
        report_error(invalid_free, pointer);
        return NULL;
    }
    if (new_size <= capacity) {
        size_t owner_index;
        void  *owner;
        if (pointer_owner(pointer, &owner_index, &owner)) return NULL;
        (void)owner_index;
        slab_header_t *slab = owner;
        if (slab->magic == SLAB_MAGIC) {
            slab_object_header_t *object = (void *)((uintptr_t)pointer - slab->cache->payload_offset);
            object->requested            = new_size;
        } else {
            ((large_header_t *)owner)->requested = new_size;
        }
        if (new_size > old_size)
            stat_add(&heap.allocated_bytes, new_size - old_size);
        else
            stat_sub(&heap.allocated_bytes, old_size - new_size);
        return pointer;
    }

    void *replacement = malloc(new_size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, old_size < new_size ? old_size : new_size);
    free(pointer);
    return replacement;
}

/* Create a dynamic cache for fixed-size objects. */
slab_cache_t *slab_cache_create(const char *name, size_t object_size, size_t alignment, slab_ctor_t ctor, slab_dtor_t dtor)
{
    if (!heap.online) return NULL;
    slab_cache_t *cache = malloc(sizeof(*cache));
    if (!cache) return NULL;
    if (cache_init(cache, name, object_size, alignment, ctor, dtor, 1)) {
        free(cache);
        return NULL;
    }
    return cache;
}

void *slab_cache_alloc(slab_cache_t *cache)
{
    return cache_alloc_requested(cache, cache ? cache->object_size : 0);
}

int slab_cache_free(slab_cache_t *cache, void *object)
{
    if (!cache || !object) return -1;
    size_t owner_index;
    void  *owner;
    if (pointer_owner(object, &owner_index, &owner)) return -1;
    (void)owner_index;
    return cache_free_object(cache, (slab_header_t *)owner, object, NULL);
}

/* Return all empty slabs to the buddy; reports the pages released. */
size_t slab_cache_shrink(slab_cache_t *cache)
{
    if (!cache) return 0;
    uint64_t rflags = spin_lock_irqsave(&cache->lock);
    size_t   pages  = 0;
    while (cache->empty) {
        slab_header_t *slab = cache->empty;
        pages += (size_t)1 << slab->order;
        slab_release_locked(cache, slab);
    }
    spin_unlock_irqrestore(&cache->lock, rflags);
    return pages;
}

/* Tear down a dynamic cache once it holds no objects. */
int slab_cache_destroy(slab_cache_t *cache)
{
    if (!cache || !cache->dynamic) return -1;
    uint64_t rflags = spin_lock_irqsave(&cache->lock);
    if (cache->objects) {
        spin_unlock_irqrestore(&cache->lock, rflags);
        return -1;
    }
    cache->destroying = 1;
    while (cache->empty) slab_release_locked(cache, cache->empty);
    spin_unlock_irqrestore(&cache->lock, rflags);
    free(cache);
    return 0;
}

void slab_cache_get_stats(slab_cache_t *cache, slab_cache_stats_t *stats)
{
    if (!cache || !stats) return;
    uint64_t rflags      = spin_lock_irqsave(&cache->lock);
    stats->object_size   = cache->object_size;
    stats->alignment     = cache->alignment;
    stats->objects       = cache->objects;
    stats->slabs         = cache->slab_count;
    stats->partial_slabs = cache->partial_count;
    stats->full_slabs    = cache->full_count;
    stats->empty_slabs   = cache->empty_count;
    stats->allocations   = cache->allocations;
    stats->frees         = cache->frees;
    spin_unlock_irqrestore(&cache->lock, rflags);
}

void heap_get_stats(heap_stats_t *stats)
{
    if (!stats) return;
    uint64_t rflags        = spin_lock_irqsave(&heap.page_lock);
    stats->arena_bytes     = heap.size;
    stats->metadata_bytes  = heap.metadata_pages * PAGE_4K_SIZE;
    stats->free_page_bytes = heap.pages.free_pages * PAGE_4K_SIZE;
    spin_unlock_irqrestore(&heap.page_lock, rflags);
    stats->live_allocations   = __atomic_load_n(&heap.live_allocations, __ATOMIC_RELAXED);
    stats->allocated_bytes    = __atomic_load_n(&heap.allocated_bytes, __ATOMIC_RELAXED);
    stats->allocation_calls   = __atomic_load_n(&heap.allocation_calls, __ATOMIC_RELAXED);
    stats->free_calls         = __atomic_load_n(&heap.free_calls, __ATOMIC_RELAXED);
    stats->failed_allocations = __atomic_load_n(&heap.failed_allocations, __ATOMIC_RELAXED);
}

static int validate_list(slab_cache_t *cache, slab_header_t *head, unsigned expected_list, size_t expected_count)
{
    size_t         count    = 0;
    slab_header_t *previous = NULL;
    for (slab_header_t *slab = head; slab; slab = slab->next) {
        if (count++ > cache->slab_count || slab->previous != previous || slab->list != expected_list || slab->cache != cache
            || slab->magic != SLAB_MAGIC || slab->cookie != slab_cookie(slab))
            return -1;
        if (expected_list == SLAB_LIST_EMPTY && slab->inuse != 0) return -1;
        if (expected_list == SLAB_LIST_FULL && slab->inuse != slab->object_count) return -1;
        if (expected_list == SLAB_LIST_PARTIAL && (!slab->inuse || slab->inuse == slab->object_count)) return -1;
        previous = slab;
    }
    return count == expected_count ? 0 : -1;
}

/* Check buddy and size-cache invariants; for debugging. */
int heap_validate(void)
{
    if (!heap.online) return -1;
    uint64_t rflags = spin_lock_irqsave(&heap.page_lock);
    int      result = buddy_validate(&heap.pages);
    spin_unlock_irqrestore(&heap.page_lock, rflags);
    if (result) return result;

    for (size_t i = 0; i < SIZE_CACHE_COUNT; i++) {
        slab_cache_t *cache = &size_caches[i];
        rflags              = spin_lock_irqsave(&cache->lock);
        result              = validate_list(cache, cache->partial, SLAB_LIST_PARTIAL, cache->partial_count)
                 || validate_list(cache, cache->full, SLAB_LIST_FULL, cache->full_count)
                 || validate_list(cache, cache->empty, SLAB_LIST_EMPTY, cache->empty_count)
                 || cache->slab_count != cache->partial_count + cache->full_count + cache->empty_count;
        spin_unlock_irqrestore(&cache->lock, rflags);
        if (result) return -1;
    }
    return 0;
}
