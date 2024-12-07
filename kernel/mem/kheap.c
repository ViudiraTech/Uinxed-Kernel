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

#define getpagesize() PAGE_SIZE
#define MAGIC 0xef
#define RMAGIC 0x5555
#define RSLOP 0
#define NBUCKETS 30
#define bcopy(a, b, c) memcpy(b,a,c)
#define ASSERT(S)

static int realloc_srchlen = 4;
extern uint32_t kh_usage_memory_byte;

static union overhead *nextf[NBUCKETS];
static void morecore(int bucket);
static int findbucket(union overhead *freep, int srchlen);
static int pagesz;
static int pagebucket;

void *program_break = (void*)0x3e0000;
void *program_break_end;

/* 开启分页机制后的内核栈 */
char kern_stack[STACK_SIZE] __attribute__ ((aligned(16)));

/* 栈顶 */
uint32_t kern_stack_top = ((uint32_t)kern_stack + STACK_SIZE);

/* 内核堆扩容措施 */
static void *sbrk(int incr)
{
	if (program_break == 0) {
		return (void *) -1;
	}
	if (program_break + incr >= program_break_end) {
		ral:
		if(program_break_end >= (void*)0x01bf8f7d) goto alloc_error;
		if ((uint32_t) program_break_end < USER_AREA_START) {
			uint32_t ai = (uint32_t)program_break_end;
			for (; ai < (uint32_t)program_break_end + PAGE_SIZE * 10;) {
				alloc_frame(get_page(ai, 1, kernel_directory), 1, 0);
				ai += PAGE_SIZE;
			}
			program_break_end = (void *) ai;
			if (program_break + incr >= program_break_end) {
				goto ral;
			}
		} else {
			alloc_error:
			char *s = 0;
			sprintf(s, "%s 0x%08X", P010, program_break_end);
			panic(s);
			return (void *) -1;
		}
	}
	void *prev_break = program_break;
	program_break += incr;
	return prev_break;
}

/* 分配内存并返回地址 */
void *kmalloc(size_t nbytes)
{
	register union overhead *op;
	register int bucket;
	register long n;
	register unsigned amt;
	if (pagesz == 0) {
		pagesz = n = getpagesize();
		op = (union overhead *) sbrk(0);
		n = n - sizeof(*op) - ((long) op & (n - 1));
		if (n < 0)
			n += pagesz;
		if (n) {
			if (sbrk(n) == (char *) -1) {
				return (0);
			}
		}
		bucket = 0;
		amt = 8;
		while (pagesz > amt) {
			amt <<= 1;
			bucket++;
		}
		pagebucket = bucket;
	}
	if (nbytes <= (n = pagesz - sizeof(*op) - RSLOP)) {
		amt = 8;
		bucket = 0;
		n = -((long) sizeof(*op) + RSLOP);
	} else {
		amt = pagesz;
		bucket = pagebucket;
	}
	while (nbytes > amt + n) {
		amt <<= 1;
		if (amt == 0) {
			return (0);
		}
		bucket++;
	}
	if ((op = nextf[bucket]) == 0) {
		morecore(bucket);
		if ((op = nextf[bucket]) == 0) {
			return (0);
		}
	}
	nextf[bucket] = op->ov_next;
	op->ov_magic = MAGIC;
	op->ov_index = bucket;
	kh_usage_memory_byte += bucket;
	return ((char *) (op + 1));
}

/* 分配内存并返回清零后的内存地址 */
void *kcalloc(size_t nelem, size_t elsize)
{
	void *ptr = kmalloc(nelem * elsize);
	memset(ptr, 0, nelem * elsize);
	return ptr;
}

/* 分配更多的空间给内存分配器 */
static void morecore(int bucket)
{
	register union overhead *op;
	register long sz;
	long amt;
	int nblks;

	sz = 1 << (bucket + 3);
	if (sz <= 0)
		return;
	if (sz < pagesz) {
		amt = pagesz;
		nblks = amt / sz;
	} else {
		amt = sz + pagesz;
		nblks = 1;
	}
	op = (union overhead *) sbrk(amt);
	if ((long) op == -1)
		return;
	nextf[bucket] = op;
	while (--nblks > 0) {
		op->ov_next = (union overhead *) ((size_t) op + sz);
		op = (union overhead *) ((size_t) op + sz);
	}
}

/* 释放分配的内存并合并 */
void kfree(void *cp)
{
	register long size;
	register union overhead *op;

	if (cp == 0)
		return;
	op = (union overhead *) ((size_t) cp - sizeof(union overhead));
	if (op->ov_magic != MAGIC)
		return;
	size = op->ov_index;
	ASSERT(size < NBUCKETS);
	op->ov_next = nextf[size];
	nextf[size] = op;
	kh_usage_memory_byte -= size;
}

/* 计算分配的内存块的实际可用大小 */
size_t kmalloc_usable_size(void *cp)
{
	register union overhead *op;
	if (cp == 0)
		return 0;
	op = (union overhead *) ((size_t) cp - sizeof(union overhead));
	if (op->ov_magic != MAGIC)
		return 0;
	return op->ov_index;
}

/* 重新分配内存区域 */
void *krealloc(void *cp, size_t nbytes)
{
	register unsigned long onb;
	register long i;
	union overhead *op;
	char *res;
	int was_alloced = 0;
	
	if (cp == 0)
		return (kmalloc(nbytes));
	if (nbytes == 0) {
		kfree(cp);
		return 0;
	}
	op = (union overhead *) ((size_t) cp - sizeof(union overhead));
	if (op->ov_magic == MAGIC) {
		was_alloced++;
		i = op->ov_index;
	} else {
		if ((i = findbucket(op, 1)) < 0 &&
			(i = findbucket(op, realloc_srchlen)) < 0)
			i = NBUCKETS;
	}
	onb = 1 << (i + 3);
	if (onb < pagesz)
		onb -= sizeof(*op) + RSLOP;
	else
		onb += pagesz - sizeof(*op) - RSLOP;
	if (was_alloced) {
		if (i) {
			i = 1 << (i + 2);
			if (i < pagesz)
				i -= sizeof(*op) + RSLOP;
			else
				i += pagesz - sizeof(*op) - RSLOP;
		}
		if (nbytes <= onb && nbytes > i) {
			return (cp);
		} else
			kfree(cp);
	}
	if ((res = kmalloc(nbytes)) == 0)
		return (0);
	if (cp != res)
		bcopy(cp, (uint8_t *)res, (nbytes < onb) ? nbytes : onb);
	return (res);
}

/* 查找特定内存块所在的bucket */
static int findbucket(union overhead *freep, int srchlen)
{
	register union overhead *p;
	register int i, j;

	for (i = 0; i < NBUCKETS; i++) {
		j = 0;
		for (p = nextf[i]; p && j != srchlen; p = p->ov_next) {
			if (p == freep)
				return (i);
			j++;
		}
	}
	return (-1);
}
