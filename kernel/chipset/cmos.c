/*
 *
 *		cmos.c
 *		CMOS memory
 *
 *		2024/6/29 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "cmos.h"
#include "common.h"

/* Reading data from CMOS memory */
unsigned char read_cmos(unsigned char p)
{
	unsigned char data;

	/* Send CMOS register index */
	outb(cmos_index, p);

	/* Read the value in the CMOS data register */
	data = inb(cmos_data);

	/* Send 0x80 to the CMOS index register, probably to reset or terminate the read signal */
	outb(cmos_index, 0x80);
	return data;
}

/* Write data to CMOS memory */
void write_cmos(unsigned char p, unsigned char data)
{
	/* Send CMOS register index */
	outb(cmos_index, p);

	/* Write data to CMOS data register */
	outb(cmos_data, data);

	/* Send 0x80 to the CMOS index register, probably to reset or terminate the read signal */
	outb(cmos_index, 0x80);
}

/* Get the HEX value of the current hour */
unsigned int get_hour_hex(void)
{
	return BCD_HEX(read_cmos(CMOS_CUR_HOUR));
}

/* Get the HEX of the current minute */
unsigned int get_min_hex(void)
{
	return BCD_HEX(read_cmos(CMOS_CUR_MIN));
}

/* Get the HEX value of the current second */
unsigned int get_sec_hex(void)
{
	return BCD_HEX(read_cmos(CMOS_CUR_SEC));
}

/* Get the current day of the month */
unsigned int get_day_of_month(void)
{
	return BCD_HEX(read_cmos(CMOS_MON_DAY));
}

/* Get the HEX number of the current day of the week */
unsigned int get_day_of_week(void)
{
	return BCD_HEX(read_cmos(CMOS_WEEK_DAY));
}

/* Get the HEX of the current month */
unsigned int get_mon_hex(void)
{
	return BCD_HEX(read_cmos(CMOS_CUR_MON));
}

/* Get the current year */
unsigned int get_year(void)
{
	/* The year stored in CMOS is from 2000, so you need to add 2010 to get the actual year */
	return (BCD_HEX(read_cmos(CMOS_CUR_CEN)) * 100) + BCD_HEX(read_cmos(CMOS_CUR_YEAR)) - 30 + 2010;
}
