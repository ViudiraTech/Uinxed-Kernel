/*
 *
 *		cpu.c
 *		cpu相关操作
 *
 *		2024/8/21 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "cpu.h"
#include "printk.h"
#include "memory.h"

/* 获取CPUID */
void cpuid(unsigned int op, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx)
{
	*eax = op;
	*ecx = 0;
	__asm__ __volatile__("cpuid"
                         : "=a"	(*eax),				//输出参数
                         "=b"	(*ebx),
                         "=c"	(*ecx),
                         "=d"	(*edx)
                         : "0"	(*eax), "2" (*ecx)	//输入参数
                         : "memory");
}

/* 获取CPU厂商名称 */
void get_vendor_name(cpu_t *c)
{
	int cpuid_level;
	static char x86_vendor_id[16] = {0};
	cpuid(0x00000000, (unsigned int *) &cpuid_level,
                      (unsigned int *) &x86_vendor_id[0],
                      (unsigned int *) &x86_vendor_id[8],
                      (unsigned int *) &x86_vendor_id[4]);
	c->vendor = x86_vendor_id;
}

/* 获取CPU型号名称 */
void get_model_name(cpu_t *c)
{
	unsigned int *v = (unsigned int *) c->model_name;
	cpuid(0x80000002, &v[0], &v[1], &v[2],	&v[3]);
	cpuid(0x80000003, &v[4], &v[5], &v[6],	&v[7]);
	cpuid(0x80000004, &v[8], &v[9], &v[10],	&v[11]);
	c->model_name[48] = 0;
}

/* 获取CPU地址大小 */
void get_cpu_address_sizes(cpu_t *c)
{
	unsigned int eax, ebx, ecx, edx;
	cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
	c->virt_bits = (eax >> 8) & 0xff;
	c->phys_bits = eax & 0xff;
}

/* 打印CPU信息 */
void print_cpu_info(void)
{
	cpu_t *c = (cpu_t *)kmalloc(sizeof(cpu_t));
	get_vendor_name(c);
	get_model_name(c);
	get_cpu_address_sizes(c);
	printk("CPU Vendor:           %s\n", c->vendor);
	printk("CPU Name:             %s\n", c->model_name);
	printk("CPU Cache:            %d\n",c->phys_bits);
	printk("CPU Virtual Address:  0x%x\n\n",c->virt_bits); // 打印完当前CPU信息后会自动帮忙打印一个空行
	kfree(c);
}

/* 获取CPU信息 */
void get_cpu_info(char **VENDOR, char **MODEL_NAME, int *PHYS_BITS, int *VIRT_BITS)
{
	cpu_t *c = (cpu_t *)kmalloc(sizeof(cpu_t));
	get_vendor_name(c);
	get_model_name(c);
	get_cpu_address_sizes(c);
	*VENDOR = c->vendor;
	*MODEL_NAME = c->model_name;
	*PHYS_BITS = c->phys_bits;
	*VIRT_BITS = c->virt_bits;
	kfree(c);
}
