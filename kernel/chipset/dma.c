/*
 *
 *		dma.c
 *		直接内存访问功能
 *
 *		2025/1/9 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#include "dma.h"
#include "common.h"

#define LOW_BYTE(x) ((x) & 0x00ff)
#define HIGH_BYTE(x) (((x) & 0xff00) >> 8)

/* 每个DMA通道的快速访问寄存器和端口 */
static const byte MASK_REG[8]	= {0x0A, 0x0A, 0x0A, 0x0A, 0xD4, 0xD4, 0xD4, 0xD4};
static const byte MODE_REG[8]	= {0x0B, 0x0B, 0x0B, 0x0B, 0xD6, 0xD6, 0xD6, 0xD6};
static const byte CLEAR_REG[8]	= {0x0C, 0x0C, 0x0C, 0x0C, 0xD8, 0xD8, 0xD8, 0xD8};

static const byte PAGE_PORT[8]	= {0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A};
static const byte ADDR_PORT[8]	= {0x00, 0x02, 0x04, 0x06, 0xC0, 0xC4, 0xC8, 0xCC};
static const byte COUNT_PORT[8]	= {0x01, 0x03, 0x05, 0x07, 0xC2, 0xC6, 0xCA, 0xCE};

static const unsigned int DMA_ADDR_MAX = (unsigned int)1 << 24;

/* 向DMA控制器发送命令 */
void dma_start(byte mode, byte channel, void *address, unsigned int size)
{
	mode |= (channel % 4);

	if (channel > 4 && size % 2 != 0) return;

	unsigned int addr = (unsigned int)address;

	if (!(addr < DMA_ADDR_MAX)) return;
	if (!(addr + size < DMA_ADDR_MAX)) return;

	byte page = addr >> 16;
	uint16_t offset = (channel > 4 ? addr / 2 : addr) & 0xffff;
	size = (channel > 4 ? size / 2 : size) - 1;

	disable_intr();

	/* 设置DMA通道，以便我们可以正确传输数据，这很简单，只要我们用I/O操作告诉DMA控制器就行了 */
	/* 我们将使用这个通道（DMA_channel） */
	outb(MASK_REG[channel], 0x04 | (channel % 4));

	/* 我们先得解除DMA对这个通道的屏蔽，不然用不了 */
	outb(CLEAR_REG[channel], 0x00);

	/* 向DMA发送指定的模式 */
	outb(MODE_REG[channel], mode);

	/* 发送数据所在的物理页 */
	outb(PAGE_PORT[channel], page);

	/* 发送偏移量地址，先发送高八位，再发送低八位（因为一次性最多只能发送一个byte）*/
	outb(ADDR_PORT[channel], LOW_BYTE(offset));
	outb(ADDR_PORT[channel], HIGH_BYTE(offset));

	/* 发送数据的长度 跟之前一样，先发送低八位，再发送高八位 */
	outb(COUNT_PORT[channel], LOW_BYTE(size));
	outb(COUNT_PORT[channel], HIGH_BYTE(size));

	/* 现在我们该做的东西已经全部做完了，所以启用DMA_channel */
	outb(MASK_REG[channel], (channel % 4));

	// 重新让CPU能够接收到中断
	enable_intr();
}

/* 使用DMA发送数据 */
void dma_send(byte channel, void *address, unsigned int size)
{
	dma_start(0x48, channel, address, size);
}

/* 使用DMA接收数据 */
void dma_recv(byte channel, void *address, unsigned int size)
{
	dma_start(0x44, channel, address, size);
}
