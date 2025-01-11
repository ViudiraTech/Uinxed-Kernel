#include "boot.h"
#include "memory.h"

#define ASSERT(x) (void)(x)

/*
 * frame_info 要实现的功能
 * 1. 能分配连续的物理页 (32位下很多外设只支持直接访问4G以内的物理页，开PAE还能蓝屏)
 * 2. 可用的frame组成双向链表 (参考TLSF设计，根据长度决定在哪个链表里)
 * 3. 正在使用的frame要能记录引用计数
 * 假如使用最直接的写法，双向链接表指针加上长度，一页就要占用12个字节了
 * 这里想办法省掉4个字节。虽然采用bitmap也许可以省掉更多，这里先实现一种简单的方案
 * 考虑到超过1页的内存区域，至少有头尾两个frame_info
 *   为了能合并连续的区间，已经要在尾巴上写长度了
 *   既然如此，不如把next指针挪到尾巴上去，而头上只写prev指针
 * 所有元素都是直接对应物理页的，不用指针而是用物理页的起始地址，最低12位就可以空出来
 * 这样每个frame_info都有两个字段
 * - boundary 指向这个区域的另一端，头指向尾，尾指向头
 * - sibling 承担双向链表指针的功能，头指向prev，尾指向next
 * 当frame处于inuse状态时，头部的sibling字段用作引用计数
 * 因为只有1页的内存区域，不需要记录长度，两个字段分别存prev和next就可以了
 */

#define FI_TAG_INUSE 0x10
#define FI_TAG_START 0x2
#define FI_TAG_END 0x4
#define FI_TAG_SINGLE_PAGE 0x8
#define FI_ADDR_MASK 0xFFFFF000
#define FI_INDEX_SHIFT 12

struct frame_info {
	union {
		uintptr_t tag;
		uintptr_t boundary;
		uintptr_t prev;
	};
	union {
		uintptr_t sibling;
		uintptr_t refcount;
		uintptr_t next;
	};
};

size_t nframes = 0;
extern struct frame_info frame_infos[];

/*
 * 参考 TLSF 设计
 * 后一个slot大小是前一个slot的两倍
 * 每个slot里元素大小都大于等于slot声称的大小
 * i=0 是 4K, i=1 是 8K, ..., i=19 是 2G
 * BIOS 会占用 [mem_lower*1K, 1M) 这个区间的地址
 * 目前直接依靠 mem_upper 判断1M以后的空间，不可能出现4G或更大的区间
 */
#define NSLOTS 20
static uintptr_t slots[NSLOTS] = {0};

static void frame_info_clear(struct frame_info *info) {
	info->boundary = 0;
	info->sibling = 0;
}

static void frame_info_init(size_t head_index, size_t tail_index) {
	if (head_index == tail_index) {
		frame_infos[head_index].tag = FI_TAG_SINGLE_PAGE;
	} else {
		frame_infos[head_index].boundary = (tail_index << FI_INDEX_SHIFT) | FI_TAG_START;
		frame_infos[tail_index].boundary = (head_index << FI_INDEX_SHIFT) | FI_TAG_END;
	}
}

static void frame_info_init_inuse(size_t head_index, size_t tail_index) {
	frame_info_init(head_index, tail_index);
	frame_infos[head_index].tag |= FI_TAG_INUSE;
	frame_infos[tail_index].tag |= FI_TAG_INUSE;
	frame_infos[head_index].refcount = 0;
}

static size_t frame_info_head(size_t index) {
	struct frame_info *info = frame_infos + index;
	if (info->tag & FI_TAG_SINGLE_PAGE)
		return index;

	ASSERT(info->tag & FI_TAG_END);
	return (info->boundary >> FI_INDEX_SHIFT);
}

static size_t frame_info_tail(size_t index) {
	struct frame_info *info = frame_infos + index;
	if (info->tag & FI_TAG_SINGLE_PAGE)
		return index;

	ASSERT(info->tag & FI_TAG_START);
	return (info->boundary >> FI_INDEX_SHIFT);
}

static uintptr_t frame_list_prev(struct frame_info *info) {
	ASSERT(!(info->tag & FI_TAG_INUSE));
	if (info->tag & FI_TAG_SINGLE_PAGE)
		return info->prev;

	ASSERT(info->tag & FI_TAG_START);
	return info->sibling;
}

static uintptr_t frame_list_next(struct frame_info *info) {
	ASSERT(!(info->tag & FI_TAG_INUSE));
	if (info->tag & FI_TAG_SINGLE_PAGE)
		return info->next;

	ASSERT(info->tag & FI_TAG_END);
	return info->sibling;
}

