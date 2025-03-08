/*
 *
 *		common.h
 *		常见设备驱动头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_COMMON_H_
#define INCLUDE_COMMON_H_

#include "stdint.h"

void outb(uint16_t port, uint8_t value);	// 端口写（8位）
void outw(uint16_t port, uint16_t value);	// 端口写（16位）
void outl(uint16_t port, uint32_t value);	// 端口写（32位）

uint8_t inb(uint16_t port);					// 端口读（8位）
uint16_t inw(uint16_t port);				// 端口读（16位）
uint32_t inl(uint16_t port);				// 端口读（32位）

/* 从I/O端口批量地读取数据到内存（16位） */
void insw(uint16_t port, void *buf, unsigned long n);

/* 从内存批量地写入数据到I/O端口（16位） */
void outsw(uint16_t port, const void *buf, unsigned long n);

/* 从I/O端口批量地读取数据到内存（32位） */
void insl(uint32_t port, void *addr, int cnt);

/* 从内存批量地写入数据到I/O端口（32位） */
void outsl(uint32_t port, const void *addr, int cnt);

/* 刷新指定地址的TLB */
void flush_tlb(uint64_t addr);

/* 获取CR3寄存器当前的值 */
uint64_t get_cr3(void);

/* 获取RSP寄存器当前的值 */
uint64_t get_rsp(void);

/* 获取状态标志寄存器当前的值 */
uint64_t get_rflags(void);

/* 向指定的内存地址写入一个32位的数据 */
void mmio_write32(uint32_t *addr, uint32_t data);

/* 向指定的内存地址写入一个64位的数据 */
void mmio_write64(void *addr, uint64_t data);

/* 从指定的内存地址读取一个32位的数据 */
uint32_t mmio_read32(void *addr);

/* 从指定的内存地址读取一个64位的数据 */
uint64_t mmio_read64(void *addr);

/* 读取msr寄存器 */
uint64_t rdmsr(uint32_t msr);

/* 写入msr寄存器 */
void wrmsr(uint32_t msr, uint64_t value);

/* 原子性地加载数据 */
uint64_t load(uint64_t *addr);

/* 原子性地存储数据 */
void store(uint64_t *addr, uint32_t value);

void enable_intr(void);		// 开启中断
void disable_intr(void);	// 关闭中断
void krn_halt(void);		// 内核停机

#endif // INCLUDE_COMMON_H_
