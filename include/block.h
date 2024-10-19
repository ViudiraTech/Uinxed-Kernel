/*
 *
 *		block.h
 *		块设备头文件
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_BLOCK_H_
#define INCLUDE_BLOCK_H_

#include "types.h"

typedef
enum io_type {
	IO_READ		= 0,
	IO_WRITE	= 1
} io_type_t;

typedef 
struct io_request {
	io_type_t	io_type;				// IO操作类型
	uint32_t	secno;					// 起始位置
	uint32_t	nsecs;					// 读写数量
	void		*buffer;				// 读写缓冲区
	uint32_t	bsize;					// 缓冲区尺寸
} io_request_t;

/* 块设备接口 */
typedef
struct block {
	const char *name;					// 设备名称
	uint32_t block_size;				// 单位块大小
	struct block_ops {					// 设备操作
		int (*init)(void);				// 设备初始化
		bool (*device_valid)(void);		// 设备是否可用
		const char *(*get_desc)(void);	// 获取设备描述
		int (*get_nr_block)(void);		// 获得设备默认块数量
		int (*request)(io_request_t *);	// 设备操作请求
		int (*ioctl)(int, int);			// 设备选项设置
	} ops;
	struct block_dev *next;				// 块设备链
} block_t;

/* 全局块设备链表 */
extern block_t *block_devs;

/* 块设备初始化 */
void block_init(void);

/* 内核注册块设备 */
int add_block(block_t *bdev);

/* IDE 设备结构 */
extern block_t ide_main_dev;

#endif // INCLUDE_BLOCK_H_
