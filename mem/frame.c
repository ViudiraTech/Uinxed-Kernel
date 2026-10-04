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
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/stdbool.h>
#include <libs/std/string.h>
#include <libs/util/bitops.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/numa.h>
#include <mem/pagecache.h>
#include <mem/swap.h>
#include <process/sched.h>
#include <sync/spin_lock.h>

/*
 * Order-0 allocations come from per-CPU pagesets before touching the
 * zone buddy.  Keep the same shape here: a modest cache absorbs page faults,
 * page-cache churn and GEM allocations without bouncing one global lock.
 */

typedef struct {
        spinlock_t lock;
        uint32_t   count;
        size_t     frames[CONFIG_FRAME_PCP_HIGH];
} frame_pcp_t;

log_buffer_t      frame_log;
frame_allocator_t frame_allocator;
uint64_t          memory_size = 0;

typedef struct {
        buddy_allocator_t buddy;
        spinlock_t        lock;
        size_t            total_frames;
        size_t            free_frames;
} frame_node_t;

static frame_node_t frame_nodes[CONFIG_NUMA_MAX_NODES];
static nodemask_t   memory_nodes;

/* Metadata is shared; each node has independent free lists and locking. */
static buddy_allocator_t *node_buddy(uint16_t node)
{
    return node == 0 ? &frame_allocator.buddy : &frame_nodes[node].buddy;
}

static uint16_t frame_node(size_t frame)
{
    return __atomic_load_n(&frame_allocator.buddy.pages[frame].reserved, __ATOMIC_RELAXED) >> 1;
}

static raw_spinlock_t    frame_reclaim_lock;
static volatile uint32_t frame_reclaim_backoff;
static frame_pcp_t       frame_pcp[CONFIG_FRAME_PCP_MAX_CPUS];

/* Early boot allocations all belong to CPU 0; topology is not safe yet. */
static uint32_t frame_pcp_cpu(void)
{
    if (!__atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE)) return 0;
    uint32_t cpu = get_current_cpu_id();
    return cpu < CONFIG_FRAME_PCP_MAX_CPUS ? cpu : CONFIG_FRAME_PCP_MAX_CPUS;
}

/* Return cached order-0 pages to the buddy without changing logical free RAM. */
static void frame_pcp_return_to_buddy(const size_t *frames, size_t count)
{
    if (!count) return;
    for (size_t i = 0; i < count; i++) {
        uint16_t node  = frame_node(frames[i]);
        uint64_t flags = spin_lock_irqsave(&frame_nodes[node].lock);
        if (buddy_free(node_buddy(node), frames[i], 0)) plogk("frame: PCP drain failed for 0x%016llx\n", frames[i] * PAGE_4K_SIZE);
        spin_unlock_irqrestore(&frame_nodes[node].lock, flags);
    }
}

/* Drain one CPU cache, used to satisfy fragmented contiguous allocations. */
static void frame_pcp_drain_cpu(uint32_t cpu)
{
    size_t frames[CONFIG_FRAME_PCP_HIGH];
    size_t count;

    uint64_t rflags = spin_lock_irqsave(&frame_pcp[cpu].lock);
    count           = frame_pcp[cpu].count;
    memcpy(frames, frame_pcp[cpu].frames, count * sizeof(frames[0]));
    frame_pcp[cpu].count = 0;
    spin_unlock_irqrestore(&frame_pcp[cpu].lock, rflags);
    frame_pcp_return_to_buddy(frames, count);
}

/* Drain the per-CPU frame caches for all supported CPUs. */
static void frame_pcp_drain_all(void)
{
    for (uint32_t cpu = 0; cpu < CONFIG_FRAME_PCP_MAX_CPUS; cpu++) frame_pcp_drain_cpu(cpu);
}

/* Put one logically-free order-0 page into the local cache. */
static void frame_pcp_put(size_t frame)
{
    uint32_t cpu = frame_pcp_cpu();
    if (cpu >= CONFIG_FRAME_PCP_MAX_CPUS) {
        frame_pcp_return_to_buddy(&frame, 1);
        return;
    }

    size_t   drain[CONFIG_FRAME_PCP_BATCH];
    size_t   drain_count = 0;
    uint64_t rflags      = spin_lock_irqsave(&frame_pcp[cpu].lock);
    if (frame_pcp[cpu].count == CONFIG_FRAME_PCP_HIGH) {
        drain_count = CONFIG_FRAME_PCP_BATCH;
        for (size_t i = 0; i < drain_count; i++) drain[i] = frame_pcp[cpu].frames[--frame_pcp[cpu].count];
    }
    frame_pcp[cpu].frames[frame_pcp[cpu].count++] = frame;
    spin_unlock_irqrestore(&frame_pcp[cpu].lock, rflags);
    frame_pcp_return_to_buddy(drain, drain_count);
}

