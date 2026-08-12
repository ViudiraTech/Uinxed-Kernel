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
#include <mem/swap.h>

log_buffer_t      frame_log;
frame_allocator_t frame_allocator;
uint64_t          memory_size = 0;

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
static uint64_t alloc_frames_aligned(size_t count, unsigned alignment_order, bool reclaim)
{
    if (!count) return 0;
    unsigned order = buddy_order_for_units(count);
    if (order > BUDDY_MAX_ORDER) return 0;
    if (alignment_order > order) order = alignment_order;
retry:
    spin_lock(&frame_allocator.lock);
    size_t frame_index = buddy_alloc(&frame_allocator.buddy, order);
    if (frame_index == SIZE_MAX) {
        spin_unlock(&frame_allocator.lock);
        if (reclaim && count == 1 && swap_reclaim(1) > 0) goto retry;
        return 0;
    }
    if (buddy_trim_allocation(&frame_allocator.buddy, frame_index, order, count)) {
        plogk("frame: Trim failed for order %u block at 0x%016llx (keep %llu frames)\n", order, frame_index * PAGE_4K_SIZE, (uint64_t)count);
        (void)buddy_free(&frame_allocator.buddy, frame_index, order);
        spin_unlock(&frame_allocator.lock);
        return 0;
    }
    for (size_t i = 0; i < count; i++) frame_allocator.buddy.pages[frame_index + i].tag = 1;
    frame_allocator.usable_frames = frame_allocator.buddy.free_pages;
    spin_unlock(&frame_allocator.lock);
    return frame_index * PAGE_4K_SIZE;
}

/* Allocate memory frames */
uint64_t alloc_frames(size_t count)
{
    return alloc_frames_aligned(count, 0, true);
}

/* Allocate frames without attempting swap reclaim. */
uint64_t alloc_frames_noreclaim(size_t count)
{
    return alloc_frames_aligned(count, 0, false);
}

/* Allocate 2M memory frames */
uint64_t alloc_frames_2M(size_t count)
{
    if (!count || count > SIZE_MAX / 512) return 0;
    return alloc_frames_aligned(count * 512, 9, true);
}

/* Allocate 1G memory frames */
uint64_t alloc_frames_1G(size_t count)
{
    if (!count || count > SIZE_MAX / 262144) return 0;
    return alloc_frames_aligned(count * 262144, 18, true);
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
    for (size_t i = 0; i < count; i++) {
        size_t        index = frame_index + i;
        buddy_page_t *page  = &frame_allocator.buddy.pages[index];
        page->tag--;
        if (!page->tag) {
            if (buddy_free(&frame_allocator.buddy, index, 0)) {
                plogk("frame: Buddy release failed for 0x%016llx\n", addr + i * PAGE_4K_SIZE);
                spin_unlock(&frame_allocator.lock);
                return -1;
            }
        }
    }
    frame_allocator.usable_frames = frame_allocator.buddy.free_pages;
    spin_unlock(&frame_allocator.lock);
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
    stats->free_frames     = frame_allocator.buddy.free_pages;
    stats->metadata_frames = frame_allocator.metadata_frames;
    stats->max_order       = frame_allocator.buddy.max_order;
    for (unsigned order = 0; order <= BUDDY_MAX_ORDER; order++) stats->free_blocks[order] = frame_allocator.buddy.free_count[order];
    spin_unlock_irqrestore(&frame_allocator.lock, rflags);
}

/* Validate the underlying buddy allocator and frame counters. */
int frame_validate(void)
{
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
