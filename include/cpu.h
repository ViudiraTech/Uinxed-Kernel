/*
 *
 *		cpu.h
 *		cpu相关操作头文件
 *
 *		2024/8/21 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_CPU_H_
#define INCLUDE_CPU_H_

typedef struct {
	char* vendor;
	char model_name[64];
	unsigned int virt_bits;
	unsigned int phys_bits;
}cpu_t;

/* 获取CPUID */
void cpuid(unsigned int op, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx);

/* 获取CPU厂商名称 */
void get_vendor_name(cpu_t *c);

/* 获取CPU型号名称 */
void get_model_name(cpu_t *c);

/* 获取CPU地址大小 */
void get_cpu_address_sizes(cpu_t *c);

/* 打印CPU信息 */
void print_cpu_info(void);

#endif // INCLUDE_CPU_H_
