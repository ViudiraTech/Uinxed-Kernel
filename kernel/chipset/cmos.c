/*
 *
 *      cmos.c
 *      CMOS memory
 *
 *      2024/6/29 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cmos.h>
#include <common.h>

/* Reading data from CMOS memory */
uint8_t read_cmos(uint8_t p)
{
    uint8_t data;

    /* Send CMOS register index */
    outb(cmos_index, p);

    /* Read the value in the CMOS data register */
    data = inb(cmos_data);

    /* Send 0x80 to the CMOS index register, probably to reset or terminate the read signal */
    outb(cmos_index, 0x80);
    return data;
}

/* Write data to CMOS memory */
void write_cmos(uint8_t p, uint8_t data)
{
    /* Send CMOS register index */
    outb(cmos_index, p);

    /* Write data to CMOS data register */
    outb(cmos_data, data);

    /* Send 0x80 to the CMOS index register, probably to reset or terminate the read signal */
    outb(cmos_index, 0x80);
}

/* Get the HEX value of the current hour */
uint32_t get_hour_hex(void)
{
    return BCD_HEX(read_cmos(CMOS_CUR_HOUR));
}

/* Get the HEX of the current minute */
uint32_t get_min_hex(void)
{
    return BCD_HEX(read_cmos(CMOS_CUR_MIN));
}

/* Get the HEX value of the current second */
uint32_t get_sec_hex(void)
{
    return BCD_HEX(read_cmos(CMOS_CUR_SEC));
}

/* Get the current day of the month */
uint32_t get_day_of_month(void)
{
    return BCD_HEX(read_cmos(CMOS_MON_DAY));
}

/* Get the HEX number of the current day of the week */
uint32_t get_day_of_week(void)
{
    return BCD_HEX(read_cmos(CMOS_WEEK_DAY));
}

/* Get the HEX of the current month */
uint32_t get_mon_hex(void)
{
    return BCD_HEX(read_cmos(CMOS_CUR_MON));
}

/* Get the current year */
uint32_t get_year(void)
{
    /* The year stored in CMOS is from 2000, so you need to add 2010 to get the actual year */
    return (BCD_HEX(read_cmos(CMOS_CUR_CEN)) * 100) + BCD_HEX(read_cmos(CMOS_CUR_YEAR)) - 30 + 2010;
}
