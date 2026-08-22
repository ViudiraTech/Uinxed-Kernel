/*
 *
 *      frame.c
 *      Memory frame
 *
 *      2025/2/16 By XIAOYI12
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/smp.h>
#include <boot/limine.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/buddy.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/pagecache.h>
#include <mem/swap.h>
#include <process/sched.h>

log_buffer_t             frame_log;
frame_allocator_t        frame_allocator;
uint64_t                 memory_size = 0;
static volatile int      frame_reclaim_active;
static volatile uint32_t frame_reclaim_backoff;

/*
 * Linux serves order-0 allocations from per-CPU pagesets before touching the
 * zone buddy.  Keep the same shape here: a modest cache absorbs page faults,
 * page-cache churn and GEM allocations without bouncing one global lock.
 */
#define FRAME_PCP_MAX_CPUS 256U
#define FRAME_PCP_HIGH     64U
#define FRAME_PCP_BATCH    16U
#define FRAME_RECLAIM_BATCH 16U

typedef struct {
        spinlock_t lock;
        uint32_t   count;
        size_t     frames[FRAME_PCP_HIGH];
} frame_pcp_t;

static frame_pcp_t frame_pcp[FRAME_PCP_MAX_CPUS];

/* Early boot allocations all belong to CPU 0; topology is not safe yet. */
static uint32_t frame_pcp_cpu(void)
{
    if (!__atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE)) return 0;
    uint32_t cpu = get_current_cpu_id();
    return cpu < FRAME_PCP_MAX_CPUS ? cpu : FRAME_PCP_MAX_CPUS;
}

/* Return cached order-0 pages to the buddy without changing logical free RAM. */
static void frame_pcp_return_to_buddy(const size_t *frames, size_t count)
{
    if (!count) return;
    spin_lock(&frame_allocator.lock);
    for (size_t i = 0; i < count; i++) {
        if (buddy_free(&frame_allocator.buddy, frames[i], 0))
            plogk("frame: PCP drain failed for 0x%016llx\n", (uint64_t)frames[i] * PAGE_4K_SIZE);
    }
    spin_unlock(&frame_allocator.lock);
}

/* Drain one CPU cache, used to satisfy fragmented contiguous allocations. */
static void frame_pcp_drain_cpu(uint32_t cpu)
{
    size_t frames[FRAME_PCP_HIGH];
    size_t count;

    uint64_t rflags = spin_lock_irqsave(&frame_pcp[cpu].lock);
    count           = frame_pcp[cpu].count;
    for (size_t i = 0; i < count; i++) frames[i] = frame_pcp[cpu].frames[i];
    frame_pcp[cpu].count = 0;
    spin_unlock_irqrestore(&frame_pcp[cpu].lock, rflags);
    frame_pcp_return_to_buddy(frames, count);
}

static void frame_pcp_drain_all(void)
{
    for (uint32_t cpu = 0; cpu < FRAME_PCP_MAX_CPUS; cpu++) frame_pcp_drain_cpu(cpu);
}

/* Put one logically-free order-0 page into the local cache. */
static void frame_pcp_put(size_t frame)
{
    uint32_t cpu = frame_pcp_cpu();
    if (cpu >= FRAME_PCP_MAX_CPUS) {
        frame_pcp_return_to_buddy(&frame, 1);
        return;
    }

    size_t   drain[FRAME_PCP_BATCH];
    size_t   drain_count = 0;
    uint64_t rflags      = spin_lock_irqsave(&frame_pcp[cpu].lock);
    if (frame_pcp[cpu].count == FRAME_PCP_HIGH) {
        drain_count = FRAME_PCP_BATCH;
        for (size_t i = 0; i < drain_count; i++) drain[i] = frame_pcp[cpu].frames[--frame_pcp[cpu].count];
    }
    frame_pcp[cpu].frames[frame_pcp[cpu].count++] = frame;
    spin_unlock_irqrestore(&frame_pcp[cpu].lock, rflags);
    frame_pcp_return_to_buddy(drain, drain_count);
}

/* Pop one local page and make it externally owned. */
static size_t frame_pcp_pop(void)
{
    uint32_t cpu = frame_pcp_cpu();
    if (cpu >= FRAME_PCP_MAX_CPUS) return SIZE_MAX;

    uint64_t rflags = spin_lock_irqsave(&frame_pcp[cpu].lock);
    if (!frame_pcp[cpu].count) {
        spin_unlock_irqrestore(&frame_pcp[cpu].lock, rflags);
        return SIZE_MAX;
    }
    size_t frame = frame_pcp[cpu].frames[--frame_pcp[cpu].count];
    frame_allocator.buddy.pages[frame].tag = 1;
    spin_unlock_irqrestore(&frame_pcp[cpu].lock, rflags);
    __atomic_sub_fetch(&frame_allocator.usable_frames, 1, __ATOMIC_RELAXED);
    return frame;
}