/* Pop one local page and make it externally owned. */
static size_t frame_pcp_pop(uint16_t node)
{
    uint32_t cpu = frame_pcp_cpu();
    if (cpu >= CONFIG_FRAME_PCP_MAX_CPUS) return SIZE_MAX;

    uint64_t rflags = spin_lock_irqsave(&frame_pcp[cpu].lock);
    size_t   frame  = SIZE_MAX;
    for (uint32_t i = frame_pcp[cpu].count; i; i--) {
        if (frame_node(frame_pcp[cpu].frames[i - 1]) != node) continue;
        frame                                  = frame_pcp[cpu].frames[i - 1];
        frame_pcp[cpu].frames[i - 1]           = frame_pcp[cpu].frames[--frame_pcp[cpu].count];
        frame_allocator.buddy.pages[frame].tag = 1;
        break;
    }
    spin_unlock_irqrestore(&frame_pcp[cpu].lock, rflags);
    if (frame != SIZE_MAX) {
        __atomic_sub_fetch(&frame_allocator.usable_frames, 1, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&frame_nodes[node].free_frames, 1, __ATOMIC_RELAXED);
    }
    return frame;
}

/* Refill a local cache from one buddy block and return its first page. */
static size_t frame_pcp_refill(uint16_t node)
{
    size_t   first = SIZE_MAX;
    size_t   units = 0;
    unsigned order = buddy_order_for_units(CONFIG_FRAME_PCP_BATCH);
    if (((size_t)1 << order) > CONFIG_FRAME_PCP_BATCH) order--;
    buddy_allocator_t *buddy = node_buddy(node);
    uint64_t           flags = spin_lock_irqsave(&frame_nodes[node].lock);
    while (1) {
        first = buddy_alloc(buddy, order);
        if (first != SIZE_MAX || order == 0) break;
        order--;
    }
    if (first != SIZE_MAX) {
        units = (size_t)1 << order;
        if (buddy_trim_allocation(buddy, first, order, units)) {
            (void)buddy_free(buddy, first, order);
            first = SIZE_MAX;
            units = 0;
        } else {
            for (size_t i = 0; i < units; i++) buddy->pages[first + i].tag = 0;
            buddy->pages[first].tag = 1;
        }
    }
    spin_unlock_irqrestore(&frame_nodes[node].lock, flags);
    if (first == SIZE_MAX) return SIZE_MAX;
    __atomic_sub_fetch(&frame_allocator.usable_frames, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&frame_nodes[node].free_frames, 1, __ATOMIC_RELAXED);
    for (size_t i = 1; i < units; i++) frame_pcp_put(first + i);
    return first;
}

/* Serialize reclaim so swap I/O cannot recursively enter reclaim allocation. */
static int frame_try_reclaim(size_t target)
{
    if (!raw_spin_trylock(&frame_reclaim_lock)) return 0;
    int reclaimed = swap_reclaim(target);
    raw_spin_unlock(&frame_reclaim_lock);
    return reclaimed;
}

/* Reclaim up to the requested number of free frame pages. */
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
    if (cache_target > CONFIG_FRAME_RECLAIM_BATCH) cache_target = CONFIG_FRAME_RECLAIM_BATCH;
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
    if (target > CONFIG_FRAME_RECLAIM_BATCH) target = CONFIG_FRAME_RECLAIM_BATCH;
    if (frame_try_reclaim(target) == 0) {
        __atomic_store_n(&frame_reclaim_backoff, 64, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&frame_reclaim_backoff, 0, __ATOMIC_RELAXED);
    }
}