static void frame_list_set_prev(size_t index, uintptr_t addr) {
	struct frame_info *info = frame_infos + index;
	ASSERT(!(info->tag & FI_TAG_INUSE));

	addr &= FI_ADDR_MASK | FRAMEINFO_NONNULL;
	if (info->tag & FI_TAG_SINGLE_PAGE) {
		info->prev = addr | FI_TAG_SINGLE_PAGE;
	} else {
		ASSERT(info->tag & FI_TAG_START);
		info->sibling = addr;
	}
}

static void frame_list_set_next(size_t index, uintptr_t addr) {
	struct frame_info *info = frame_infos + index;
	ASSERT(!(info->tag & FI_TAG_INUSE));

	addr &= FI_ADDR_MASK | FRAMEINFO_NONNULL;
	if (info->tag & FI_TAG_SINGLE_PAGE) {
		info->next = addr;
	} else {
		ASSERT(info->tag & FI_TAG_END);
		info->sibling = addr;
	}
}

static size_t slot_number(size_t npages) {
	size_t slot = 0;
	npages -= 1;
	for (; npages; npages >>= 1)
		++slot;
	return slot;
}

static size_t frame_slot_number(size_t head_index, size_t tail_index) {
	size_t npages = tail_index - head_index + 1;
	size_t slot = 0;
	for (; npages; npages >>= 1)
		++slot;
	return slot - 1;
}

static void frame_list_add(size_t head_index, size_t tail_index) {
	ASSERT(!(frame_infos[head_index].tag & FI_TAG_INUSE));
	ASSERT(!(frame_infos[tail_index].tag & FI_TAG_INUSE));

	size_t slot = frame_slot_number(head_index, tail_index);
	frame_info_clear(frame_infos + head_index);
	frame_info_clear(frame_infos + tail_index);
	frame_info_init(head_index, tail_index);

	uintptr_t first_addr = slots[slot];
	if (first_addr & FRAMEINFO_NONNULL) {
		frame_list_set_prev(first_addr >> FI_INDEX_SHIFT, (tail_index << FI_INDEX_SHIFT) | FRAMEINFO_NONNULL);
		frame_list_set_next(tail_index, first_addr);
	} else {
		frame_list_set_next(tail_index, 0);
	}
	frame_list_set_prev(head_index, 0);
	slots[slot] = (head_index << FI_INDEX_SHIFT) | FRAMEINFO_NONNULL;
}

static void frame_list_remove(size_t head_index, size_t tail_index) {
	ASSERT(!(frame_infos[head_index].tag & FI_TAG_INUSE));
	ASSERT(!(frame_infos[tail_index].tag & FI_TAG_INUSE));
	uintptr_t prev_addr = frame_list_prev(frame_infos + head_index);
	uintptr_t next_addr = frame_list_next(frame_infos + tail_index);
	if (prev_addr & FRAMEINFO_NONNULL) {
		frame_list_set_next(prev_addr >> FI_INDEX_SHIFT, next_addr);
	} else {
		size_t slot = frame_slot_number(head_index, tail_index);
		ASSERT((slots[slot] >> FI_INDEX_SHIFT) == head_index);
		slots[slot] = next_addr;
	}

	if (next_addr & FRAMEINFO_NONNULL)
		frame_list_set_prev(next_addr >> FI_INDEX_SHIFT, prev_addr);
}

static uintptr_t frame_alloc_from_slot(size_t slot, size_t npages) {
	uintptr_t first_addr = slots[slot];
	ASSERT(first_addr & FRAMEINFO_NONNULL);
	size_t head_index = first_addr >> FI_INDEX_SHIFT;
	size_t tail_index = frame_info_tail(head_index);
	frame_list_remove(head_index, tail_index);

	if (head_index + npages - 1 < tail_index) {
		frame_list_add(head_index + npages, tail_index);
	} else {
		uintptr_t next_addr = frame_list_next(frame_infos + tail_index);
		slots[slot] = next_addr;
	}

	tail_index = head_index + npages - 1;
	frame_info_init_inuse(head_index, tail_index);
	return (head_index << FI_INDEX_SHIFT) | FRAMEINFO_NONNULL;
}

uintptr_t frame_alloc(size_t npages) {
	size_t slot = slot_number(npages);
	for ( ; slot < NSLOTS; ++slot)
		if (slots[slot] & FRAMEINFO_NONNULL)
			return frame_alloc_from_slot(slot, npages);
	return 0;
}

void frame_free(uintptr_t addr) {
	size_t index = addr >> FI_INDEX_SHIFT;
	struct frame_info *info = frame_infos + index;
	uintptr_t tag = info->tag;
	uintptr_t refcount = info->refcount;
	if (!(tag & FI_TAG_INUSE) || (refcount > 0)) {
		ASSERT(0);
		return;
	}

	size_t head_index = index;
	struct frame_info *prev = info - 1;
	if (!(prev->tag & FI_TAG_INUSE)) {
		// 和前一项合并
		head_index = frame_info_head(index - 1);
		frame_list_remove(head_index, index - 1);
		frame_info_clear(prev);
	}

	index = frame_info_tail(index);
	size_t tail_index = index;
	struct frame_info *next = frame_infos + (index + 1);
	if (!(next->tag & FI_TAG_INUSE)) {
		// 和后一项合并
		tail_index = frame_info_tail(index + 1);
		frame_list_remove(index + 1, tail_index);
		frame_info_clear(next);
	}

	frame_info_clear(info);
	frame_info_clear(frame_infos + index);
	frame_list_add(head_index, tail_index);
}