/* Refill a local cache from one buddy block and return its first page. */
static size_t frame_pcp_refill(void)
{
    size_t   first = SIZE_MAX;
    size_t   units = 0;
    unsigned order = 4; /* 16 pages, matching FRAME_PCP_BATCH. */

    spin_lock(&frame_allocator.lock);
    while (1) {
        first = buddy_alloc(&frame_allocator.buddy, order);
        if (first != SIZE_MAX || order == 0) break;
        order--;
    }
    if (first != SIZE_MAX) {
        units = (size_t)1 << order;
        if (buddy_trim_allocation(&frame_allocator.buddy, first, order, units)) {
            (void)buddy_free(&frame_allocator.buddy, first, order);
            first = SIZE_MAX;
            units = 0;
        } else {
            for (size_t i = 0; i < units; i++) frame_allocator.buddy.pages[first + i].tag = 0;
            frame_allocator.buddy.pages[first].tag = 1;
        }
    }
    spin_unlock(&frame_allocator.lock);

    if (first == SIZE_MAX) return SIZE_MAX;
    __atomic_sub_fetch(&frame_allocator.usable_frames, 1, __ATOMIC_RELAXED);
    for (size_t i = 1; i < units; i++) frame_pcp_put(first + i);
    return first;
}

/* Serialize reclaim so swap I/O cannot recursively enter reclaim allocation. */
static int frame_try_reclaim(size_t target)
{
    int expected = 0;
    if (!__atomic_compare_exchange_n(&frame_reclaim_active, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 0;
    int reclaimed = swap_reclaim(target);
    __atomic_store_n(&frame_reclaim_active, 0, __ATOMIC_RELEASE);
    return reclaimed;
}

int frame_reclaim_pages(size_t target)
{
    return target ? frame_try_reclaim(target) : 0;
}

/* Start reclaim at a low watermark from a VM-safe allocation boundary. */
void frame_reclaim_if_needed(size_t requested)
{
    size_t total = frame_allocator.origin_frames;
    size_t free  = __atomic_load_n(&frame_allocator.usable_frames, __ATOMIC_RELAXED);
    size_t low   = total / 32;
    size_t high  = total / 16;
    if (low < 128) low = 128;
    if (high < low + 64) high = low + 64;
    if (free > low) return;

    /*
     * Clean file-backed cache is cheaper to recover than anonymous memory.
     * In particular, parallel compilers can otherwise exhaust RAM while the
     * source/object working set remains reclaimable in the page cache.  This
     * path is also useful on systems without an active swap area.
     */
    size_t cache_target = high > free ? high - free : requested;
    if (cache_target < requested) cache_target = requested;
    if (cache_target > FRAME_RECLAIM_BATCH) cache_target = FRAME_RECLAIM_BATCH;
    (void)pagecache_reclaim(cache_target);

    free = __atomic_load_n(&frame_allocator.usable_frames, __ATOMIC_RELAXED);
    if (free > low || !swap_has_free_space()) return;

    uint32_t backoff = __atomic_load_n(&frame_reclaim_backoff, __ATOMIC_RELAXED);
    bool     urgent  = free <= requested;
    if (backoff && !urgent) {
        (void)__atomic_compare_exchange_n(&frame_reclaim_backoff, &backoff, backoff - 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        return;
    }

    size_t target = high > free ? high - free : requested;
    if (target < requested) target = requested;
    if (target > FRAME_RECLAIM_BATCH) target = FRAME_RECLAIM_BATCH;
    if (frame_try_reclaim(target) == 0)
        __atomic_store_n(&frame_reclaim_backoff, 64, __ATOMIC_RELAXED);
    else
        __atomic_store_n(&frame_reclaim_backoff, 0, __ATOMIC_RELAXED);
}

/* Initialize memory frame */
void init_frame(void)
{
    struct limine_memmap_response *memory_map = memmap_request.response;
    if (!memory_map) krn_halt();

    frame_allocator.lock.lock   = 0;
    frame_allocator.lock.rflags = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type == LIMINE_MEMMAP_USABLE) {
            uint64_t region_end = region->base + region->length;
            if (region_end > memory_size) memory_size = region_end;
        }
    }
    log_buffer_write(&frame_log, "frame: Highest usable address is %p\n", memory_size);
    size_t   frame_count      = ALIGN_UP(memory_size, PAGE_4K_SIZE) / PAGE_4K_SIZE;
    size_t   metadata_size    = ALIGN_UP(frame_count * sizeof(buddy_page_t), PAGE_4K_SIZE);
    uint64_t metadata_address = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t region_start = ALIGN_UP(MAX(region->base, 0x100000ULL), PAGE_4K_SIZE);
        uint64_t region_end   = ALIGN_DOWN(region->base + region->length, PAGE_4K_SIZE);
        if (region_start >= region_end || region_end - region_start < metadata_size) continue;

        metadata_address = ALIGN_DOWN(region_end - metadata_size, PAGE_4K_SIZE);
        break;
    }
    if (metadata_address) {
        log_buffer_write(&frame_log, "frame: Ownership metadata allocated at %p (size: %llu KiB)\n", metadata_address, metadata_size / 1024);
    } else {
        log_buffer_write(&frame_log, "frame: Failed to allocate ownership metadata.\n");
        return;
    }
    unsigned max_order = buddy_order_for_units(frame_count);
    if (max_order > BUDDY_MAX_ORDER || ((size_t)1 << max_order) > frame_count) max_order--;
    if (buddy_init(&frame_allocator.buddy, phys_to_virt(metadata_address), frame_count, max_order)) {
        log_buffer_write(&frame_log, "frame: Failed to initialise buddy metadata.\n");
        return;
    }
    frame_allocator.frame_count = frame_count;
    size_t origin_frames        = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start = ALIGN_UP(region->base, PAGE_4K_SIZE);
            uint64_t end   = ALIGN_DOWN(region->base + region->length, PAGE_4K_SIZE);
            if (start >= end) continue;
            size_t start_frame = start / PAGE_4K_SIZE;
            size_t count       = (end - start) / PAGE_4K_SIZE;
            origin_frames += count;

            /* Physical address zero is the public allocation failure value. */
            if (start_frame == 0) {
                start_frame++;
                count--;
            }
            if (!count) continue;

            size_t metadata_start = metadata_address / PAGE_4K_SIZE;
            size_t metadata_count = metadata_size / PAGE_4K_SIZE;
            size_t range_end      = start_frame + count;
            if (metadata_start >= range_end || metadata_start + metadata_count <= start_frame) {
                (void)buddy_add_range(&frame_allocator.buddy, start_frame, count);
            } else {
                if (metadata_start > start_frame) (void)buddy_add_range(&frame_allocator.buddy, start_frame, metadata_start - start_frame);
                size_t after_metadata = metadata_start + metadata_count;
                if (after_metadata < range_end) (void)buddy_add_range(&frame_allocator.buddy, after_metadata, range_end - after_metadata);
            }
            log_buffer_write(&frame_log, "frame: Added    0x%08x frames from %p to buddy.\n", count, start);
        }
    }
    size_t metadata_frame_count = metadata_size / PAGE_4K_SIZE;

    log_buffer_write(&frame_log, "frame: Reserved 0x%08x frames for ownership metadata at %p\n", metadata_frame_count, metadata_address);

    frame_allocator.origin_frames   = origin_frames;
    frame_allocator.usable_frames   = frame_allocator.buddy.free_pages;
    frame_allocator.metadata_frames = metadata_frame_count;

    log_buffer_write(&frame_log, "frame: Total physical frames = 0x%08x (%d KiB)\n", origin_frames, (origin_frames * 4096) >> 10);
    log_buffer_write(&frame_log, "frame: Available frames after buddy metadata = 0x%08x (%d KiB)\n", frame_allocator.usable_frames, (frame_allocator.usable_frames * 4096) >> 10);
}

