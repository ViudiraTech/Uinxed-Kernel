/*
 *
 *		kheap.c
 *		内核内存分配器
 *
 *		2024/12/7 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "memory.h"
#include "printk.h"
#include "string.h"
#include "debug.h"

#define ASSERT(S)

/* 基于BSD 4.2 malloc() 的设计
 * 总共有 NBUCKETS (30) 个bucket。这里把bucket序号记作i，从0开始编号
 * bucket i里所有块长度都是 2^(i+3) 个字节
 * 不开启 range check (#ifdef RCHECK) 时
 * 每个块的可用长度为 2^(i+3) - 4
 * 每个块开头四个字节用于存放块元信息
 * 未分配时是指向下一个可用块的指针
 * 分配时只有前两个字节有用
 * - 第一个字节为 MAGIC (0xff)
 * - 第二个字节为 ov_index 即bucket序号i
 * 后两个字节用于保证返回的指针是四个字节对齐的
 *            ┌────────────────────────────────┐
 *            │<-    2^(ov_index+3) bytes    ->│
 *            │┌─────┬─ ov_next (when free)    │
 *            ├┴┬─┬─┬┴┬─┬─┬─┬─┬────────────────┤
 * when used: └┬┴┬┴┬┴┬┴┬┴─┴─┴┬┴────────────────┘
 *  ov_magic ──┘ │ │ │ └─────┴─ ov_magic
 *  ov_index ────┘ └─┴─ ov_size
 *                 #ifdef RCHECK
 * 开启 range check 时，在可用区域头尾各放四个字节 RMAGIC (0x55555555)
 * 可用长度为 2^(i+3) - 12
 */
/*
 * malloc.c (Caltech) 2/21/82
 * Chris Kingsley, kingsley@cit-20.
 *
 * This is a very fast storage allocator.  It allocates blocks of a small
 * number of different sizes, and keeps free lists of each size.  Blocks that
 * don't exactly fit are passed up to the next larger size.  In this
 * implementation, the available sizes are 2^n-4 (or 2^n-12) bytes long.
 * This is designed for use in a program that uses vast quantities of memory,
 * but bombs when it runs out.
 */

/*
 * The overhead on a block is at least 4 bytes.  When free, this space
 * contains a pointer to the next free block, and the bottom two bits must
 * be zero.  When in use, the first byte is set to MAGIC, and the second
 * byte is the size index.  The remaining bytes are for alignment.
 * If range checking is enabled and the size of the block fits
 * in two bytes, then the top two bytes hold the size of the requested block
 * plus the range checking words, and the header word MINUS ONE.
 */
union overhead {
	union overhead *ov_next;	/* when free */
	struct {
		uint8_t ov_magic;	/* magic number */
		uint8_t ov_index;	/* bucket # */
#ifdef RCHECK
		uint16_t ov_size;	/* actual block size */
		uint32_t ov_magic;	/* range magic number */
#endif
	};
};

#define MAGIC	0xff			/* magic # on accounting info */
#define RMAGIC	0x55555555		/* magic # on range info */
#ifdef RCHECK
#define RSLOP	sizeof(uint32_t)
#else
#define RSLOP	0
#endif

/*
 * nextf[i] is the pointer to the next free block of size 2^(i+3).  The
 * smallest allocatable block is 8 bytes.  The overhead information
 * precedes the data area returned to the user.
 */
#define NBUCKETS 30
static union overhead *nextf[NBUCKETS];


#ifdef MSTATS
/*
 * nmalloc[i] is the difference between the number of mallocs and frees
 * for a given block size.
 */
static size_t nmalloc[NBUCKETS];
#endif

static void morecore(size_t bucket);

uintptr_t program_break;
uintptr_t program_break_end = KERNEL_STACK_BASE;

/* 内核堆扩容措施 */
static void *sbrk(size_t incr)
{
	uintptr_t next_program_break = program_break + incr;
	if (next_program_break >= program_break_end)
		return (void*)-1;
	void *result = (void *)program_break;
	for ( ; program_break < next_program_break; program_break += PAGE_SIZE) {
		page_alloc(program_break, PT_W);
	}
	memset(result, 0, incr);
	return result;
}

