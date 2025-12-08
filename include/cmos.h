/*
 *
 *      cmos.h
 *      CMOS memory header file
 *
 *      2024/6/29 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CMOS_H_
#define INCLUDE_CMOS_H_

#include <stdint.h>

#define cmos_index    0x70
#define cmos_data     0x71
#define CMOS_CUR_SEC  0x0  // CMOS current second (BCD)
#define CMOS_ALA_SEC  0x1  // CMOS alarm seconds (BCD)
#define CMOS_CUR_MIN  0x2  // CMOS current division (BCD)
#define CMOS_ALA_MIN  0x3  // CMOS alarm points (BCD)
#define CMOS_CUR_HOUR 0x4  // CMOS current time (BCD)
#define CMOS_ALA_HOUR 0x5  // CMOS alarm (BCD)
#define CMOS_WEEK_DAY 0x6  // CMOS current day of the week (BCD)
#define CMOS_MON_DAY  0x7  // CMOS January current date (BCD)
#define CMOS_CUR_MON  0x8  // CMOS current month (BCD)
#define CMOS_CUR_YEAR 0x9  // CMOS current year (BCD)
#define CMOS_DEV_TYPE 0x12 // CMOS Driver Format
#define CMOS_CUR_CEN  0x32 // CMOS Current Century (BCD)

#define BCD_HEX(n) ((((n) >> 4) * 10) + ((n) & 0xf))         // BCD to Hexadecimal
#define HEX_BCD(n) (((n) >= 0xa) ? ((n) - 0xa + 0x10) : (n)) // Hexadecimal to BCD

#define BCD_ASCII_first(n) ((((n) << 4) >> 4) + 0x30)
#define BCD_ASCII_S(n)     (((n) << 4) + 0x30)

/* Reading data from CMOS memory */
uint8_t read_cmos(uint8_t p);

/* Write data to CMOS memory */
void write_cmos(uint8_t p, uint8_t data);

uint32_t get_hour_hex(void);     // Get the HEX value of the current hour
uint32_t get_min_hex(void);      // Get the HEX of the current minute
uint32_t get_sec_hex(void);      // Get the HEX value of the current second
uint32_t get_day_of_month(void); // Get the current day of the month
uint32_t get_day_of_week(void);  // Get the HEX number of the current day of the week
uint32_t get_mon_hex(void);      // Get the HEX of the current month
uint32_t get_year(void);         // Get the current year

#endif // INCLUDE_CMOS_H_
