/*
 *
 *      frame.c
 *      Memory frame
 *
 *      2025/2/16 By XIAOYI12
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <chipset/common.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/bitmap.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/page.h>

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
    size_t   bitmap_size      = ALIGN_UP((frame_count + 7) / 8, PAGE_4K_SIZE);
    size_t   refcount_size    = ALIGN_UP(frame_count * sizeof(uint32_t), PAGE_4K_SIZE);
    size_t   metadata_size    = bitmap_size + refcount_size;
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
    bitmap_t *bitmap = &frame_allocator.bitmap;
    bitmap_init(bitmap, phys_to_virt(metadata_address), bitmap_size);
    frame_allocator.refcounts   = phys_to_virt(metadata_address + bitmap_size);
    frame_allocator.frame_count = frame_count;
    memset(frame_allocator.refcounts, 0, refcount_size);
    size_t origin_frames = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type == LIMINE_MEMMAP_USABLE) {
            size_t start_frame = region->base / 4096;
            size_t frame_count = region->length / 4096;
            origin_frames += frame_count;
            bitmap_set_range(bitmap, start_frame, start_frame + frame_count, 1);
            log_buffer_write(&frame_log, "frame: Marked   0x%08x frames from %p as usable.\n", frame_count, region->base);
        }
    }
    size_t metadata_frame_start = metadata_address / PAGE_4K_SIZE;
    size_t metadata_frame_count = metadata_size / PAGE_4K_SIZE;
    size_t metadata_frame_end   = metadata_frame_start + metadata_frame_count;
    bitmap_set_range(bitmap, metadata_frame_start, metadata_frame_end, 0);

    log_buffer_write(&frame_log, "frame: Reserved 0x%08x frames for ownership metadata at %p\n", metadata_frame_count, metadata_address);

    frame_allocator.origin_frames = origin_frames;
    frame_allocator.usable_frames = origin_frames - metadata_frame_count;

    log_buffer_write(&frame_log, "frame: Total physical frames = 0x%08x (%d KiB)\n", origin_frames, (origin_frames * 4096) >> 10);
    log_buffer_write(&frame_log, "frame: Available frames after deducting bitmap usage = 0x%08x (%d KiB)\n", frame_allocator.usable_frames,
                     (frame_allocator.usable_frames * 4096) >> 10);
}

/* Allocate memory frames */
uint64_t alloc_frames(size_t count)
{
    if (!count) return 0;

    spin_lock(&frame_allocator.lock);
    bitmap_t *bitmap      = &frame_allocator.bitmap;
    size_t    frame_index = bitmap_find_range(bitmap, count, 1);
    if (frame_index == (size_t)-1 || frame_index + count > frame_allocator.frame_count) {
        spin_unlock(&frame_allocator.lock);
        return 0;
    }
    bitmap_set_range(bitmap, frame_index, frame_index + count, 0);
    for (size_t i = 0; i < count; i++) __atomic_store_n(&frame_allocator.refcounts[frame_index + i], 1, __ATOMIC_RELEASE);
    frame_allocator.usable_frames -= count;
    spin_unlock(&frame_allocator.lock);
    return frame_index * PAGE_4K_SIZE;
}

/* Allocate 2M memory frames */
uint64_t alloc_frames_2M(size_t count)
{
    if (!count || count > SIZE_MAX / 512) return 0;

    spin_lock(&frame_allocator.lock);
    bitmap_t *bitmap         = &frame_allocator.bitmap;
    size_t    frames_per_2mb = 512;
    size_t    total_frames   = count * frames_per_2mb;

    for (size_t i = 0; i < frame_allocator.frame_count; i += frames_per_2mb) {
        if (total_frames > frame_allocator.frame_count - i) break;
        if (bitmap_range_all(bitmap, i, i + total_frames, 1)) {
            bitmap_set_range(bitmap, i, i + total_frames, 0);
            for (size_t j = 0; j < total_frames; j++) __atomic_store_n(&frame_allocator.refcounts[i + j], 1, __ATOMIC_RELEASE);
            frame_allocator.usable_frames -= total_frames;
            spin_unlock(&frame_allocator.lock);
            return i * PAGE_4K_SIZE;
        }
    }
    spin_unlock(&frame_allocator.lock);
    return 0;
}

