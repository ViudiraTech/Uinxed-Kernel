/*
 *
 *		cmos.h
 *		CMOS memory header file
 *
 *		2024/6/29 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_CMOS_H_
#define INCLUDE_CMOS_H_

/* Reading data from CMOS memory */
unsigned char read_cmos(unsigned char p);

/* Write data to CMOS memory */
void write_cmos(unsigned char p, unsigned char data);

unsigned int get_hour_hex(void);								// Get the HEX value of the current hour
unsigned int get_min_hex(void);									// Get the HEX of the current minute
unsigned int get_sec_hex(void);									// Get the HEX value of the current second
unsigned int get_day_of_month(void);							// Get the current day of the month
unsigned int get_day_of_week(void);								// Get the HEX number of the current day of the week
unsigned int get_mon_hex(void);									// Get the HEX of the current month
unsigned int get_year(void);									// Get the current year

#define cmos_index		0x70
#define cmos_data		0x71
#define CMOS_CUR_SEC	0x0										// CMOS current second (BCD)
#define CMOS_ALA_SEC	0x1										// CMOS alarm seconds (BCD)
#define CMOS_CUR_MIN	0x2										// CMOS current division (BCD)
#define CMOS_ALA_MIN	0x3										// CMOS alarm points (BCD)
#define CMOS_CUR_HOUR	0x4										// CMOS current time (BCD)
#define CMOS_ALA_HOUR	0x5										// CMOS alarm (BCD)
#define CMOS_WEEK_DAY	0x6										// CMOS current day of the week (BCD)
#define CMOS_MON_DAY	0x7										// CMOS January current date (BCD)
#define CMOS_CUR_MON	0x8										// CMOS current month (BCD)
#define CMOS_CUR_YEAR	0x9										// CMOS current year (BCD)
#define CMOS_DEV_TYPE	0x12									// CMOS Driver Format
#define CMOS_CUR_CEN	0x32									// CMOS Current Century (BCD)

#define BCD_HEX(n) ((n >> 4) * 10) + (n & 0xf)					// BCD to Hexadecimal
#define HEX_BCD(n) (((n) >= 0xA) ? ((n) - 0xA + 0x10) : (n))	// Hexadecimal to BCD

#define BCD_ASCII_first(n) (((n<<4)>>4)+0x30)
#define BCD_ASCII_S(n) ((n<<4)+0x30)

#endif // INCLUDE_CMOS_H_
