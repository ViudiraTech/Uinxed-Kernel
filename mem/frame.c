/*
 *
 *      frame.c
 *      Memory frame
 *
 *      2025/2/16 By XIAOYI12
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <bitmap.h>
#include <frame.h>
#include <hhdm.h>
#include <limine.h>
#include <page.h>
#include <printk.h>
#include <uinxed.h>

log_buffer_t      frame_log;
frame_allocator_t frame_allocator;
uint64_t          memory_size = 0;

/* Initialize memory frame */
void init_frame(void)
{
    struct limine_memmap_response *memory_map = memmap_request.response;
    for (uint64_t i = memory_map->entry_count - 1; i != 0; i--) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type == LIMINE_MEMMAP_USABLE) {
            memory_size = region->base + region->length;
            log_buffer_write(&frame_log, "frame: Found usable region at %p (size: %llu KiB)\n", region->base, region->length / 1024);
            break;
        }
    }
    size_t   bitmap_size    = (memory_size / PAGE_4K_SIZE + 7) / 8;
    uint64_t bitmap_address = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type == LIMINE_MEMMAP_USABLE && region->length >= bitmap_size) {
            bitmap_address = region->base;
            break;
        }
    }
    if (bitmap_address) {
        log_buffer_write(&frame_log, "frame: Bitmap allocated at %p (size: %llu KiB)\n", bitmap_address, bitmap_size / 1024);
    } else {
        log_buffer_write(&frame_log, "frame: Failed to allocate bitmap memory.\n");
        return;
    }
    bitmap_t *bitmap = &frame_allocator.bitmap;
    bitmap_init(bitmap, phys_to_virt(bitmap_address), bitmap_size);
    bitmap_fill(bitmap, 0);
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
    size_t bitmap_frame_start = bitmap_address / 4096;
    size_t bitmap_frame_count = (bitmap_size + 4095) / 4096;
    size_t bitmap_frame_end   = bitmap_frame_start + bitmap_frame_count;
    bitmap_set_range(bitmap, bitmap_frame_start, bitmap_frame_end, 0);

    log_buffer_write(&frame_log, "frame: Reserved 0x%08x frames for bitmap at %p\n", bitmap_frame_count, bitmap_address);

    frame_allocator.origin_frames = origin_frames;
    frame_allocator.usable_frames = origin_frames - bitmap_frame_count;

    log_buffer_write(&frame_log, "frame: Total physical frames = 0x%08x (%d KiB)\n", origin_frames, (origin_frames * 4096) >> 10);
    log_buffer_write(&frame_log, "frame: Available frames after deducting bitmap usage = 0x%08x (%d KiB)\n", frame_allocator.usable_frames,
                     (frame_allocator.usable_frames * 4096) >> 10);
}

/* Allocate memory frames */
uint64_t alloc_frames(size_t count)
{
    bitmap_t *bitmap      = &frame_allocator.bitmap;
    size_t    frame_index = bitmap_find_range(bitmap, count, 1);

    if (frame_index == (size_t)-1) return 0;
    bitmap_set_range(bitmap, frame_index, frame_index + count, 0);
    frame_allocator.usable_frames -= count;
    return frame_index * 4096;
}

/* Allocate 2M memory frames */
uint64_t alloc_frames_2M(size_t count)
{
    bitmap_t *bitmap         = &frame_allocator.bitmap;
    size_t    frames_per_2mb = 512;
    size_t    total_frames   = count * frames_per_2mb;

    for (size_t i = 0; i < bitmap->length; i += frames_per_2mb) {
        if (i + total_frames > bitmap->length) break;
        if (bitmap_range_all(bitmap, i, i + total_frames, 1)) {
            bitmap_set_range(bitmap, i, i + total_frames, 0);
            frame_allocator.usable_frames -= total_frames;
            return i * 4096;
        }
    }
    return 0;
}

/* Allocate 1G memory frames */
uint64_t alloc_frames_1G(size_t count)
{
    bitmap_t *bitmap         = &frame_allocator.bitmap;
    size_t    frames_per_1gb = 262144;
    size_t    total_frames   = count * frames_per_1gb;

    for (size_t i = 0; i < bitmap->length; i += frames_per_1gb) {
        if (i + total_frames > bitmap->length) break;
        if (bitmap_range_all(bitmap, i, i + total_frames, 1)) {
            bitmap_set_range(bitmap, i, i + total_frames, 0);
            frame_allocator.usable_frames -= total_frames;
            return i * 4096;
        }
    }
    return 0;
}

/* Free a memory frame */
void free_frame(uint64_t addr)
{
    if (!addr) return;
    size_t frame_index = addr / 4096;

    if (!frame_index) return;
    bitmap_t *bitmap = &frame_allocator.bitmap;
    bitmap_set(bitmap, frame_index, 1);
    frame_allocator.usable_frames++;
}

/* Free memory frames */
void free_frames(uint64_t addr, size_t count)
{
    if (!addr || !count) return;
    size_t frame_index = addr / 4096;

    if (!frame_index) return;
    bitmap_t *bitmap = &frame_allocator.bitmap;
    bitmap_set_range(bitmap, frame_index, frame_index + count, 1);
    frame_allocator.usable_frames += count;
}

/* Free 2M memory frames */
void free_frames_2M(uint64_t addr)
{
    if (!addr) return;
    size_t frame_index = addr / 4096;

    if (!frame_index) return;
    bitmap_t *bitmap = &frame_allocator.bitmap;
    for (size_t i = 0; i < 512; i++) bitmap_set(bitmap, frame_index + i, 1);
    frame_allocator.usable_frames += 512;
}

/* Free 1G memory frames */
void free_frames_1G(uint64_t addr)
{
    if (!addr) return;
    size_t frame_index = addr / 4096;

    if (!frame_index) return;
    bitmap_t *bitmap = &frame_allocator.bitmap;
    for (size_t i = 0; i < 262144; i++) bitmap_set(bitmap, frame_index + i, 1);
    frame_allocator.usable_frames += 262144;
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