/* 分配内存并返回地址 */
void *kmalloc(size_t nbytes)
{
	/*
	 * Convert amount of memory requested into
	 * closest block size stored in hash buckets
	 * which satisfies request.  Account for
	 * space used per block for accounting.
	 */
	/* 注: #define RCHECK 导致sizeof(union overhead)变大 */
	nbytes += sizeof(union overhead) + RSLOP;
	nbytes = (nbytes + 3) &~ 3; /* 整4字节 */

	size_t bucket = 0;
	size_t shiftr = (nbytes - 1) >> 2;
	/* apart from this loop, this is O(1) */
	while (shiftr >>= 1)
		bucket++;

	/*
	 * If nothing in hash bucket right now,
	 * request more memory from the system.
	 */
	if (nextf[bucket] == NULL)
		morecore(bucket);
	union overhead *p = nextf[bucket];
	if (p == NULL)
		return NULL;
	/* remove from linked list */
	nextf[bucket] = nextf[bucket]->ov_next;
	p->ov_magic = MAGIC;
	p->ov_index= bucket;
#ifdef MSTATS
	nmalloc[bucket]++;
#endif
#ifdef RCHECK
	/*
	 * Record allocated size of block and
	 * bound space with magic numbers.
	 */
	/* 就算申请0字节，nbytes也大于0。因此ov_size里可以填nbytes-1 */
	if (nbytes <= 0x10000)
		p->ov_size = nbytes - 1;
	p->ov_rmagic = RMAGIC;
	*((uint32_t *)((uintptr_t)p + nbytes - RSLOP)) = RMAGIC;
#endif
	kh_usage_memory_byte += 1UL << (bucket + 3);
	return ((char *)(p + 1));
}

/* 分配内存并返回清零后的内存地址 */
void *kcalloc(size_t nelem, size_t elsize)
{
	void *ptr = kmalloc(nelem * elsize);
	memset(ptr, 0, nelem * elsize);
	return ptr;
}

/* 分配更多的空间给内存分配器 */
static void morecore(size_t bucket)
{
	if (nextf[bucket])
		return;

	size_t sz = 1 << (bucket + 3);
	size_t nblks;
	size_t amt;

	/* 这里对祖传代码进行简化，至少分配一整页 */
	if (sz < PAGE_SIZE) {
		amt = PAGE_SIZE;
		nblks = PAGE_SIZE / sz;
	} else {
		amt = sz;
		nblks = 1;
	}

	union overhead *op = (union overhead *) sbrk(amt);
	if (op == (void *)-1)
		return;

	/*
	 * Add new memory allocated to that on
	 * free list for this hash bucket.
	 */
	nextf[bucket] = op;
	while (--nblks > 0) {
		op->ov_next = (union overhead *)((uintptr_t)op + sz);
		op = (union overhead *)((uintptr_t)op + sz);
	}
	op->ov_next = NULL;
}

/* 释放分配的内存并合并 */
void kfree(void *cp)
{
	if (cp == NULL)
		return;
	union overhead *op = (union overhead *)((uintptr_t)cp - sizeof(union overhead));
	ASSERT(op->ov_magic == MAGIC);		/* make sure it was in use */
	if (op->ov_magic != MAGIC)
		return;			/* sanity */
#ifdef RCHECK
	ASSERT(op->ov_rmagic == RMAGIC);
	if (op->ov_index <= 13)
		ASSERT(*(uint32_t *)((uintptr_t)op + op->ov_size + 1 - RSLOP) == RMAGIC);
#endif
	ASSERT(op->ov_index < NBUCKETS);
	size_t bucket = op->ov_index;
	op->ov_next = nextf[bucket];
	nextf[bucket] = op;
#ifdef MSTATS
	nmalloc[bucket]--;
#endif
	kh_usage_memory_byte -= 1UL << (bucket + 3);
}

/* 计算分配的内存块的实际可用大小 */
size_t kmalloc_usable_size(void *cp)
{
	if (cp == NULL)
		return 0;
	register union overhead *op;
	op = ((union overhead *)cp) - 1;
	if (op->ov_magic != MAGIC)
		return 0;
	size_t i = op->ov_index;
	return (1 << (i+3)) - sizeof(union overhead) - RSLOP;
}

/* 重新分配内存区域 */
void *krealloc(void *cp, size_t nbytes)
{
	if (cp == NULL)
		return (kmalloc(nbytes));

	union overhead *op = (union overhead *)((uintptr_t)cp - sizeof(union overhead));
	if (op->ov_magic != MAGIC) {
		/* V7的realloc有点特殊，文档里是这么说的
		 * Realloc also works if ptr points to a block freed since the last call of malloc, realloc or calloc;
		 * thus sequences of free, malloc and realloc can exploit the search strategy of malloc to do storage compaction.
		 * 4.2BSD的realloc为了兼容这种行为增加了一些代码
		 * C语言标准早已经不支持这种做法了，因此在这里把这部分代码删了，不作判断直接出错
		 */
		ASSERT(0);
		return NULL;
	}

	size_t i = op->ov_index;
	size_t bytes_required = nbytes + sizeof(union overhead) + RSLOP;
	/* avoid the copy if same size block */
	if ((bytes_required <= (1 << (i + 3))) && (bytes_required > (1 << (i + 2))))
		return cp;

	void *res = kmalloc(nbytes);
	if (res == NULL) {
		/* 根据C语言标准，除非malloc成功，不能对传入的区域进行写操作 */
		/* 这里只能直接返回空指针 */
		return NULL;
	}
	size_t old_nbytes = kmalloc_usable_size(cp);
	/* realloc既可能扩容也可能缩容，*/
	memcpy(res, cp, (nbytes<old_nbytes)?nbytes:old_nbytes);
	kfree(cp);
	return res;
}