/* Allocate count frames aligned to 2^alignment_order pages. */
static uint64_t alloc_frames_aligned(size_t count, unsigned alignment_order)
{
    if (!count) return 0;
    unsigned order = buddy_order_for_units(count);
    if (order > BUDDY_MAX_ORDER) return 0;
    if (alignment_order > order) order = alignment_order;

    if (count == 1 && alignment_order == 0) {
        size_t frame = frame_pcp_pop();
        if (frame == SIZE_MAX) frame = frame_pcp_refill();
        if (frame == SIZE_MAX) {
            frame_pcp_drain_all();
            frame = frame_pcp_refill();
        }
        return frame == SIZE_MAX ? 0 : frame * PAGE_4K_SIZE;
    }

    spin_lock(&frame_allocator.lock);
    size_t frame_index = buddy_alloc(&frame_allocator.buddy, order);
    if (frame_index == SIZE_MAX) {
        spin_unlock(&frame_allocator.lock);
        frame_pcp_drain_all();
        spin_lock(&frame_allocator.lock);
        frame_index = buddy_alloc(&frame_allocator.buddy, order);
        if (frame_index == SIZE_MAX) {
            spin_unlock(&frame_allocator.lock);
            return 0;
        }
    }
    if (buddy_trim_allocation(&frame_allocator.buddy, frame_index, order, count)) {
        plogk("frame: Trim failed for order %u block at 0x%016llx (keep %llu frames)\n", order, frame_index * PAGE_4K_SIZE, (uint64_t)count);
        (void)buddy_free(&frame_allocator.buddy, frame_index, order);
        spin_unlock(&frame_allocator.lock);
        return 0;
    }
    for (size_t i = 0; i < count; i++) frame_allocator.buddy.pages[frame_index + i].tag = 1;
    __atomic_sub_fetch(&frame_allocator.usable_frames, count, __ATOMIC_RELAXED);
    spin_unlock(&frame_allocator.lock);
    return frame_index * PAGE_4K_SIZE;
}

