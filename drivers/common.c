/*
 *
 *      common.c
 *      Common device
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <stddef.h>
#include <stdint.h>
#include <tty.h>

/* Port write (8 bits) */
void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %1, %0" ::"dN"(port), "a"(value));
}

/* Port read (8 bits) */
uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

/* Port write (16 bits) */
void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %1, %0" ::"dN"(port), "a"(value));
}

/* Port read (16 bits) */
uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

/* Port write (32 bits) */
void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %1, %0" ::"dN"(port), "a"(value));
}

/* Port read (32 bits) */
uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

/* Read data from I/O port to memory in batches (16 bits) */
void insw(uint16_t port, void *buf, size_t n)
{
    __asm__ volatile("cld; rep; insw" : "+D"(buf), "+c"(n) : "d"(port));
}

/* Write data from memory to I/O port in batches (16 bits) */
void outsw(uint16_t port, const void *buf, size_t n)
{
    __asm__ volatile("cld; rep; outsw" : "+S"(buf), "+c"(n) : "d"(port));
}

/* Read data from I/O port to memory in batches (32 bits) */
void insl(uint32_t port, void *addr, size_t cnt)
{
    __asm__ volatile("cld; repne; insl;" : "=D"(addr), "=c"(cnt) : "d"(port), "0"(addr), "1"(cnt) : "memory", "cc");
}

/* Write data from memory to I/O port in batches (32 bits) */
void outsl(uint32_t port, const void *addr, size_t cnt)
{
    __asm__ volatile("cld; repne; outsl;" : "=S"(addr), "=c"(cnt) : "d"(port), "0"(addr), "1"(cnt) : "memory", "cc");
}

/* Flushes the TLB of the specified address */
void flush_tlb(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" ::"r"(addr) : "memory");
}

/* Get the current value of the CR3 register */
uint64_t get_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Get the current value of the RSP register */
uint64_t get_rsp(void)
{
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

/* Get the current value of the status flag register */
uint64_t get_rflags(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags)::"memory");
    return rflags;
}

/* Write a 32-bit data to the specified memory address */
void mmio_write32(uint32_t *addr, uint32_t data)
{
    *(volatile uint32_t *)addr = data;
}

/* Write a 64-bit data to the specified memory address */
void mmio_write64(void *addr, uint64_t data)
{
    *(volatile uint64_t *)addr = data;
}

/* Read a 32-bit data from the specified memory address */
uint32_t mmio_read32(void *addr)
{
    return *(volatile uint32_t *)addr;
}

/* Read a 64-bit data from the specified memory address */
uint64_t mmio_read64(void *addr)
{
    return *(volatile uint64_t *)addr;
}

/* Read msr register */
uint64_t rdmsr(uint32_t msr)
{
    uint32_t rax, rdx;
    __asm__ volatile("rdmsr" : "=a"(rax), "=d"(rdx) : "c"(msr));
    return ((uint64_t)rdx << 32) | rax;
}

/* Write to msr register */
void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t rax = (uint32_t)value;
    uint32_t rdx = value >> 32;
    __asm__ volatile("wrmsr" ::"c"(msr), "a"(rax), "d"(rdx));
}

/* Loading data atomically */
uint64_t load(uint64_t *addr)
{
    uint64_t ret = 0;
    __asm__ volatile("lock xadd %[ret], %[addr];" : [addr] "+m"(*addr), [ret] "+r"(ret)::"memory");
    return ret;
}

/* Storing data atomically */
void store(uint64_t *addr, uint32_t value)
{
    __asm__ volatile("lock xchg %[value], %[addr];" : [addr] "+m"(*addr), [value] "+r"(value)::"memory");
}

/* Basic rdtsc reading */
uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Serialized rdtsc reads */
uint64_t rdtsc_serialized(void)
{
    uint32_t lo, hi;
    __asm__ volatile("mfence\n\t"
                     "rdtsc\n\t"
                     "lfence"
                     : "=a"(lo), "=d"(hi)
                     :
                     : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Basic rdtscp reading */
uint64_t rdtscp(uint32_t *aux)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(*aux) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Serialized rdtscp reads */
uint64_t rdtscp_serialized(uint32_t *aux)
{
    uint32_t lo, hi;
    __asm__ volatile("mfence\n\t"
                     "rdtscp\n\t"
                     "lfence"
                     : "=a"(lo), "=d"(hi), "=c"(*aux)
                     :
                     : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Enable interrupt */
void enable_intr(void)
{
    __asm__ volatile("sti");
}

/* Disable interrupts */
void disable_intr(void)
{
    __asm__ volatile("cli" ::: "memory");
}

/* Kernel halt */
void krn_halt(void)
{
    tty_buff_flush();
    disable_intr();
    while (1) __asm__ volatile("hlt");
}

/* Compiler barrier */
__attribute__((noinline)) void compiler_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}
