/*
 *
 *		vgafont.c
 *		VGA文字模式加载字体
 *
 *		2024/7/30 By wrhmade
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "vgafont.h"
#include "common.h"

#define VGA_AC_INDEX		0x3C0
#define VGA_AC_WRITE		0x3C0
#define VGA_AC_READ			0x3C1
#define VGA_MISC_WRITE		0x3C2
#define VGA_SEQ_INDEX		0x3C4
#define VGA_SEQ_DATA		0x3C5
#define VGA_DAC_READ_INDEX	0x3C7
#define VGA_DAC_WRITE_INDEX	0x3C8
#define VGA_DAC_DATA		0x3C9
#define VGA_MISC_READ		0x3CC
#define VGA_GC_INDEX		0x3CE
#define VGA_GC_DATA			0x3CF

#define VGA_CRTC_INDEX		0x3D4 /* 0x3B4 */
#define VGA_CRTC_DATA		0x3D5 /* 0x3B5 */
#define VGA_INSTAT_READ		0x3DA
#define VGA_NUM_SEQ_REGS	5
#define VGA_NUM_CRTC_REGS	25
#define VGA_NUM_GC_REGS		9
#define VGA_NUM_AC_REGS		21
#define VGA_NUM_REGS		(1 + VGA_NUM_SEQ_REGS + VGA_NUM_CRTC_REGS + VGA_NUM_GC_REGS + VGA_NUM_AC_REGS)
#define _vmemwr(DS, DO, S, N) memcpy((uint8_t *)((DS)*16 + (DO)), S, N)

void pokeb(int setmentaddr, int offset, char value)
{
	*(char *)(setmentaddr * 0x10 + offset) = value;
}

void pokew(int setmentaddr, int offset, short value)
{
	*(short *)(setmentaddr * 0x10 + offset) = value;
}

static void set_plane(unsigned p)
{
	unsigned char pmask;

	p &= 3;
	pmask = 1 << p;

	/* 设置读取平面 */
	outb(VGA_GC_INDEX, 4);
	outb(VGA_GC_DATA, p);

	/* 设置写入平面 */
	outb(VGA_SEQ_INDEX, 2);
	outb(VGA_SEQ_DATA, pmask);
}

unsigned get_fb_seg(void)
{
	unsigned seg;

	outb(VGA_GC_INDEX, 6);
	seg = inb(VGA_GC_DATA);
	seg >>= 2;
	seg &= 3;

	switch (seg) {
		case 0:
		case 1:
			seg = 0xA000;
			break;
		case 2:
			seg = 0xB000;
			break;
		case 3:
			seg = 0xB800;
			break;
	}
	return seg;
}

void vmemwr(unsigned dst_off, unsigned char *src, unsigned count)
{
	_vmemwr(get_fb_seg(), dst_off, src, count);
}

void write_font(unsigned char *buf, unsigned font_height)
{
	unsigned char seq2, seq4, gc4, gc5, gc6;
	unsigned i;

	outb(VGA_SEQ_INDEX, 2);
	seq2 = inb(VGA_SEQ_DATA);

	outb(VGA_SEQ_INDEX, 4);
	seq4 = inb(VGA_SEQ_DATA);

	outb(VGA_SEQ_DATA, seq4 | 0x04);

	outb(VGA_GC_INDEX, 4);
	gc4 = inb(VGA_GC_DATA);

	outb(VGA_GC_INDEX, 5);
	gc5 = inb(VGA_GC_DATA);

	outb(VGA_GC_DATA, gc5 & ~0x10);

	outb(VGA_GC_INDEX, 6);
	gc6 = inb(VGA_GC_DATA);

	outb(VGA_GC_DATA, gc6 & ~0x02);

	set_plane(2);

	for (i = 0; i < 256; i++) {
		vmemwr(16384u * 0 + i * 32, buf, font_height);
		buf += font_height;
	}

#if 0
	for (i = 0; i < 256; i++) {
		vmemwr(16384u * 1 + i * 32, buf, font_height);
		buf += font_height;
	}
#endif

	outb(VGA_SEQ_INDEX, 2);
	outb(VGA_SEQ_DATA, seq2);
	outb(VGA_SEQ_INDEX, 4);
	outb(VGA_SEQ_DATA, seq4);
	outb(VGA_GC_INDEX, 4);
	outb(VGA_GC_DATA, gc4);
	outb(VGA_GC_INDEX, 5);
	outb(VGA_GC_DATA, gc5);
	outb(VGA_GC_INDEX, 6);
	outb(VGA_GC_DATA, gc6);
}

void set_font(char *fontbuf)
{
	unsigned rows, cols, ht;
	cols = 80;
	rows = 25;
	ht = 16;

	/* 设置字库 */
	write_font((unsigned char *)fontbuf, 16);
	pokew(0x40, 0x4A, cols);			// 屏幕上的列
	pokew(0x40, 0x4C, cols * rows * 2);	// 帧缓冲器大小
	pokew(0x40, 0x50, 0);				// 光标位置
	pokeb(0x40, 0x60, ht - 1);			// 光标形状
	pokeb(0x40, 0x61, ht - 2);
	pokeb(0x40, 0x84, rows - 1);		// 屏幕上的行数 - 1
	pokeb(0x40, 0x85, ht);				// 字符高度
}

void init_kfont()
{
	extern char kfont[4096];
	set_font(kfont);
}
