/*
 *
 *		block.c
 *		块设备
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "string.h"
#include "block.h"
#include "mbr.h"
#include "printk.h"

/* 全局块设备链表 */
block_t *block_devs;

/* 块设备初始化 */
void block_init(void)
{
	print_busy("Init IDE Driver ...\r");

	block_t *ide_dev = &ide_main_dev;
	int status = ide_dev->ops.init();
	if (status == -1) {
		print_warn("Main IDE Device Not Found!\n");
		return;
	} else if (status == -2) {
		print_warn("The IDE controller could not be found!\n");
		return;
	}
	add_block(ide_dev);

	if (!ide_dev->ops.device_valid()) {
		print_erro("Main IDE Device Error!\n");
		return;
	}
	print_succ("Found IDE Driver:");
	printk(" %u(sectors) Desc: %s\n",
           ide_dev->ops.get_nr_block(), ide_dev->ops.get_desc());

	if (read_mbr_info(ide_dev) != 0) {
		print_erro("Read MBR Info Error!\n");
	}
}

/* 内核注册块设备 */
int add_block(block_t *bdev)
{
	block_t *p = block_devs;
	while (p) {
		if (strcmp(p->name, bdev->name) == 0) {
			return -1;
		}
		bdev->next = (struct block_dev *)block_devs;
	}
	bdev->next = (struct block_dev *)block_devs;
	block_devs = bdev;
	return 0;
}
