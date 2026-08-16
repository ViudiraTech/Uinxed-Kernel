/*
 *
 *      fbcon.h
 *      Framebuffer console
 *
 *      2026/5/16 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FBCON_H_
#define INCLUDE_FBCON_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Initialize framebuffer console */
void fbcon_init(void);

/* Resize fbcon grids after framebuffer dimensions change */
void fbcon_resize(void);

/* Quiesce framebuffer drawing while video.c changes the backing surface. */
void fbcon_handoff_begin(void);
void fbcon_handoff_end(void);

/* True once the fbcon text grid is allocated and safe to render into. */
bool fbcon_is_ready(void);

/* Draw a character with per-cell foreground and background color */
void fbcon_draw_char_bg(const char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);

/* ANSI escape sequence aware rendering primitives */
void fbcon_scroll_up(uint32_t top, uint32_t bottom, uint32_t lines);
void fbcon_scroll_down(uint32_t top, uint32_t bottom, uint32_t lines);
void fbcon_erase_display(uint32_t mode);
void fbcon_erase_line(uint32_t mode, uint32_t y);
void fbcon_erase_chars(uint32_t x, uint32_t y, uint32_t count);
void fbcon_insert_chars(uint32_t x, uint32_t y, uint32_t n, uint32_t cols);
void fbcon_delete_chars(uint32_t x, uint32_t y, uint32_t n, uint32_t cols);

/* Process a buffer of characters through the ANSI escape sequence parser */
void fbcon_ansi_write(const uint8_t *buf, size_t len);

/*
 * Boot-logo area control.
 *
 * While the boot logo is active fbcon treats the top of the screen as an
 * overlay: the console keeps the full framebuffer grid but scrolls below
 * the logo.  fbcon_release_logo() hands the whole screen back without
 * touching the logo pixels - the very next console scrolls then reclaims
 * the logo area line by line.
 */
void fbcon_set_logo_active(bool active);
void fbcon_release_logo(void);

/*
 * Periodic cursor blink driver.  Called from the video refresh worker;
 * flips the block-cursor phase roughly every CURSOR_BLINK_INTERVAL
 * scheduler ticks and repaints the affected cells directly into the
 * framebuffer (the worker's frame-diff then flushes them).
 */
void fbcon_cursor_tick(uint64_t now_ticks);

#endif // INCLUDE_FBCON_H_
