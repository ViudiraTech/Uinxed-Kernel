/*
 *
 *      frame.c
 *      Memory Frame
 *
 *      2025/2/16 By XIAOYI12
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "frame.h"
#include "hhdm.h"
#include "limine.h"
#include "page.h"
#include "printk.h"
#include "uinxed.h"

FrameAllocator frame_allocator;
uint64_t memory_size = 0;

/* Initialize memory frame */
void init_frame(void)
{
    struct limine_memmap_response *memory_map = memmap_request.response;
    for (uint64_t i = memory_map->entry_count - 1; i != 0; i--) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type == LIMINE_MEMMAP_USABLE) {
            memory_size = region->base + region->length;
            plogk("frame: Found maximum usable region at %p (size: %llu KiB)\n", region->base, region->length / 1024);
            break;
        }
    }
    size_t bitmap_size      = (memory_size / PAGE_SIZE + 7) / 8;
    uint64_t bitmap_address = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type == LIMINE_MEMMAP_USABLE && region->length >= bitmap_size) {
            bitmap_address = region->base;
            break;
        }
    }
    if (bitmap_address) {
        plogk("frame: Bitmap allocated at %p (size: %llu KiB)\n", bitmap_address, bitmap_size / 1024);
    } else {
        plogk("frame: Failed to allocate bitmap memory!\n");
        return;
    }
    Bitmap *bitmap = &frame_allocator.bitmap;
    bitmap_init(bitmap, phys_to_virt(bitmap_address), bitmap_size);
    size_t origin_frames = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *region = memory_map->entries[i];
        if (region->type == LIMINE_MEMMAP_USABLE) {
            size_t start_frame = region->base / 4096;
            size_t frame_count = region->length / 4096;
            origin_frames += frame_count;
            bitmap_set_range(bitmap, start_frame, start_frame + frame_count, 1);
            plogk("frame: Marked 0x%06x frames from %p as usable.\n", frame_count, region->base);
        }
    }
    size_t bitmap_frame_start = bitmap_address / 4096;
    size_t bitmap_frame_count = (bitmap_size + 4095) / 4096;
    size_t bitmap_frame_end   = bitmap_frame_start + bitmap_frame_count;
    bitmap_set_range(bitmap, bitmap_frame_start, bitmap_frame_end, 0);
    plogk("frame: Reserved 0x%04x frames for bitmap at %p\n", bitmap_frame_count, bitmap_address);
    frame_allocator.origin_frames = origin_frames;
    frame_allocator.usable_frames = origin_frames - bitmap_frame_count;
    plogk("frame: Total physical frames = 0x%08x (%d MiB)\n", origin_frames, (origin_frames * 4096) >> 20);
    plogk("frame: Available frames after bitmap = 0x%08x (%d MiB)\n", frame_allocator.usable_frames,
          (frame_allocator.usable_frames * 4096) >> 20);
}

/* Allocate memory frame */
uint64_t alloc_frames(size_t count)
{
    Bitmap *bitmap     = &frame_allocator.bitmap;
    size_t frame_index = bitmap_find_range(bitmap, count, 1);

    if (frame_index == (size_t)-1) return 0;
    bitmap_set_range(bitmap, frame_index, frame_index + count, 0);
    frame_allocator.usable_frames -= count;
    return frame_index * 4096;
}

/* Free memory frames */
void free_frame(uint64_t addr)
{
    if (addr == 0) return;
    size_t frame_index = addr / 4096;

    if (frame_index == 0) return;
    Bitmap *bitmap = &frame_allocator.bitmap;
    bitmap_set(bitmap, frame_index, 1);
    frame_allocator.usable_frames++;
}

/* Print memory map */
void print_memory_map(void)
{
    if (!memmap_request.response) return;
    plogk("Physical RAM map:\n");
    plogk(" <MEMMAP>\n");

    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];
        uint64_t base                     = entry->base;
        uint64_t length                   = entry->length;
        uint64_t end                      = base + length - 1;

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
