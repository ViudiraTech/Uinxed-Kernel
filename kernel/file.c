/*
 *
 *		file.c
 *		文件操作抽象层
 *
 *		2024/11/30 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#include "file.h"
#include "memory.h"
#include "string.h"
#include "types.h"
#include "printk.h"

vfs_node_t working_dir = 0; // 当前工作目录

/* 文件操作抽象层初始化 */
void file_init(void)
{
	working_dir = vfs_open("/");
	print_succ("File abstraction working is initialize.\n");
}

/* 将vfs_node_t结构体路径转为字符串 */
char *vfs_node_to_path(vfs_node_t node)
{
	if (node == 0) {
		return 0;
	}
	if (node->parent == 0) {
		char* path = strdup("/");
		return path;
	} else {
		char* parent_path = vfs_node_to_path(node->parent);
		if (parent_path == 0) {
			return 0;
		}
		char* path = (char *)kmalloc(strlen(parent_path) + strlen(node->name) + 2);
		if (path == 0) {
			kfree(parent_path);
			return 0;
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

/* 切换工作目录 */
int file_cd(const char *path)
{
	char *s = (char *)path;
	char *wd = vfs_node_to_path(working_dir);
	if (s[strlen(s) - 1] == '/' && strlen(s) > 1) { s[strlen(s) - 1] = '\0'; }
	if (streq(s, ".")) return 0;
	if (streq(s, "..")) {
		if (streq(wd, "/")) return -1;
		char *n = wd + strlen(wd);
		while (*--n != '/' && n != wd) {}
		*n = '\0';
		if (strlen(wd) == 0) strcpy(wd, "/");
		working_dir = vfs_open(wd);
		return 0;
	}
	char *old = strdup(wd);
	if (s[0] == '/') {
		strcpy(wd, s);
		working_dir = vfs_open(wd);
	} else {
		if (streq(wd, "/")) {
			sprintf(wd, "%s%s", wd, s);
			working_dir = vfs_open(wd);
		} else {
			sprintf(wd, "%s/%s", wd, s);
			working_dir = vfs_open(wd);
		}
	}
	if (vfs_open(wd) == 0) {
		sprintf(wd, "%s", old);
		working_dir = vfs_open(wd);
		kfree(old);
		kfree(wd);
		return -1;
	}
	kfree(old);
	kfree(wd);
	return 0;
}

/* 列出制定目录下的文件 */
int file_ls(const char *path)
{
	vfs_node_t p = vfs_open(path);
	if (p) {
		list_foreach(p->child, i) {
			vfs_node_t c = (vfs_node_t)i->data;
			printk("%s ", c->name);
		}
		printk("\n");
		return 0;
	} else {
		return -1;
	}
}