#define MAX_RECLAIMABLES 3
size_t nreclaimables = 0;
struct memory_region reclaimables[MAX_RECLAIMABLES] = {0};

static void init_reclaimables(void) {
	size_t count = 0;
	struct memory_region *all_regions[MAX_RECLAIMABLES] = {&boot_info.initrd, &boot_info.symtab, &boot_info.strtab};
	struct memory_region *regions[MAX_RECLAIMABLES];

	for (size_t i=0; i<MAX_RECLAIMABLES; ++i) {
		if (all_regions[i]->end > all_regions[i]->start) {
			regions[count] = all_regions[i];
			count += 1;
		}
	}

	if (count == 0)
		return;

	// 排序
	for (size_t i=0; i+1<count; ++i) {
		size_t min = i;
		for (size_t j=i+1; j<count; ++j)
			if (regions[j]->start < regions[min]->start)
				min = j;
		if (min == i)
			continue;
		struct memory_region *region_min = regions[min];
		regions[min] = regions[i];
		regions[i] = region_min;
	}

	struct memory_region aligned[MAX_RECLAIMABLES] = {0};

	// 对齐到页边界
	for (size_t i=0; i<count; ++i) {
	  aligned[i].start = align_down(regions[i]->start, PAGE_SIZE);
	  aligned[i].end = align_up(regions[i]->end, PAGE_SIZE);
	}

	// 合并连续区间
	reclaimables[0] = aligned[0];
	for (size_t i=1; i<count; ++i) {
	  if (aligned[i].start <= reclaimables[nreclaimables-1].end) {
	    reclaimables[nreclaimables-1].end = aligned[i].end;
	  } else {
	    reclaimables[nreclaimables++] = aligned[i];
	  }
	}
}

extern struct frame_info __frame_info_before;
extern struct frame_info __frame_info_after;
extern char __kernel_end[]; // 注意这是LMA

void
init_frame(void) {
	init_reclaimables();

	// 指的是从1M开始有mem_upper K内存，一页是4K
	nframes = (0x100000/0x1000) + (boot_info.mem_upper/4);
	program_break = align_up((uintptr_t)(frame_infos+nframes+1), PAGE_SIZE);

	kh_usage_memory_byte += ((uintptr_t)__kernel_end) - KERNEL_LMA_BASE;
	kh_usage_memory_byte -= ((uintptr_t)__frame_infos_end) - program_break;

	struct memory_region initial_regions[3] = {
		{ .start = 0,
		  .end = boot_info.mem_lower * 1024 },
		{ .start = VMA2LMA(program_break),
		  .end = VMA2LMA(__frame_infos_end) },
		{ .start = (uintptr_t)__kernel_end,
		  .end = 0x100000 + boot_info.mem_upper * 1024
		},
	};

	size_t count = 0; // regions
	size_t n = 0; // reclaimables
	struct memory_region regions[3+MAX_RECLAIMABLES] = {0};

	for (size_t i=0; i<3; ++i) {
		uintptr_t start = initial_regions[i].start;
		uintptr_t end = initial_regions[i].end;

		for ( ; n < nreclaimables; ++n) {
			if (reclaimables[n].start >= end)
				break;

			if (start < reclaimables[n].start) {
				regions[count].start = start;
				regions[count].end = reclaimables[n].start;
				++count;
			}

			start = reclaimables[n].end;
		}

		if (end > start) {
			regions[count].start = start;
			regions[count].end = end;
			++count;
		}
	}

	// 头尾额外放一个，合并可用块时就不用特殊判断了
	__frame_info_before.tag = FI_TAG_INUSE;
	__frame_info_after.tag = FI_TAG_INUSE;
	for (size_t i=0; i<nframes; ++i)
		frame_info_clear(frame_infos+i);

	frame_list_add(regions[0].start>>FI_INDEX_SHIFT, (regions[0].end>>FI_INDEX_SHIFT)-1);
	for (size_t i=1; i<count; ++i) {
		frame_info_init_inuse(regions[i-1].end>>FI_INDEX_SHIFT, (regions[i].start>>FI_INDEX_SHIFT)-1);
		frame_list_add(regions[i].start>>FI_INDEX_SHIFT, (regions[i].end>>FI_INDEX_SHIFT)-1);
	}
}
