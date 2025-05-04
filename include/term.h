/*
 *
 *      term.h
 *      Terminal input and output handling
 *
 *      2025/5/4 By XSlime (W9pi3cZ1)
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef _TERM_H
#define _TERM_H

/* Re-export */
#include "printk.h"
#include "string.h"
#include "video.h"

#define BUFF_SIZE 4096
#define TRIM_SIZE 3
#define TRIM      " $ "
#define KEY_UP    0x148
#define KEY_DOWN  0x150
#define KEY_LEFT  0x14b
#define KEY_RIGHT 0x14d

/* Reverse a color on the screen */
void reverse_color(uint32_t x, uint32_t y);

/* Calculate end-of-line cx and cy */
void eol_calc();

/* Draw the cursor */
void draw_cursor();

/* Print terminal trims */
void term_trims(void);

/* Reset the terminal */
void term_reset(void);

/* Initialize the terminal */
void term_init(void);

/* Write a character to the terminal buffer */
void write_buf(char ch, long long idx);

#endif