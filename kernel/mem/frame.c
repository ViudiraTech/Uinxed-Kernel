/*
 *
 *		frame.c
 *		内存帧
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "frame.h"
#include "printk.h"
#include "hhdm.h"
#include "limine.h"

__attribute__((used, section(".limine_requests")))
static __volatile__ struct limine_memmap_request memmap_request = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0,
};

FrameAllocator frame_allocator;
uint64_t memory_size = 0;

/* 初始化内存帧 */
void init_frame(void)
{
	struct limine_memmap_response *memory_map = memmap_request.response;
	for (uint64_t i = memory_map->entry_count - 1; i != 0; i--) {
		struct limine_memmap_entry *region = memory_map->entries[i];
		if (region->type == LIMINE_MEMMAP_USABLE) {
			memory_size = region->base + region->length;
			plogk("Frame Allocator: Found usable memory region at base 0x%08x, length 0x%08x\n", region->base, region->length);
			break;
		}
	}
	unsigned long bitmap_size = (memory_size / 4096 + 7) / 8;
	uint64_t bitmap_address = 0;

	for (uint64_t i = 0; i < memory_map->entry_count; i++) {
		struct limine_memmap_entry *region = memory_map->entries[i];
		if (region->type == LIMINE_MEMMAP_USABLE && region->length >= bitmap_size) {
			bitmap_address = region->base;
			plogk("Frame Allocator: Bitmap allocated at base 0x%08x, size 0x%08x\n", bitmap_address, bitmap_size);
			break;
		}
	}
	if (!bitmap_address) {
		plogk("Frame Allocator: Failed to allocate bitmap memory!\n");
		return;
	}
	Bitmap *bitmap = &frame_allocator.bitmap;
	bitmap_init(bitmap, phys_to_virt(bitmap_address), bitmap_size);
	unsigned long origin_frames = 0;

	for (uint64_t i = 0; i < memory_map->entry_count; i++) {
		struct limine_memmap_entry *region = memory_map->entries[i];
		if (region->type == LIMINE_MEMMAP_USABLE) {
			unsigned long start_frame = region->base / 4096;
			unsigned long frame_count = region->length / 4096;
			origin_frames += frame_count;
			bitmap_set_range(bitmap, start_frame, start_frame + frame_count, 1);
			plogk("Frame Allocator: Marked %06lu frames starting at frame %06lu as usable\n", frame_count, start_frame);
		}
	}
	unsigned long bitmap_frame_start = bitmap_address / 4096;
	unsigned long bitmap_frame_count = (bitmap_size + 4095) / 4096;
	unsigned long bitmap_frame_end = bitmap_frame_start + bitmap_frame_count;
	bitmap_set_range(bitmap, bitmap_frame_start, bitmap_frame_end, 0);
	plogk("Frame Allocator: Reserved %06lu frames for bitmap starting at frame %06lu\n", bitmap_frame_count, bitmap_frame_start);
	frame_allocator.origin_frames = origin_frames;
	frame_allocator.usable_frames = origin_frames - bitmap_frame_count;
	plogk("Frame Allocator: %lu frames\n", origin_frames);
}

/* 分配内存帧 */
uint64_t alloc_frames(unsigned long count)
{
	Bitmap *bitmap = &frame_allocator.bitmap;
	unsigned long frame_index = bitmap_find_range(bitmap, count, 1);

	if (frame_index == (unsigned long)-1) return 0;
	bitmap_set_range(bitmap, frame_index, frame_index + count, 0);
	frame_allocator.usable_frames -= count;
	return frame_index * 4096;
}
