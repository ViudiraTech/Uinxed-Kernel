/*
 *
 *      common.h
 *      Common device header file
 *
 *      2024/6/27 By Rainy101112
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_COMMON_H_
#define INCLUDE_COMMON_H_

#include "stddef.h"
#include "stdint.h"

void outb(uint16_t port, uint8_t value);  // Port write (8 bits)
void outw(uint16_t port, uint16_t value); // Port write (16 bits)
void outl(uint16_t port, uint32_t value); // Port write (32 bits)

uint8_t inb(uint16_t port);  // Port read (8 bits)
uint16_t inw(uint16_t port); // Port read (16 bits)
uint32_t inl(uint16_t port); // Port read (32 bits)

/* Read data from I/O port to memory in batches (16 bits) */
void insw(uint16_t port, void *buf, size_t n);

/* Write data from memory to I/O port in batches (16 bits) */
void outsw(uint16_t port, const void *buf, size_t n);

/* Read data from I/O port to memory in batches (32 bits) */
void insl(uint32_t port, void *addr, int cnt);

/* Write data from memory to I/O port in batches (32 bits) */
void outsl(uint32_t port, const void *addr, int cnt);

/* Flushes the TLB of the specified address */
void flush_tlb(uint64_t addr);

/* Get the current value of the CR3 register */
uint64_t get_cr3(void);

/* Get the current value of the RSP register */
uint64_t get_rsp(void);

/* Get the current value of the status flag register */
uint64_t get_rflags(void);

/* Write a 32-bit data to the specified memory address */
void mmio_write32(uint32_t *addr, uint32_t data);

/* Write a 64-bit data to the specified memory address */
void mmio_write64(void *addr, uint64_t data);

/* Read a 32-bit data from the specified memory address */
uint32_t mmio_read32(void *addr);

/* Read a 64-bit data from the specified memory address */
uint64_t mmio_read64(void *addr);

/* Read msr register */
uint64_t rdmsr(uint32_t msr);

/* Write to msr register */
void wrmsr(uint32_t msr, uint64_t value);

/* Loading data atomically */
uint64_t load(uint64_t *addr);

/* Storing data atomically */
void store(uint64_t *addr, uint32_t value);

void enable_intr(void);  // Enable interrupt
void disable_intr(void); // Disable interrupts
void krn_halt(void);     // Kernel halt

#endif // INCLUDE_COMMON_H_
