/*
 *
 *		common.c
 *		Common device
 *
 *		2024/6/27 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "common.h"

/* Port write (8 bits) */
inline void outb(uint16_t port, uint8_t value)
{
	__asm__ __volatile__("outb %1, %0" :: "dN"(port), "a"(value));
}

/* Port read (8 bits) */
inline uint8_t inb(uint16_t port)
{
	uint8_t ret;
	__asm__ __volatile__("inb %1, %0" : "=a"(ret) : "dN"(port));
	return ret;
}

/* Port write (16 bits) */
inline void outw(uint16_t port, uint16_t value)
{
	__asm__ __volatile__("outw %1, %0" :: "dN"(port), "a"(value));
}

/* Port read (16 bits) */
inline uint16_t inw(uint16_t port)
{
	uint16_t ret;
	__asm__ __volatile__("inw %1, %0" : "=a"(ret) : "dN"(port));
	return ret;
}

/* Port write (32 bits) */
inline void outl(uint16_t port, uint32_t value)
{
	__asm__ __volatile__("outl %1, %0" :: "dN"(port), "a"(value));
}

/* Port read (32 bits) */
inline uint32_t inl(uint16_t port)
{
	uint32_t ret;
	__asm__ __volatile__("inl %1, %0" : "=a"(ret) : "dN"(port));
	return ret;
}

/* Read data from I/O port to memory in batches (16 bits) */
inline void insw(uint16_t port, void *buf, unsigned long n)
{
	__asm__ __volatile__("cld; rep; insw" : "+D"(buf), "+c"(n) : "d"(port));
}

/* Write data from memory to I/O port in batches (16 bits) */
inline void outsw(uint16_t port, const void *buf, unsigned long n)
{
	__asm__ __volatile__("cld; rep; outsw" : "+S"(buf), "+c"(n) : "d"(port));
}

/* Read data from I/O port to memory in batches (32 bits) */
inline void insl(uint32_t port, void *addr, int cnt)
{
	__asm__ __volatile__("cld; repne; insl;" : "=D"(addr), "=c"(cnt)
                         : "d"(port), "0"(addr), "1"(cnt)
                         : "memory", "cc");
}

/* Write data from memory to I/O port in batches (32 bits) */
inline void outsl(uint32_t port, const void *addr, int cnt)
{
	__asm__ __volatile__("cld; repne; outsl;" : "=S"(addr), "=c"(cnt)
                         : "d"(port), "0"(addr), "1"(cnt)
                         : "memory", "cc");
}

/* Flushes the TLB of the specified address */
inline void flush_tlb(uint64_t addr)
{
	__asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}

/* Get the current value of the CR3 register */
inline uint64_t get_cr3(void)
{
	uint64_t cr3;
	__asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
	return cr3;
}

/* Get the current value of the RSP register */
inline uint64_t get_rsp(void)
{
	uint64_t rsp;
	__asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
	return rsp;
}

/* Get the current value of the status flag register */
inline uint64_t get_rflags(void)
{
	uint64_t rflags;
	__asm__ __volatile__("pushfq; pop %0" : "=r"(rflags) :: "memory");
	return rflags;
}

/* Write a 32-bit data to the specified memory address */
inline void mmio_write32(uint32_t *addr, uint32_t data)
{
	*(__volatile__ uint32_t *)addr = data;
}

/* Write a 64-bit data to the specified memory address */
inline void mmio_write64(void *addr, uint64_t data)
{
	*(__volatile__ uint64_t *)addr = data;
}

/* Read a 32-bit data from the specified memory address */
inline uint32_t mmio_read32(void *addr)
{
	return *(__volatile__ uint32_t *)addr;
}

/* Read a 64-bit data from the specified memory address */
inline uint64_t mmio_read64(void *addr)
{
	return *(__volatile__ uint64_t *)addr;
}

/* Read msr register */
inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t rax, rdx;
	__asm__ __volatile__("rdmsr" : "=a"(rax), "=d"(rdx) : "c"(msr));
	return ((uint64_t)rdx << 32) | rax;
}

/* Write to msr register */
inline void wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t rax = (uint32_t)value;
	uint32_t rdx = value >> 32;
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"(rax), "d"(rdx));
}

/* Loading data atomically */
inline uint64_t load(uint64_t *addr)
{
	uint64_t ret = 0;
	__asm__ __volatile__("lock xadd %[ret], %[addr];"
                         : [addr] "+m"(*addr), [ret] "+r"(ret) :: "memory");
	return ret;
}

/* Storing data atomically */
inline void store(uint64_t *addr, uint32_t value)
{
	__asm__ __volatile__("lock xchg %[value], %[addr];"
                         : [addr] "+m"(*addr), [value] "+r"(value) :: "memory");
}

/* Enable interrupt */
inline void enable_intr(void)
{
	__asm__ __volatile__("sti");
}

/* Disable interrupts */
inline void disable_intr(void)
{
	__asm__ __volatile__("cli" ::: "memory");
}

/* Kernel halt */
void krn_halt(void)
{
	disable_intr();
	while (1) __asm__("hlt");
}