/* Allocate memory frames; reclaim I/O is initiated only at explicit safe points. */
uint64_t alloc_frames(size_t count)
{
    return alloc_frames_aligned(count, 0);
}

/* Compatibility spelling for lock-held callers; allocation itself never reclaims. */
uint64_t alloc_frames_noreclaim(size_t count)
{
    return alloc_frames_aligned(count, 0);
}

/* Allocate 2M memory frames */
uint64_t alloc_frames_2M(size_t count)
{
    if (!count || count > SIZE_MAX / 512) return 0;
    return alloc_frames_aligned(count * 512, 9);
}

/* Allocate 1G memory frames */
uint64_t alloc_frames_1G(size_t count)
{
    if (!count || count > SIZE_MAX / 262144) return 0;
    return alloc_frames_aligned(count * 262144, 18);
}

/* Bump the reference count of an allocated frame range. */
int frame_retain_range(uint64_t addr, size_t count)
{
    if (!addr || !count || (addr & (PAGE_4K_SIZE - 1))) return -1;
    size_t frame_index = addr / PAGE_4K_SIZE;
    if (frame_index >= frame_allocator.frame_count || count > frame_allocator.frame_count - frame_index) return -1;

    spin_lock(&frame_allocator.lock);
    for (size_t i = 0; i < count; i++) {
        buddy_page_t *page = &frame_allocator.buddy.pages[frame_index + i];
        uint32_t      refs = page->tag;
        if (page->state != BUDDY_PAGE_ALLOC_HEAD || page->order != 0) {
            spin_unlock(&frame_allocator.lock);
            return -1;
        }
        if (!refs || refs == UINT32_MAX) {
            spin_unlock(&frame_allocator.lock);
            return -1;
        }
    }
    for (size_t i = 0; i < count; i++) frame_allocator.buddy.pages[frame_index + i].tag++;
    spin_unlock(&frame_allocator.lock);
    return 0;
}

/* Drop a reference from a frame range, freeing frames at zero. */
int frame_release_range(uint64_t addr, size_t count)
{
    if (!addr || !count || (addr & (PAGE_4K_SIZE - 1))) return -1;
    size_t frame_index = addr / PAGE_4K_SIZE;
    if (frame_index >= frame_allocator.frame_count || count > frame_allocator.frame_count - frame_index) return -1;

    spin_lock(&frame_allocator.lock);
    for (size_t i = 0; i < count; i++) {
        buddy_page_t *page = &frame_allocator.buddy.pages[frame_index + i];
        if (page->state != BUDDY_PAGE_ALLOC_HEAD || page->order != 0 || !page->tag) {
            plogk("frame: Invalid release at 0x%016llx (state %u, order %u, refs %u)\n", addr + i * PAGE_4K_SIZE, page->state, page->order, page->tag);
            spin_unlock(&frame_allocator.lock);
            return -1;
        }
    }
    size_t released = 0;
    for (size_t i = 0; i < count; i++) {
        size_t        index = frame_index + i;
        buddy_page_t *page  = &frame_allocator.buddy.pages[index];
        page->tag--;
        if (!page->tag) {
            page->reserved = 1; /* exactly this release owns the PCP handoff */
            released++;
        }
    }
    __atomic_add_fetch(&frame_allocator.usable_frames, released, __ATOMIC_RELAXED);
    spin_unlock(&frame_allocator.lock);

    /* Final references stay as order-0 allocated heads while cached. */
    if (released)
        for (size_t i = 0; i < count; i++)
            if (__atomic_exchange_n(&frame_allocator.buddy.pages[frame_index + i].reserved, 0, __ATOMIC_ACQ_REL)) frame_pcp_put(frame_index + i);
    return 0;
}

