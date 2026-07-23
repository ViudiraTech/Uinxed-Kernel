/*
 *
 *      fbcon.h
 *      Framebuffer console
 *
 *      2026/5/16 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FBCON_H_
#define INCLUDE_FBCON_H_

#include <libs/std/stdint.h>

/* Initialize framebuffer console */
void fbcon_init(void);

/* Resize fbcon grids after framebuffer dimensions change */
void fbcon_resize(void);

/* Scroll to a position that units are characters */
void fbcon_move_to(uint32_t cx, uint32_t cy);

/* Screen scrolling operation */
void fbcon_scroll(void);

/* Draw a character at the specified coordinates on the screen */
void fbcon_draw_char(const char c, uint32_t x, uint32_t y, uint32_t color);

/* Flush character buffer to screen */
void fbcon_flush_buffer(uint32_t color);

/* Print a character at the specified coordinates on the screen */
void fbcon_put_char(const char c, uint32_t color);

/* Print a string at the specified coordinates on the screen */
void fbcon_put_string(const char *str);

/* Print a string with color at the specified coordinates on the screen */
void fbcon_put_string_color(const char *str, uint32_t color);

#endif // INCLUDE_FBCON_H_