/* Add boot RAM a node at a time, preserving reserved holes and ownership. */
static void frame_add_range(size_t start, size_t count)
{
    while (count) {
        uint16_t node    = numa_phys_node(start * PAGE_4K_SIZE);
        uint64_t address = start * PAGE_4K_SIZE;
        uint64_t end     = (start + count) * PAGE_4K_SIZE;
        for (uint16_t i = 0; i < numa_topology.nr_ranges; i++) {
            const numa_range_t *range = &numa_topology.ranges[i];
            if (range->base > address && range->base < end) end = range->base;
            if (range->end > address && range->end < end) end = range->end;
        }
        size_t units = (end - address) / PAGE_4K_SIZE;
        for (size_t i = 0; i < units; i++) frame_allocator.buddy.pages[start + i].reserved = (uint16_t)(node << 1);
        if (buddy_add_range(node_buddy(node), start, units)) krn_halt();
        frame_nodes[node].total_frames += units;
        memory_nodes |= 1ULL << node;
        start += units;
        count -= units;
    }
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
    log_buffer_write(&frame_log, "frame: Highest usable address is %p\n", (void *)memory_size);
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
        log_buffer_write(&frame_log, "frame: Ownership metadata allocated at %p (size: %zu KiB)\n", (void *)metadata_address, metadata_size / 1024);
    } else {
        log_buffer_write(&frame_log, "frame: Failed to allocate ownership metadata.\n");
        return;
    }
    unsigned max_order = buddy_order_for_units(frame_count);
    if (max_order > CONFIG_BUDDY_MAX_ORDER || ((size_t)1 << max_order) > frame_count) max_order--;
    if (buddy_init(&frame_allocator.buddy, phys_to_virt(metadata_address), frame_count, max_order)) {
        log_buffer_write(&frame_log, "frame: Failed to initialise buddy metadata.\n");
        return;
    }
    for (uint16_t node = 1; node < numa_topology.nr_nodes; node++) {
        frame_nodes[node].buddy      = frame_allocator.buddy;
        frame_nodes[node].buddy.node = node;
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
                frame_add_range(start_frame, count);
            } else {
                if (metadata_start > start_frame) frame_add_range(start_frame, metadata_start - start_frame);
                size_t after_metadata = metadata_start + metadata_count;
                if (after_metadata < range_end) frame_add_range(after_metadata, range_end - after_metadata);
            }
            log_buffer_write(&frame_log, "frame: Added    0x%08zx frames from %p to buddy.\n", count, (void *)start);
        }
    }
    size_t metadata_frame_count = metadata_size / PAGE_4K_SIZE;

    log_buffer_write(&frame_log, "frame: Reserved 0x%08zx frames for ownership metadata at %p\n", metadata_frame_count, (void *)metadata_address);

    frame_allocator.origin_frames = origin_frames;
    frame_allocator.usable_frames = 0;
    for (uint16_t node = 0; node < numa_topology.nr_nodes; node++) {
        frame_nodes[node].free_frames = node_buddy(node)->free_pages;
        frame_allocator.usable_frames += frame_nodes[node].free_frames;
    }
    frame_allocator.metadata_frames = metadata_frame_count;

    log_buffer_write(&frame_log, "frame: Total physical frames = 0x%08zx (%zu KiB)\n", origin_frames, (origin_frames * 4096) >> 10);
    log_buffer_write(&frame_log, "frame: Available frames after buddy metadata = 0x%08zx (%zu KiB)\n", frame_allocator.usable_frames, (frame_allocator.usable_frames * 4096) >> 10);
}

/* Try one node, with its own buddy lock; fallback order belongs to the caller. */
static uint64_t alloc_on_node(size_t count, unsigned order, unsigned alignment_order, uint16_t node)
{
    if (count == 1 && alignment_order == 0) {
        size_t frame = frame_pcp_pop(node);
        if (frame == SIZE_MAX) frame = frame_pcp_refill(node);
        return frame == SIZE_MAX ? 0 : frame * PAGE_4K_SIZE;
    }
    buddy_allocator_t *buddy = node_buddy(node);
    uint64_t           flags = spin_lock_irqsave(&frame_nodes[node].lock);
    size_t             first = buddy_alloc(buddy, order);
    if (first == SIZE_MAX) {
        spin_unlock_irqrestore(&frame_nodes[node].lock, flags);
        return 0;
    }
    if (buddy_trim_allocation(buddy, first, order, count)) {
        (void)buddy_free(buddy, first, order);
        spin_unlock_irqrestore(&frame_nodes[node].lock, flags);
        return 0;
    }
    for (size_t i = 0; i < count; i++) buddy->pages[first + i].tag = 1;
    __atomic_sub_fetch(&frame_allocator.usable_frames, count, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&frame_nodes[node].free_frames, count, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&frame_nodes[node].lock, flags);
    return first * PAGE_4K_SIZE;
}

