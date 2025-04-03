/*
 *
 *		video.c
 *		Basic Video
 *
 *		2024/9/16 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "limine.h"
#include "video.h"
#include "string.h"
#include "common.h"

__attribute__((used, section(".limine_requests")))
static __volatile__ struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

extern uint8_t ascfont[];	// Fonts

uint64_t width;				// Screen length
uint64_t height;			// Screen width
uint64_t stride;			// Frame buffer line spacing
uint32_t *buffer;			// Video Memory
uint32_t *back_buffer;		// Video memory buffer

int32_t x, y;				// The current absolute cursor position
int32_t cx, cy;				// The character position of the current cursor
uint32_t c_width, c_height;	// Screen character width and height

uint32_t fore_color;		// Foreground color
uint32_t back_color;		// Background color

/* Initialize Video */
void video_init(void)
{
	if (framebuffer_request.response == 0 || framebuffer_request.response->framebuffer_count < 1) {
		krn_halt();
	}
	struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
	buffer		= framebuffer->address;
	width		= framebuffer->width;
	height		= framebuffer->height;
	stride		= framebuffer->pitch / 4;
	back_buffer	= buffer;

	x = 2;
	y = cx = cy = 0;
	c_width = width / 9;
	c_height = height / 16;

	fore_color = 0xffffffff;
	back_color = 0xff000000;
	video_clear();
}

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void)
{
	return framebuffer_request.response->framebuffers[0];
}

/* Clear screen */
void video_clear(void)
{
	for (uint32_t i = 0; i < (width * height); i++) {
		back_buffer[i] = 0xff000000;
	}
	back_color = 0xff000000;
	x = 2;
	y = 0;
	cx = cy = 0;
	buffer = back_buffer;
}

/* Clear screen with color */
void video_clear_color(int color)
{
	for (uint32_t i = 0; i < (width * height); i++) {
		back_buffer[i] = color;
	}
	back_color = color;
	x = 2;
	y = 0;
	cx = cy = 0;
	buffer = back_buffer;
}

/* Screen scrolling operation */
inline void video_scroll(void)
{
	if ((uint32_t)cx > c_width) {
		cx = 0;
		cy++;
	} else cx++;
	if ((uint32_t)cy >= c_height) {
		cy = c_height - 1;
		memcpy(back_buffer, back_buffer + stride * 16, width * (height - 16) * sizeof(uint32_t));
		memset(back_buffer + (height - 16) * stride, back_color, 16 * stride * sizeof(uint32_t));
		buffer = back_buffer;
	}
}

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
	(back_buffer)[y * width + x] = color;
	buffer = back_buffer;
}

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(int x0, int y0, int x1, int y1, int color)
{
	int x, y;
	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++) {
			(back_buffer)[y * width + x] = color;
		}
	}
	buffer = back_buffer;
}

/* Draw a character at the specified coordinates on the screen */
void video_draw_char(char c, int32_t x, int32_t y, int color)
{
	uint8_t *font = ascfont;
	font += c * 16;
	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 9; j++) {
			if (font[i] & (0x80 >> j)) {
				back_buffer[(y + i) * width + x + j] = color;
			} else back_buffer[(y + i) * width + x + j] = back_color;
		}
	}
	buffer = back_buffer;
}

/* Print a character at the specified coordinates on the screen */
void video_put_char(char c, int color)
{
	if (c == '\n') {
		video_scroll();
		cx = 0;
		cy++;
		return;
	} else if (c == '\r') {
		cx = 0;
		return;
	} else if(c == '\t') {
		for (int i = 0; i < 4; i++) video_put_char(' ', color);
		return;
	} else if (c == '\b' && cx > 0) {
		cx -= 1;
		if (cx == 0) {
			cx = c_width - 1;
			if (cy != 0) cy -= 1;
			if (cy == 0) cx = 0, cy = 0;
		}
		int x = (cx+1) * 9 - 7;
		int y = cy * 16;
		video_draw_rect(x, y, x + 9, y + 16, back_color);
		return;
	}
	video_scroll();
	video_draw_char(c, cx * 9 - 7, cy * 16, color);
}

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str)
{
	for (; *str; ++str) {
		char c = *str;
		video_put_char(c, fore_color);
	}
}

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, int color)
{
	for (; *str; ++str) {
		char c = *str;
		video_put_char(c, color);
	}
}
