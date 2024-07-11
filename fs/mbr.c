/*
 *
 *		mbr.c
 *		mbr相关
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "mbr.h"
#include "printk.h"

/* MBR信息 */
mbr_info_t mbr_info;

/* 读取分区信息 */
int read_mbr_info(block_t *bdev)
{
	io_request_t request;

	request.io_type = IO_READ;
	request.secno = 0;
	request.nsecs = 1;
	request.buffer = &mbr_info;
	request.bsize = sizeof(mbr_info);

	if (bdev->ops.request(&request) == 0) {
		return 0;
	}
	return -1;
}

void show_partition_info(void)
{
	printk("\nPartition Info:\n");
	for (int i = 0; i < PARTITION_COUNT; ++i) {
		if (mbr_info.part[i].partition_type != 0) {
			printk("Active: %02X  ", mbr_info.part[i].active_flag);
			printk("Type: %02X  ", mbr_info.part[i].partition_type);
			printk("SCHS: %02X%02X%02X  ",  mbr_info.part[i].start_chs[0],
                   mbr_info.part[i].start_chs[1], mbr_info.part[i].start_chs[2]);
			printk("ECHS: %02X%02X%02X  ", mbr_info.part[i].end_chs[0],
                   mbr_info.part[i].end_chs[1], mbr_info.part[i].end_chs[2]);
			printk("Start: %04u  ", mbr_info.part[i].start_sector);
			printk("Count: %05u\n", mbr_info.part[i].nr_sectors);  
		}
	}
}