/* Node exhaustion falls back by distance; BIND never escapes its allowed set. */
static uint64_t alloc_frames_mask(size_t count, unsigned alignment_order, uint16_t preferred, nodemask_t allowed)
{
    if (!count || alignment_order > CONFIG_BUDDY_MAX_ORDER) return 0;
    unsigned order = buddy_order_for_units(count);
    if (order > CONFIG_BUDDY_MAX_ORDER) return 0;
    if (alignment_order > order) order = alignment_order;
    allowed &= memory_nodes;
    if (!allowed) return 0;
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        nodemask_t remaining = allowed;
        while (remaining) {
            uint16_t node = numa_nearest_node(preferred, remaining);
            if (node == NUMA_NO_NODE) break;
            remaining &= ~(1ULL << node);
            uint64_t address = alloc_on_node(count, order, alignment_order, node);
            if (address) return address;
        }
        if (!attempt) frame_pcp_drain_all();
    }
    return 0;
}

static uint16_t allocation_node(void)
{
    return numa_cpu_node(__atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE) ? get_current_cpu_id() : 0);
}

uint64_t alloc_frames(size_t count)
{
    return alloc_frames_mask(count, 0, allocation_node(), memory_nodes);
}

uint64_t alloc_frames_node(size_t count, uint16_t node, bool strict)
{
    if (node >= numa_topology.nr_nodes) return 0;
    return alloc_frames_mask(count, 0, node, strict ? 1ULL << node : memory_nodes);
}

uint64_t alloc_frames_policy(size_t count, unsigned alignment_order, uint64_t page_index, const numa_policy_t *vma_policy)
{
    task_t    *task      = __atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE) ? current_task() : NULL;
    nodemask_t allowed   = task ? __atomic_load_n(&task->mems_allowed, __ATOMIC_ACQUIRE) & memory_nodes : memory_nodes;
    uint16_t   preferred = allocation_node();
    if (task) {
        const numa_policy_t *policy = vma_policy && vma_policy->mode != NUMA_POLICY_DEFAULT ? vma_policy : &task->mempolicy;
        if (policy->mode == NUMA_POLICY_BIND) allowed &= policy->nodes;
        if (policy->mode == NUMA_POLICY_PREFERRED && (allowed & (1ULL << policy->preferred))) preferred = policy->preferred;
        if (policy->mode == NUMA_POLICY_INTERLEAVE) {
            nodemask_t nodes = policy->nodes & allowed;
            if (!nodes) return 0;
            uint64_t cursor  = vma_policy && vma_policy->mode != NUMA_POLICY_DEFAULT ? page_index : task->numa_interleave_next++;
            uint64_t ordinal = cursor % (uint64_t)popcount64(nodes);
            while (ordinal--) nodes &= nodes - 1;
            preferred = (uint16_t)__builtin_ctzll(nodes);
        }
    }
    return alloc_frames_mask(count, alignment_order, preferred, allowed);
}

uint64_t alloc_frames_user(size_t count, unsigned alignment_order, uint64_t page_index)
{
    return alloc_frames_policy(count, alignment_order, page_index, NULL);
}

uint64_t alloc_frames_2M(size_t count)
{
    if (!count || count > SIZE_MAX / 512) return 0;
    return alloc_frames_mask(count * 512, 9, allocation_node(), memory_nodes);
}

uint64_t alloc_frames_1G(size_t count)
{
    if (!count || count > SIZE_MAX / 262144) return 0;
    return alloc_frames_mask(count * 262144, 18, allocation_node(), memory_nodes);
}

nodemask_t frame_memory_nodes(void)
{
    return memory_nodes;
}

/* Bump the reference count of an allocated frame range. */
int frame_retain_range(uint64_t addr, size_t count)
{
    if (!addr || !count || (addr & (PAGE_4K_SIZE - 1))) return -EINVAL;
    size_t frame_index = addr / PAGE_4K_SIZE;
    if (frame_index >= frame_allocator.frame_count || count > frame_allocator.frame_count - frame_index) return -EINVAL;

    spin_lock(&frame_allocator.lock);
    for (size_t i = 0; i < count; i++) {
        buddy_page_t *page = &frame_allocator.buddy.pages[frame_index + i];
        uint32_t      refs = page->tag;
        if (page->state != BUDDY_PAGE_ALLOC_HEAD || page->order != 0) {
            spin_unlock(&frame_allocator.lock);
            return -EFAULT;
        }
        if (!refs || refs == UINT32_MAX) {
            spin_unlock(&frame_allocator.lock);
            return -EOVERFLOW;
        }
    }
    for (size_t i = 0; i < count; i++) frame_allocator.buddy.pages[frame_index + i].tag++;
    spin_unlock(&frame_allocator.lock);
    return 0;
}

