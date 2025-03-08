/*
 *
 *		common.c
 *		常见设备驱动
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */
 
#include "common.h"

/* 端口写（8位） */
inline void outb(uint16_t port, uint8_t value)
{
	__asm__ __volatile__("outb %1, %0" :: "dN"(port), "a"(value));
}

/* 端口读（8位） */
inline uint8_t inb(uint16_t port)
{
	uint8_t ret;
	__asm__ __volatile__("inb %1, %0" : "=a"(ret) : "dN"(port));
	return ret;
}

/* 端口写（16位） */
inline void outw(uint16_t port, uint16_t value)
{
	__asm__ __volatile__("outw %1, %0" :: "dN"(port), "a"(value));
}

/* 端口读（16位） */
inline uint16_t inw(uint16_t port)
{
	uint16_t ret;
	__asm__ __volatile__("inw %1, %0" : "=a"(ret) : "dN"(port));
	return ret;
}

/* 端口写（32位） */
inline void outl(uint16_t port, uint32_t value)
{
	__asm__ __volatile__("outl %1, %0" :: "dN"(port), "a"(value));
}

/* 端口读（32位） */
inline uint32_t inl(uint16_t port)
{
	uint32_t ret;
	__asm__ __volatile__("inl %1, %0" : "=a"(ret) : "dN"(port));
	return ret;
}

/* 从I/O端口批量地读取数据到内存（16位） */
inline void insw(uint16_t port, void *buf, unsigned long n)
{
	__asm__ __volatile__("cld; rep; insw" : "+D"(buf), "+c"(n) : "d"(port));
}

/* 从内存批量地写入数据到I/O端口（16位） */
inline void outsw(uint16_t port, const void *buf, unsigned long n)
{
	__asm__ __volatile__("cld; rep; outsw" : "+S"(buf), "+c"(n) : "d"(port));
}

/* 从I/O端口批量地读取数据到内存（32位） */
inline void insl(uint32_t port, void *addr, int cnt)
{
	__asm__ __volatile__("cld; repne; insl;" : "=D"(addr), "=c"(cnt)
                         : "d"(port), "0"(addr), "1"(cnt)
                         : "memory", "cc");
}

/* 从内存批量地写入数据到I/O端口（32位） */
inline void outsl(uint32_t port, const void *addr, int cnt)
{
	__asm__ __volatile__("cld; repne; outsl;" : "=S"(addr), "=c"(cnt)
                         : "d"(port), "0"(addr), "1"(cnt)
                         : "memory", "cc");
}

/* 刷新指定地址的TLB */
inline void flush_tlb(uint64_t addr)
{
	__asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}

/* 获取CR3寄存器当前的值 */
inline uint64_t get_cr3(void)
{
	uint64_t cr3;
	__asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
	return cr3;
}

/* 获取RSP寄存器当前的值 */
inline uint64_t get_rsp(void)
{
	uint64_t rsp;
	__asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
	return rsp;
}

/* 获取状态标志寄存器当前的值 */
inline uint64_t get_rflags(void)
{
	uint64_t rflags;
	__asm__ __volatile__("pushfq; pop %0" : "=r"(rflags) :: "memory");
	return rflags;
}

/* 向指定的内存地址写入一个32位的数据 */
inline void mmio_write32(uint32_t *addr, uint32_t data)
{
	*(__volatile__ uint32_t *)addr = data;
}

/* 向指定的内存地址写入一个64位的数据 */
inline void mmio_write64(void *addr, uint64_t data)
{
	*(__volatile__ uint64_t *)addr = data;
}

/* 从指定的内存地址读取一个32位的数据 */
inline uint32_t mmio_read32(void *addr)
{
	return *(__volatile__ uint32_t *)addr;
}

/* 从指定的内存地址读取一个64位的数据 */
inline uint64_t mmio_read64(void *addr)
{
	return *(__volatile__ uint64_t *)addr;
}

/* 读取msr寄存器 */
inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t eax, edx;
	__asm__ __volatile__("rdmsr" : "=a"(eax), "=d"(edx) : "c"(msr));
	return ((uint64_t)edx << 32) | eax;
}

/* 写入msr寄存器 */
inline void wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t eax = (uint32_t)value;
	uint32_t edx = value >> 32;
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"(eax), "d"(edx));
}

/* 原子性地加载数据 */
inline uint64_t load(uint64_t *addr)
{
	uint64_t ret = 0;
	__asm__ __volatile__("lock xadd %[ret], %[addr];"
                         : [addr] "+m"(*addr), [ret] "+r"(ret) :: "memory");
	return ret;
}

/* 原子性地存储数据 */
inline void store(uint64_t *addr, uint32_t value)
{
	__asm__ __volatile__("lock xchg %[value], %[addr];"
                         : [addr] "+m"(*addr), [value] "+r"(value) :: "memory");
}

/* 开启中断 */
inline void enable_intr(void)
{
	__asm__ __volatile__("sti");
}

/* 关闭中断 */
inline void disable_intr(void)
{
	__asm__ __volatile__("cli" ::: "memory");
}

/* 内核停机 */
void krn_halt(void)
{
	disable_intr();
	while (1) __asm__("hlt");
}
