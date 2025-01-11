/*
 *
 *		common.c
 *		常见设备驱动
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "common.h"
#include "cpu.h"

/* 端口写（8位） */
inline void outb(uint16_t port, uint8_t value)
{
	__asm__ __volatile__("outb %1, %0" : : "dN" (port), "a" (value));
}

/* 端口读（8位） */
inline uint8_t inb(uint16_t port)
{
	uint8_t ret;
	__asm__ __volatile__("inb %1, %0" : "=a" (ret) : "dN" (port));
	return ret;
}

/* 端口写（16位） */
inline void outw(uint16_t port, uint16_t value)
{
	__asm__ __volatile__("outw %1, %0" : : "dN" (port), "a" (value));
}

/* 端口读（16位） */
inline uint16_t inw(uint16_t port)
{
	uint16_t ret;
	__asm__ __volatile__("inw %1, %0" : "=a" (ret) : "dN" (port));
	return ret;
}

/* 端口写（32位） */
inline void outl(uint16_t port, uint32_t value)
{
	__asm__ __volatile__("outl %1, %0" : : "dN"(port), "a"(value));
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
	__asm__ __volatile__("cld; rep; insw"
                 : "+D"(buf),
                 "+c"(n)
                 : "d"(port));
}

/* 从内存批量地写入数据到I/O端口（16位） */
inline void outsw(uint16_t port, const void *buf, unsigned long n)
{
	__asm__ __volatile__("cld; rep; outsw"
                 : "+S"(buf),
                 "+c"(n)
                 : "d"(port));
}

/* 从I/O端口批量地读取数据到内存（32位） */
inline void insl(uint32_t port, void *addr, int cnt)
{
	__asm__ __volatile__("cld;"
                 "repne; insl;"
                 : "=D" (addr), "=c" (cnt)
                 : "d" (port), "0" (addr), "1" (cnt)
                 : "memory", "cc");
}

/* 从内存批量地写入数据到I/O端口（32位） */
inline void outsl(uint32_t port, const void *addr, int cnt)
{
	__asm__ __volatile__("cld;"
                 "repne; outsl;"
                 : "=S" (addr), "=c" (cnt)
                 : "d" (port), "0" (addr), "1" (cnt)
                 : "memory", "cc");
}

/* 加载eflags寄存器 */
inline uint32_t load_eflags(void)
{
	uint32_t eflags;
	__asm__ __volatile__("pushf\n\t"
                 "pop %0"
                 : "=g"(eflags)
                 :
                 : "memory");
	return eflags;
}

/* 存储eflags寄存器 */
inline void store_eflags(uint32_t eflags)
{
	__asm__ __volatile__("push %0\n\t"
                 "popf"
                 :
                 : "r" (eflags)
                 : "memory", "cc"
	);
}

/* 获取当前的CR0寄存器的值 */
uint32_t get_cr0(void)
{
	uint32_t cr0;
	__asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
	return cr0;
}

/* 将值写入CR0寄存器 */
void set_cr0(uint32_t cr0)
{
	__asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
}

/* 检查当前CPU是否支持MSR */
int cpu_has_msr(void)
{
	static uint32_t a, b, c, d; // eax, ebx, ecx, edx
	cpuid(1, &a, &b, &c, &d);
	return d & (1 << 5);
}

/* 读取指定的MSR值 */
void cpu_get_msr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
	__asm__ __volatile__("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

/* 设置指定的MSR值 */
void cpu_set_msr(uint32_t msr, uint32_t lo, uint32_t hi)
{
	__asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

/* 开启中断 */
void enable_intr(void)
{
	__asm__ __volatile__("sti");
}

/* 关闭中断 */
void disable_intr(void)
{
	__asm__ __volatile__("cli" ::: "memory");
}

/* 内核停机 */
void krn_halt(void)
{
	while(1) {__asm__("hlt");}
}
