// cmos.c -- cmos存储器驱动（基于 GPL-3.0 开源协议）
// Copyright © 2020 ViudiraTech，保留所有权利。
// 源于 MicroFish 撰写于 2024-6-29.

#include "cmos.h"
#include "printk.h"

/* 从CMOS存储器中读取数据 */
unsigned char read_cmos(unsigned char p)
{
	unsigned char data;

	/* 发送CMOS寄存器索引 */
	outb(cmos_index, p);

	/* 读取CMOS数据寄存器中的值 */
	data = inb(cmos_data);

	/* 发送0x80到CMOS索引寄存器，可能是用于重置或终止读取的信号 */
	outb(cmos_index, 0x80);
	return data;
}

/* 获取当前小时的十六进制表示 */
unsigned int get_hour_hex()
{
	return BCD_HEX(read_cmos(CMOS_CUR_HOUR));
}

unsigned int get_min_hex()
{
	return BCD_HEX(read_cmos(CMOS_CUR_MIN));
}

/* 获取当前秒的十六进制表示 */
unsigned int get_sec_hex()
{
	return BCD_HEX(read_cmos(CMOS_CUR_SEC));
}

/* 获取当前月份中的日的十六进制表示 */
unsigned int get_day_of_month()
{
	return BCD_HEX(read_cmos(CMOS_MON_DAY));
}

/* 获取当前星期几的十六进制表示 */
unsigned int get_day_of_week()
{
	return BCD_HEX(read_cmos(CMOS_WEEK_DAY));
}

/* 获取当前月份的十六进制表示 */
unsigned int get_mon_hex()
{
	return BCD_HEX(read_cmos(CMOS_CUR_MON));
}

/* 获取当前年份 */
unsigned int get_year()
{
	/* CMOS存储的年份是自2000年以来的，因此需要加上2010来得到实际年份 */
	return (BCD_HEX(read_cmos(CMOS_CUR_CEN)) * 100) + BCD_HEX(read_cmos(CMOS_CUR_YEAR)) - 30 + 2010;
}

static void cpuid(unsigned int op, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx)
{
	*eax = op;
	*ecx = 0;
	asm volatile("cpuid"
				: "=a" (*eax),				//输出参数
				"=b" (*ebx),
				"=c" (*ecx),
				"=d" (*edx)
				: "0" (*eax), "2" (*ecx)	//输入参数
				: "memory");
}

/* 获取CPU厂商名称 */
static void get_vendor_name(cpu_t *c)
{
	int cpuid_level;
	char x86_vendor_id[16] = {0};
	cpuid(0x00000000, (unsigned int *) &cpuid_level,
			(unsigned int *) &x86_vendor_id[0],
			(unsigned int *) &x86_vendor_id[8],
			(unsigned int *) &x86_vendor_id[4]);
	c->vendor = x86_vendor_id;
}

/* CPU型号名称 */
static void get_model_name(cpu_t *c)
{
	unsigned int *v = (unsigned int *) c->model_name;
	cpuid(0x80000002, &v[0], &v[1], &v[2], &v[3]);
	cpuid(0x80000003, &v[4], &v[5], &v[6], &v[7]);
	cpuid(0x80000004, &v[8], &v[9], &v[10], &v[11]);
	c->model_name[48] = 0;
}

/* 获取CPU地址大小 */
static void get_cpu_address_sizes(cpu_t *c)
{
	unsigned int eax, ebx, ecx, edx;
	cpuid(0x80000008, &eax, &ebx, &ecx, &edx);

	c->virt_bits = (eax >> 8) & 0xff;
	c->phys_bits = eax & 0xff;
}

/* 打印CPU信息 */
void print_cpu_id()
{
	cpu_t *c = (cpu_t *) kmalloc(sizeof(cpu_t));
	get_vendor_name(c);
	get_model_name(c);
	get_cpu_address_sizes(c);
	printk("CPU Vendor:            %s\n", c->vendor);
	printk("CPU Name:              %s\n", c->model_name);
	printk("CPU Cache:             %d\n",c->phys_bits);
	printk("CPU Virtual Address:   0x%x\n",c->virt_bits);
	kfree(c);
}
