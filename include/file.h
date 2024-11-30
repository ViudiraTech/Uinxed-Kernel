/*
 *
 *		file.h
 *		文件操作抽象层头文件
 *
 *		2024/11/30 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_FILE_H_
#define INCLUDE_FILE_H_

#include "vfs.h"

extern vfs_node_t working_dir; // 当前工作目录

/* 文件操作抽象层初始化 */
void file_init(void);

/* 将vfs_node_t结构体路径转为字符串 */
char *vfs_node_to_path(vfs_node_t node);

#endif // INCLUDE_FILE_H_