/* Allocate 1G memory frames */
uint64_t alloc_frames_1G(size_t count)
{
    if (!count || count > SIZE_MAX / 262144) return 0;

    spin_lock(&frame_allocator.lock);
    bitmap_t *bitmap         = &frame_allocator.bitmap;
    size_t    frames_per_1gb = 262144;
    size_t    total_frames   = count * frames_per_1gb;

    for (size_t i = 0; i < frame_allocator.frame_count; i += frames_per_1gb) {
        if (total_frames > frame_allocator.frame_count - i) break;
        if (bitmap_range_all(bitmap, i, i + total_frames, 1)) {
            bitmap_set_range(bitmap, i, i + total_frames, 0);
            for (size_t j = 0; j < total_frames; j++) __atomic_store_n(&frame_allocator.refcounts[i + j], 1, __ATOMIC_RELEASE);
            frame_allocator.usable_frames -= total_frames;
            spin_unlock(&frame_allocator.lock);
            return i * PAGE_4K_SIZE;
        }
    }
    spin_unlock(&frame_allocator.lock);
    return 0;
}

int frame_retain_range(uint64_t addr, size_t count)
{
    if (!addr || !count || (addr & (PAGE_4K_SIZE - 1))) return -1;
    size_t frame_index = addr / PAGE_4K_SIZE;
    if (frame_index >= frame_allocator.frame_count || count > frame_allocator.frame_count - frame_index) return -1;

    spin_lock(&frame_allocator.lock);
    for (size_t i = 0; i < count; i++) {
        uint32_t refs = __atomic_load_n(&frame_allocator.refcounts[frame_index + i], __ATOMIC_ACQUIRE);
        if (!refs || refs == UINT32_MAX) {
            spin_unlock(&frame_allocator.lock);
            return -1;
        }
    }
    for (size_t i = 0; i < count; i++) __atomic_add_fetch(&frame_allocator.refcounts[frame_index + i], 1, __ATOMIC_RELEASE);
    spin_unlock(&frame_allocator.lock);
    return 0;
}

int frame_release_range(uint64_t addr, size_t count)
{
    if (!addr || !count || (addr & (PAGE_4K_SIZE - 1))) return -1;
    size_t frame_index = addr / PAGE_4K_SIZE;
    if (frame_index >= frame_allocator.frame_count || count > frame_allocator.frame_count - frame_index) return -1;

    spin_lock(&frame_allocator.lock);
    for (size_t i = 0; i < count; i++) {
        if (!__atomic_load_n(&frame_allocator.refcounts[frame_index + i], __ATOMIC_ACQUIRE)) {
            spin_unlock(&frame_allocator.lock);
            return -1;
        }
    }
    for (size_t i = 0; i < count; i++) {
        size_t index = frame_index + i;
        if (__atomic_sub_fetch(&frame_allocator.refcounts[index], 1, __ATOMIC_ACQ_REL) == 0) {
            bitmap_set(&frame_allocator.bitmap, index, 1);
            frame_allocator.usable_frames++;
        }
    }
    spin_unlock(&frame_allocator.lock);
    return 0;
}

uint32_t frame_refcount(uint64_t addr)
{
    if (!addr || (addr & (PAGE_4K_SIZE - 1))) return 0;
    size_t frame_index = addr / PAGE_4K_SIZE;
    if (frame_index >= frame_allocator.frame_count) return 0;
    return __atomic_load_n(&frame_allocator.refcounts[frame_index], __ATOMIC_ACQUIRE);
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
