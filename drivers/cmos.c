// cmos.c -- cmos存储器驱动（基于 GPL-3.0 开源协议）
// Copyright © 2020 ViudiraTech，保留所有权利。
// 源于 MicroFish 撰写于 2024-6-29.

#include "cmos.h"
#include "common.h"

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