/* Drop a reference from a frame range, freeing frames at zero. */
int frame_release_range(uint64_t addr, size_t count)
{
    if (!addr || !count || (addr & (PAGE_4K_SIZE - 1))) return -EINVAL;
    size_t frame_index = addr / PAGE_4K_SIZE;
    if (frame_index >= frame_allocator.frame_count || count > frame_allocator.frame_count - frame_index) return -EINVAL;

    spin_lock(&frame_allocator.lock);
    for (size_t i = 0; i < count; i++) {
        buddy_page_t *page = &frame_allocator.buddy.pages[frame_index + i];
        if (page->state != BUDDY_PAGE_ALLOC_HEAD || page->order != 0 || !page->tag) {
            plogk_once("frame: Invalid release at 0x%016llx (state %u, order %u, refs %u)\n", addr + (i * PAGE_4K_SIZE), page->state, page->order, page->tag);
            spin_unlock(&frame_allocator.lock);
            return -EFAULT;
        }
    }
    size_t released = 0;
    for (size_t i = 0; i < count; i++) {
        size_t        index = frame_index + i;
        buddy_page_t *page  = &frame_allocator.buddy.pages[index];
        page->tag--;
        if (!page->tag) {
            __atomic_fetch_or(&page->reserved, 1U, __ATOMIC_RELAXED); // exactly this release owns the PCP handoff
            released++;
            __atomic_add_fetch(&frame_nodes[frame_node(index)].free_frames, 1, __ATOMIC_RELAXED);
        }
    }
    __atomic_add_fetch(&frame_allocator.usable_frames, released, __ATOMIC_RELAXED);
    spin_unlock(&frame_allocator.lock);

    /* Final references stay as order-0 allocated heads while cached. */
    if (released)
        for (size_t i = 0; i < count; i++)
            if (__atomic_fetch_and(&frame_allocator.buddy.pages[frame_index + i].reserved, (uint16_t)~1U, __ATOMIC_ACQ_REL) & 1) frame_pcp_put(frame_index + i);
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

/* Per-node snapshots include pages parked in CPU caches. */
int frame_get_node_stats(uint16_t node, frame_stats_t *stats)
{
    if (!stats || node >= numa_topology.nr_nodes) return -EINVAL;
    uint64_t           flags = spin_lock_irqsave(&frame_nodes[node].lock);
    buddy_allocator_t *buddy = node_buddy(node);
    memset(stats, 0, sizeof(*stats));
    stats->total_frames = frame_nodes[node].total_frames;
    stats->free_frames  = __atomic_load_n(&frame_nodes[node].free_frames, __ATOMIC_RELAXED);
    stats->max_order    = buddy->max_order;
    for (unsigned order = 0; order <= CONFIG_BUDDY_MAX_ORDER; order++) stats->free_blocks[order] = buddy->free_count[order];
    spin_unlock_irqrestore(&frame_nodes[node].lock, flags);
    return 0;
}

void frame_get_stats(frame_stats_t *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->total_frames    = frame_allocator.origin_frames;
    stats->free_frames     = __atomic_load_n(&frame_allocator.usable_frames, __ATOMIC_RELAXED);
    stats->metadata_frames = frame_allocator.metadata_frames;
    stats->max_order       = frame_allocator.buddy.max_order;
    for (uint16_t node = 0; node < numa_topology.nr_nodes; node++) {
        frame_stats_t snapshot;
        (void)frame_get_node_stats(node, &snapshot);
        for (unsigned order = 0; order <= CONFIG_BUDDY_MAX_ORDER; order++) stats->free_blocks[order] += snapshot.free_blocks[order];
    }
}

/* Validation is a quiescent diagnostic: allocation must be stopped by caller. */
int frame_validate(void)
{
    frame_pcp_drain_all();
    size_t free = 0;
    for (uint16_t node = 0; node < numa_topology.nr_nodes; node++) {
        uint64_t flags  = spin_lock_irqsave(&frame_nodes[node].lock);
        int      result = buddy_validate(node_buddy(node));
        size_t   pages  = node_buddy(node)->free_pages;
        if (!result && frame_nodes[node].free_frames != pages) result = -EFAULT;
        free += pages;
        spin_unlock_irqrestore(&frame_nodes[node].lock, flags);
        if (result) return result;
    }
    return free == frame_allocator.usable_frames ? 0 : -EFAULT;
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
        plogk("  [mem %p-%p] (%*llu KiB) %s\n", (void *)base, (void *)end, 9, (length / 1024), type_str);
    }
    plogk(" </MEMMAP>\n");
}
