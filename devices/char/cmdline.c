/*
 *
 *		cmdline.c
 *		内核命令行
 *
 *		2025/3/9 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "limine.h"

__attribute__((used, section(".limine_requests")))
static __volatile__ struct limine_kernel_file_request kernel_file_request = {
	.id = LIMINE_KERNEL_FILE_REQUEST,
	.revision = 0,
	.response = 0
};

/* 获取内核命令行 */
const char *get_cmdline(void)
{
	return kernel_file_request.response->kernel_file->cmdline;
}
