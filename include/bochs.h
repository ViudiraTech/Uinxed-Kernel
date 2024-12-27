/*
 *
 *		bochs.h
 *		bochs图形模式驱动头文件
 *
 *		2024/12/27 By ywx2012
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_BOCHS_H_
#define INCLUDE_BOCHS_H_

#include "multiboot.h"

void init_bochs(multiboot_t *info);

#endif // INCLUDE_BOCHS_H_
