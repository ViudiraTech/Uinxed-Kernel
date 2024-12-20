/*
 *
 *		lib_os_terminal.lib.h
 *		lib_os_terminal库头文件
 *
 *		2024/10/20 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_LIB_OS_TERMINAL_LIB_H_
#define INCLUDE_LIB_OS_TERMINAL_LIB_H_

#define TERMINAL_EMBEDDED_FONT

#include "types.h"

typedef enum TerminalInitResult {
	Success,
	MallocIsNull,
	FreeIsNull,
	FontBufferIsNull,
} TerminalInitResult;

typedef struct TerminalDisplay {
	size_t width;
	size_t height;
	uint32_t *address;
} TerminalDisplay;

typedef struct TerminalPalette {
	uint32_t foreground;
	uint32_t background;
	uint32_t ansi_colors[16];
} TerminalPalette;

#if defined(TERMINAL_EMBEDDED_FONT)
extern enum TerminalInitResult terminal_init(const struct TerminalDisplay *display, float font_size, void *(*malloc)(size_t),
                                             void(*free)(void*), void(*serial_print)(const char*));
#endif

#if !defined(TERMINAL_EMBEDDED_FONT)
extern enum TerminalInitResult terminal_init(const struct TerminalDisplay *display, const uint8_t *font_buffer, size_t font_buffer_size,
                                             float font_size, void *(*malloc)(size_t), void(*free)(void*), void(*serial_print)(const char*));
#endif

void terminal_destroy(void);
void terminal_flush(void);
void terminal_process(const char *s);
void terminal_process_char(char c);
void terminal_set_history_size(size_t size);
void terminal_set_nature_scroll(bool mode);
void terminal_set_auto_flush(bool auto_flush);
void terminal_set_bell_handler(void (*handler)(void));
void terminal_set_color_scheme(size_t palette_index);
void terminal_set_custom_color_scheme(const struct TerminalPalette *palette);
const uint8_t *terminal_handle_keyboard(uint8_t scancode);

#endif // INCLUDE_LIB_OS_TERMINAL_LIB_H_
