/*
 *
 *		cmos.h
 *		cmos存储器头文件
 *
 *		2024/6/29 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_CMOS_H_
#define INCLUDE_CMOS_H_

/* 从CMOS存储器中读取数据 */
unsigned char read_cmos(unsigned char p);

/* 写入数据到CMOS存储器 */
void write_cmos(unsigned char p, unsigned char data);

unsigned int get_hour_hex(void);								// 获取当前小时的HEX
unsigned int get_min_hex(void);									// 获取当前分钟的HEX
unsigned int get_sec_hex(void);									// 获取当前秒钟的HEX
unsigned int get_day_of_month(void);							// 获取当前月中日
unsigned int get_day_of_week(void);								// 获取当前周中日的HEX
unsigned int get_mon_hex(void);									// 获取当前月份的HEX
unsigned int get_year(void);									// 获取当前年份

#define cmos_index		0x70
#define cmos_data		0x71
#define CMOS_CUR_SEC	0x0										// CMOS当前秒(BCD)
#define CMOS_ALA_SEC	0x1										// CMOS报警秒(BCD)
#define CMOS_CUR_MIN	0x2										// CMOS当前分(BCD)
#define CMOS_ALA_MIN	0x3										// CMOS报警分(BCD)
#define CMOS_CUR_HOUR	0x4										// CMOS当前时(BCD)
#define CMOS_ALA_HOUR	0x5										// CMOS报警时(BCD)
#define CMOS_WEEK_DAY	0x6										// CMOS一周当前天(BCD)
#define CMOS_MON_DAY	0x7										// CMOS一月当前日(BCD)
#define CMOS_CUR_MON	0x8										// CMOS当前月(BCD)
#define CMOS_CUR_YEAR	0x9										// CMOS当前年(BCD)
#define CMOS_DEV_TYPE	0x12									// CMOS驱动器格式
#define CMOS_CUR_CEN	0x32									// CMOS当前世纪(BCD)

#define BCD_HEX(n) ((n >> 4) * 10) + (n & 0xf)					// BCD转十六进制
#define HEX_BCD(n) (((n) >= 0xA) ? ((n) - 0xA + 0x10) : (n))	// 十六进制转BCD

#define BCD_ASCII_first(n) (((n<<4)>>4)+0x30)
#define BCD_ASCII_S(n) ((n<<4)+0x30)

#endif // INCLUDE_CMOS_H_
