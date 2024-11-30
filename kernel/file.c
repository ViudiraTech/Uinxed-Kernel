/*
 *
 *		file.c
 *		文件操作抽象层
 *
 *		2024/11/30 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "file.h"
#include "memory.h"
#include "string.h"
#include "types.h"
#include "printk.h"

vfs_node_t working_dir = NULL; // 当前工作目录

/* 文件操作抽象层初始化 */
void file_init(void)
{
	working_dir = vfs_open("/");
	print_succ("File abstraction working is initialize.\n");
}

/* 将vfs_node_t结构体路径转为字符串 */
char *vfs_node_to_path(vfs_node_t node)
{
	if (node == NULL) {
		return NULL;
	}
	if (node->parent == NULL) {
		char* path = strdup("/");
		return path;
	} else {
		char* parent_path = vfs_node_to_path(node->parent);
		if (parent_path == NULL) {
			return NULL;
		}
		char* path = (char *)kmalloc(strlen(parent_path) + strlen(node->name) + 2);
		if (path == NULL) {
			kfree(parent_path);
			return NULL;
		}
		strcpy(path, parent_path);
		if (strcmp(parent_path, "/") != 0) {
			strcat(path, "/");
		}
		strcat(path, node->name);
		kfree(parent_path);
		return path;
	}
}