/* Return the reference count of a single frame. */
uint32_t frame_refcount(uint64_t addr)
{
    if (!addr || (addr & (PAGE_4K_SIZE - 1))) return 0;
    size_t frame_index = addr / PAGE_4K_SIZE;
    if (frame_index >= frame_allocator.frame_count) return 0;
    buddy_page_t *page = &frame_allocator.buddy.pages[frame_index];
    if (page->state != BUDDY_PAGE_ALLOC_HEAD || page->order != 0) return 0;
    return __atomic_load_n(&page->tag, __ATOMIC_ACQUIRE);
}

/* Snapshot frame allocator statistics. */
void frame_get_stats(frame_stats_t *stats)
{
    if (!stats) return;
    uint64_t rflags        = spin_lock_irqsave(&frame_allocator.lock);
    stats->total_frames    = frame_allocator.origin_frames;
    stats->free_frames     = __atomic_load_n(&frame_allocator.usable_frames, __ATOMIC_RELAXED);
    stats->metadata_frames = frame_allocator.metadata_frames;
    stats->max_order       = frame_allocator.buddy.max_order;
    for (unsigned order = 0; order <= BUDDY_MAX_ORDER; order++) stats->free_blocks[order] = frame_allocator.buddy.free_count[order];
    spin_unlock_irqrestore(&frame_allocator.lock, rflags);
}

/* Validate the underlying buddy allocator and frame counters. */
int frame_validate(void)
{
    frame_pcp_drain_all();
    uint64_t rflags = spin_lock_irqsave(&frame_allocator.lock);
    int      result = buddy_validate(&frame_allocator.buddy);
    if (!result && frame_allocator.usable_frames != frame_allocator.buddy.free_pages) result = -1;
    spin_unlock_irqrestore(&frame_allocator.lock, rflags);
    return result;
}

/* Free a memory frame */
void free_frame(uint64_t addr)
{
    (void)frame_release_range(addr, 1);
}

/* Free memory frames */
void free_frames(uint64_t addr, size_t count)
{
    (void)frame_release_range(addr, count);
}

/* Free 2M memory frames */
void free_frames_2M(uint64_t addr)
{
    (void)frame_release_range(addr, PAGE_2M_SIZE / PAGE_4K_SIZE);
}

/* Free 1G memory frames */
void free_frames_1G(uint64_t addr)
{
    (void)frame_release_range(addr, PAGE_1G_SIZE / PAGE_4K_SIZE);
}

/* Print memory map */
void print_memory_map(void)
{
    if (!memmap_request.response) return;
    plogk("Physical RAM map:\n");
    plogk(" <MEMMAP>\n");

    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry  = memmap_request.response->entries[i];
        uint64_t                    base   = entry->base;
        uint64_t                    length = entry->length;
        uint64_t                    end    = base + length - 1;

        const char *type_str;
        switch (entry->type) {
            case LIMINE_MEMMAP_USABLE :
                type_str = "usable";
                break;
            case LIMINE_MEMMAP_RESERVED :
                type_str = "reserved";
                break;
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE :
                type_str = "ACPI reclaimable";
                break;
            case LIMINE_MEMMAP_ACPI_NVS :
                type_str = "ACPI NVS";
                break;
            case LIMINE_MEMMAP_BAD_MEMORY :
                type_str = "bad memory";
                break;
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE :
                type_str = "bootloader reclaimable";
                break;
            case LIMINE_MEMMAP_KERNEL_AND_MODULES :
                type_str = "kernel and modules";
                break;
            case LIMINE_MEMMAP_FRAMEBUFFER :
                type_str = "framebuffer";
                break;
            default :
                type_str = "unknown";
                break;
        }
        plogk("  [mem %p-%p] (%*llu KiB) %s\n", base, end, 9, length / 1024, type_str);
    }
    plogk(" </MEMMAP>\n");
}
