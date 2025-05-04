/*
 *
 *      term.c
 *      Terminal input and output handling
 *
 *      2025/5/4 By XSlime (W9pi3cZ1)
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "term.h"
#include "printk.h"
#include "ps2.h"
#include "string.h"
#include "video.h"

char input_buffer[BUFF_SIZE];
char cpy_buffer[BUFF_SIZE];
long long input_buffer_index = 0;
uint32_t old_cx              = 0;
uint32_t old_cy              = 0;
uint32_t cur_input_size      = 0;
uint32_t eol_cx              = 0; // cx of end-of-line
uint32_t eol_cy              = 0; // cy of end-of-line

/* Reverse a color on the screen */
void reverse_color(uint32_t x, uint32_t y)
{
    video_draw_pixel(x, y, video_get_pixel(x, y) ^ 0x00ffffff);
}

/* Calculate end-of-line cx and cy */
void eol_calc()
{
    VideoInfo info = video_get_info();
    uint32_t cy    = old_cy + ((cur_input_size + TRIM_SIZE) / info.c_width);
    uint32_t cx    = (cur_input_size + TRIM_SIZE) % info.c_width;
    if ((uint32_t)cx >= info.c_width) {
        cx = (cur_input_size + TRIM_SIZE) % info.c_width;
        cy++;
    }
    if ((uint32_t)cy >= info.c_height) {
        printk("\n");
        info = video_get_info();
        old_cy--;
        cx = (cur_input_size + TRIM_SIZE) % info.c_width;
        cy = old_cy + ((cur_input_size + TRIM_SIZE + 1) / info.c_width);
    }
    eol_cx = cx;
    eol_cy = cy;
}

/* Draw the cursor */
void draw_cursor()
{
    VideoInfo info = video_get_info();
    uint32_t cy    = old_cy + ((input_buffer_index + TRIM_SIZE) / info.c_width);
    uint32_t cx    = (input_buffer_index + TRIM_SIZE) % info.c_width;
    if ((uint32_t)cx >= info.c_width) {
        cx = (input_buffer_index + TRIM_SIZE) % info.c_width;
        cy++;
    }
    if ((uint32_t)cy >= info.c_height) {
        printk("\n");
        info = video_get_info();
        old_cy--;
        cx = (input_buffer_index + TRIM_SIZE) % info.c_width;
        cy = old_cy + ((input_buffer_index + TRIM_SIZE + 1) / info.c_width);
    }
    int x = cx * 9;
    int y = cy * 16;
    video_invoke_area(x, y, x + 8, y + 15, reverse_color);
}

/* Print terminal trims */
void term_trims(void)
{
    printk(TRIM); // trigger a screen scroll
}

/* Reset the terminal */
void term_reset(void)
{
    // Clear the input buffer
    memset(input_buffer, '\0', BUFF_SIZE);
    input_buffer_index = 0;
    VideoInfo info     = video_get_info();
    old_cx             = info.cx;
    old_cy             = info.cy;
    cur_input_size     = 0;
    term_trims();
}

#define KEY_UP    0x148
#define KEY_DOWN  0x150
#define KEY_LEFT  0x14b
#define KEY_RIGHT 0x14d

/* Write a character to the terminal buffer */
void write_buf(char ch, long long idx)
{
    if (idx < BUFF_SIZE) {
        input_buffer[idx] = ch;
    } else {
        // overflow
    }
}

/* Handle keyboard input */
static void keyboard_handle(int scancode, int ascii, int _, int released)
{
    long long n = 0;
    if (!released) {
        switch (scancode) {
            case KEY_UP :
                // Do nothing
                break;
            case KEY_DOWN :
                // Do nothing
                break;
            case KEY_LEFT :
                draw_cursor();
                if (input_buffer_index > 0) { input_buffer_index--; }
                break;
            case KEY_RIGHT :
                draw_cursor();
                if (input_buffer[input_buffer_index] != '\0') { input_buffer_index++; }
                break;
            default :
                goto write_to_buf;
                break;
        }
        draw_cursor();
    }
    return;
write_to_buf:
    if (ascii != -1) {
        switch (ascii) {
            case '\b' :
                // Backspace processing
                draw_cursor();
                if (input_buffer[input_buffer_index] == '\0') {
                    input_buffer_index--;
                    if (input_buffer_index < 0) {
                        input_buffer_index = 0;
                        draw_cursor();
                    }
                    write_buf('\0', input_buffer_index);
                    draw_cursor();
                    printk("\b \b");
                } else {
                    input_buffer_index--;
                    if (input_buffer_index < 0) {
                        input_buffer_index = 0;
                        draw_cursor();
                    } else {
                        strcpy(&input_buffer[input_buffer_index], &input_buffer[input_buffer_index + 1]);
                        if (input_buffer_index < 0) { input_buffer_index = 0; }
                        draw_cursor();
                        printk("\b \b");
                    }
                }
                break;
            case '\n' :
                // Enter processing
                write_buf('\0', cur_input_size);
                draw_cursor();

                // Send to screen START
                printk("\n  Input: ");
                while (n < cur_input_size) {
                    printk("%02x ", input_buffer[n]);
                    n++;
                }
                printk("\n");
                // Send to screen END

                term_reset();
                draw_cursor();
                return;
                break;
            default :
                // Only printable characters
                if (ascii >= ' ') {
                    if (input_buffer[input_buffer_index] == '\0') {
                        write_buf(ascii, input_buffer_index);
                        input_buffer_index++;
                    } else {
                        strcpy(cpy_buffer, &input_buffer[input_buffer_index]);
                        write_buf(ascii, input_buffer_index);
                        strcpy(&input_buffer[input_buffer_index + 1], cpy_buffer);
                        input_buffer_index++;
                    }
                }
                break;
        }
        cur_input_size = strlen(input_buffer);
        video_move_to(old_cx, old_cy);
        term_trims();
        printk("%s", input_buffer);
        eol_calc();
        // Draw cursor
        draw_cursor();
    }
};

/* Initialize the terminal */
void term_init(void)
{
    register_kbd_handler(keyboard_handle); // Register keyboard handler
    printk("Terminal initialized.\n");
    term_reset();
    draw_cursor();
}