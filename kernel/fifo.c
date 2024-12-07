/*
 *
 *		fifo.c
 *		FIFO先进先出结构
 *
 *		2024/2/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "fifo.h"

void fifo_init(fifo_t *fifo, int size, uint32_t *buf)
{
	fifo->size	= size;
	fifo->buf	= buf;
	fifo->free	= size;
	fifo->flags	= 0;
	fifo->p		= 0;
	fifo->q		= 0;
}

int fifo_put(fifo_t *fifo, uint32_t data)
{
	if (fifo->free == 0) {
		fifo->flags |= FIFO_FLAGS_OVERRUN;
		return -1;
	}

	fifo->buf[fifo->p] = data;
	fifo->p++;

	if (fifo->p == fifo->size) fifo->p = 0;
	fifo->free--;

	return 0;
}

int fifo_get(fifo_t *fifo)
{
	int data;

	if (fifo->free == fifo->size) return - 1;

	data = fifo->buf[fifo->q];
	fifo->q++;

	if (fifo->q == fifo->size) fifo->q = 0;
	fifo->free++;

	return data;
}

int fifo_status(fifo_t *fifo)
{
	return fifo->size - fifo->free;
}

void fifo8_init(fifo8_t *fifo, int size, uint8_t *buf)
{
	fifo->size = size;
	fifo->buf = buf;
	fifo->free = size;
	fifo->flags = 0;
	fifo->p = 0;
	fifo->q = 0;
}

int fifo8_put(fifo8_t *fifo, uint8_t data)
{
	if (fifo->free == 0) {
		fifo->flags |= FIFO_FLAGS_OVERRUN;
		return -1;
	}

	fifo->buf[fifo->p] = data;
	fifo->p++;

	if (fifo->p == fifo->size) fifo->p = 0;
	fifo->free--;

	return 0;
}

int fifo8_get(fifo8_t *fifo)
{
	int data;

	if (fifo->free == fifo->size) return - 1;

	data = fifo->buf[fifo->q];
	fifo->q++;

	if (fifo->q == fifo->size) fifo->q = 0;
	fifo->free++;

	return data;
}

int fifo8_status(fifo8_t *fifo)
{
	return fifo->size - fifo->free;
}
