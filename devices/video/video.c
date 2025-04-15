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
static volatile struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

extern uint8_t ascfont[];	// Fonts

uint64_t width;				// Screen length
uint64_t height;			// Screen width
uint64_t stride;			// Frame buffer line spacing
uint32_t *buffer;			// Video Memory

int32_t x, y;				// The current absolute cursor position
int32_t cx, cy;				// The character position of the current cursor
uint32_t c_width, c_height;	// Screen character width and height

uint32_t fore_color;		// Foreground color
uint32_t back_color;		// Background color

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void)
{
	return framebuffer_request.response->framebuffers[0];
}

/* Initialize Video */
void video_init(void)
{
	if (framebuffer_request.response == 0 || framebuffer_request.response->framebuffer_count < 1) {
		krn_halt();
	}
	struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
	buffer	= framebuffer->address;
	width	= framebuffer->width;
	height	= framebuffer->height;
	stride	= framebuffer->pitch / 4;

	x = 2;
	y = cx = cy = 0;
	c_width = width / 9;
	c_height = height / 16;

	fore_color = 0xffaaaaaa;
	back_color = 0xff000000;
	video_clear();
}

/* Clear screen */
void video_clear(void)
{
	for (uint32_t i = 0; i < (width * height); i++) {
		buffer[i] = 0xff000000;
	}
	back_color = 0xff000000;
	x = 2;
	y = 0;
	cx = cy = 0;
}

/* Clear screen with color */
void video_clear_color(int color)
{
	for (uint32_t i = 0; i < (width * height); i++) {
		buffer[i] = color;
	}
	back_color = color;
	x = 2;
	y = 0;
	cx = cy = 0;
}

/* Screen scrolling operation */
void video_scroll(void)
{
	if ((uint32_t)cx > c_width) {
		cx = 0;
		cy++;
	} else cx++;
	if ((uint32_t)cy >= c_height) {
		uint8_t *dest = (uint8_t *)buffer;
		const uint8_t *src = (const uint8_t *)(buffer + stride * 16);
		size_t count = (width * (height - 16) * sizeof(uint32_t)) / 8;

		__asm__ volatile ("rep movsq"
                          : "+D"(dest), "+S"(src), "+c"(count)
                          :: "memory");

		memset(buffer + (height - 16) * stride, back_color, 16 * stride * sizeof(uint32_t));
		cy = c_height - 1;
	}
}

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
	(buffer)[y * width + x] = color;
}

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(int x0, int y0, int x1, int y1, int color)
{
	int x, y;
	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++) {
			(buffer)[y * width + x] = color;
		}
	}
}

/* Draw a character at the specified coordinates on the screen */
void video_draw_char(const char c, int32_t x, int32_t y, int color)
{
	uint8_t *font = ascfont;
	font += c * 16;
	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 9; j++) {
			if (font[i] & (0x80 >> j)) {
				buffer[(y + i) * width + x + j] = color;
			} else buffer[(y + i) * width + x + j] = back_color;
		}
	}
}

/* Print a character at the specified coordinates on the screen */
void video_put_char(const char c, int color)
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
		for (int i = 0; i < 8; i++) video_put_char(' ', color);
		return;
	} else if (c == '\b' && cx > 0) {
		cx -= 1;
		if (cx == 0) {
			cx = c_width - 1;
			if (cy != 0) cy -= 1;
			if (cy == 0) cx = 0, cy = 0;
		}
		int x = (cx + 1) * 8 - 7;
		int y = cy * 16;
		video_draw_rect(x, y, x + 8, y + 16, back_color);
		return;
	}
	video_scroll();
	video_draw_char(c, cx * 8 - 7, cy * 16, color);
}

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str)
{
	for (; *str; ++str)
		video_put_char(*str, fore_color);
}

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, int color)
{
	for (; *str; ++str)
		video_put_char(*str, color);
}
