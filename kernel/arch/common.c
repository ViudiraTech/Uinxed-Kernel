/*
 *
 *      common.c
 *      Common device
 *
 *      2024/6/27 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>

/* Port write (8 bits) */
extern inline __attribute__((always_inline)) void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %1, %0" ::"dN"(port), "a"(value));
}

/* Port read (8 bits) */
extern inline __attribute__((always_inline)) uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

/* Port write (16 bits) */
extern inline __attribute__((always_inline)) void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %1, %0" ::"dN"(port), "a"(value));
}

/* Port read (16 bits) */
extern inline __attribute__((always_inline)) uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

/* Port write (32 bits) */
extern inline __attribute__((always_inline)) void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %1, %0" ::"dN"(port), "a"(value));
}

/* Port read (32 bits) */
extern inline __attribute__((always_inline)) uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

/* Read data from I/O port to memory in batches (16 bits) */
extern inline __attribute__((always_inline)) void insw(uint16_t port, void *buf, size_t n)
{
    __asm__ volatile("cld; rep; insw" : "+D"(buf), "+c"(n) : "d"(port));
}

/* Write data from memory to I/O port in batches (16 bits) */
extern inline __attribute__((always_inline)) void outsw(uint16_t port, const void *buf, size_t n)
{
    __asm__ volatile("cld; rep; outsw" : "+S"(buf), "+c"(n) : "d"(port));
}

/* Read data from I/O port to memory in batches (32 bits) */
extern inline __attribute__((always_inline)) void insl(uint32_t port, void *addr, size_t cnt)
{
    __asm__ volatile("cld; repne; insl;" : "=D"(addr), "=c"(cnt) : "d"(port), "0"(addr), "1"(cnt) : "memory", "cc");
}

/* Write data from memory to I/O port in batches (32 bits) */
extern inline __attribute__((always_inline)) void outsl(uint32_t port, const void *addr, size_t cnt)
{
    __asm__ volatile("cld; repne; outsl;" : "=S"(addr), "=c"(cnt) : "d"(port), "0"(addr), "1"(cnt) : "memory", "cc");
}

/* Flushes the TLB of the specified address */
extern inline __attribute__((always_inline)) void flush_tlb(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" ::"r"(addr) : "memory");
}

/* Get the current value of the CR3 register */
extern inline __attribute__((always_inline)) uint64_t get_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Get the current value of the RSP register */
extern inline __attribute__((always_inline)) uint64_t get_rsp(void)
{
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

/* Get the current value of the status flag register */
extern inline __attribute__((always_inline)) uint64_t get_rflags(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags)::"memory");
    return rflags;
}

/* Write an 8-bit data to the specified memory address */
extern inline __attribute__((always_inline)) void mmio_write8(volatile void *addr, uint8_t data)
{
    *(volatile uint8_t *)addr = data;
}

/* Write a 16-bit data to the specified memory address */
extern inline __attribute__((always_inline)) void mmio_write16(volatile void *addr, uint16_t data)
{
    *(volatile uint16_t *)addr = data;
}

/* Write a 32-bit data to the specified memory address */
extern inline __attribute__((always_inline)) void mmio_write32(volatile void *addr, uint32_t data)
{
    *(volatile uint32_t *)addr = data;
}

/* Write a 64-bit data to the specified memory address */
extern inline __attribute__((always_inline)) void mmio_write64(volatile void *addr, uint64_t data)
{
    *(volatile uint64_t *)addr = data;
}

/* Read an 8-bit data from the specified memory address */
extern inline __attribute__((always_inline)) uint8_t mmio_read8(const volatile void *addr)
{
    return *(volatile uint8_t *)addr;
}

/* Read a 16-bit data from the specified memory address */
extern inline __attribute__((always_inline)) uint16_t mmio_read16(const volatile void *addr)
{
    return *(volatile uint16_t *)addr;
}

/* Read a 32-bit data from the specified memory address */
extern inline __attribute__((always_inline)) uint32_t mmio_read32(const volatile void *addr)
{
    return *(volatile uint32_t *)addr;
}

/* Read a 64-bit data from the specified memory address */
extern inline __attribute__((always_inline)) uint64_t mmio_read64(const volatile void *addr)
{
    return *(volatile uint64_t *)addr;
}

/* Read msr register */
extern inline __attribute__((always_inline)) uint64_t rdmsr(uint32_t msr)
{
    uint32_t rax, rdx;
    __asm__ volatile("rdmsr" : "=a"(rax), "=d"(rdx) : "c"(msr));
    return ((uint64_t)rdx << 32) | rax;
}

/* Write to msr register */
extern inline __attribute__((always_inline)) void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t rax = (uint32_t)value;
    uint32_t rdx = value >> 32;
    __asm__ volatile("wrmsr" ::"c"(msr), "a"(rax), "d"(rdx));
}

/* Loading data atomically */
extern inline __attribute__((always_inline)) uint64_t load(uint64_t *addr)
{
    uint64_t ret = 0;
    __asm__ volatile("lock xadd %[ret], %[addr];" : [addr] "+m"(*addr), [ret] "+r"(ret)::"memory");
    return ret;
}

/* Storing data atomically */
extern inline __attribute__((always_inline)) void store(uint64_t *addr, uint32_t value)
{
    __asm__ volatile("lock xchg %[value], %[addr];" : [addr] "+m"(*addr), [value] "+r"(value)::"memory");
}

/* Basic rdtsc reading */
extern inline __attribute__((always_inline)) uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Serialized rdtsc reads */
extern inline __attribute__((always_inline)) uint64_t rdtsc_serialized(void)
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
extern inline __attribute__((always_inline)) uint64_t rdtscp(uint32_t *aux)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(*aux) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Serialized rdtscp reads */
extern inline __attribute__((always_inline)) uint64_t rdtscp_serialized(uint32_t *aux)
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
extern inline __attribute__((always_inline)) void enable_intr(void)
{
    __asm__ volatile("sti");
}

/* Disable interrupts */
extern inline __attribute__((always_inline)) void disable_intr(void)
{
    __asm__ volatile("cli" ::: "memory");
}

/* Compiler barrier */
extern inline __attribute__((always_inline)) void compiler_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* Hint the CPU that this is a spin-wait loop. */
extern inline __attribute__((always_inline)) void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

/* Order DMA reads before subsequent memory accesses. */
extern inline __attribute__((always_inline)) void dma_read_barrier(void)
{
    __asm__ volatile("lfence" ::: "memory");
}

/* Make prior memory writes visible before subsequent DMA/MMIO operations. */
extern inline __attribute__((always_inline)) void dma_write_barrier(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

/* Fully order DMA and memory accesses. */
extern inline __attribute__((always_inline)) void dma_full_barrier(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

/* Kernel halt */
extern inline __attribute__((always_inline)) __attribute__((noreturn)) void krn_halt(void)
{
    disable_intr();
    while (1) __asm__ volatile("hlt");
}
