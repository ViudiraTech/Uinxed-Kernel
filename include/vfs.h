/*
 *
 *		vfs.c
 *		虚拟文件系统头文件
 *
 *		2024/7/12 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_VFS_H_
#define INCLUDE_VFS_H_

#define toupper(c) ((c) >= 'a' && (c) <= 'z' ? c - 32 : c)

#include "common.h"
#include "list.h"

typedef struct FILE {
	unsigned int mode;
	unsigned int fileSize;
	unsigned char *buffer;
	unsigned int bufferSize;
	unsigned int p;
	char *name;
} FILE;

typedef enum { FLE, DIR, RDO, HID, SYS } ftype;
typedef struct {
	char name[255];
	ftype type;
	unsigned int size;
	unsigned short year, month, day;
	unsigned short hour, minute;
} vfs_file;

typedef struct vfs_t {
	struct List *path;
	void *cache;
	char FSName[255];
	int disk_number;
	uint8_t drive; // 大写（必须）
	vfs_file *(*FileInfo)(struct vfs_t *vfs, char *filename);
	struct List *(*ListFile)(struct vfs_t *vfs, char *dictpath);
	bool (*ReadFile)(struct vfs_t *vfs, char *path, char *buffer);
	bool (*WriteFile)(struct vfs_t *vfs, char *path, char *buffer, int size);
	bool (*DelFile)(struct vfs_t *vfs, char *path);
	bool (*DelDict)(struct vfs_t *vfs, char *path);
	bool (*CreateFile)(struct vfs_t *vfs, char *filename);
	bool (*CreateDict)(struct vfs_t *vfs, char *filename);
	bool (*RenameFile)(struct vfs_t *vfs, char *filename, char *filename_of_new);
	bool (*Attrib)(struct vfs_t *vfs, char *filename, ftype type);
	bool (*Format)(uint8_t disk_number);
	void (*InitFs)(struct vfs_t *vfs, uint8_t disk_number);
	void (*DeleteFs)(struct vfs_t *vfs);
	bool (*Check)(uint8_t disk_number);
	bool (*cd)(struct vfs_t *vfs, char *dictName);
	int (*FileSize)(struct vfs_t *vfs, char *filename);
	void (*CopyCache)(struct vfs_t *dest, struct vfs_t *src);
	int flag;
} vfs_t;

/* 改变当前工作目录 */
bool vfs_change_path(char *dictName);

/* 获取当前路径 */
void vfs_getPath(char *buffer);

/* 获取当前路径但不包括驱动器部分 */
void vfs_getPath_no_drive(char *buffer);

/* 挂载指定驱动器号的磁盘 */
bool vfs_mount_disk(uint8_t disk_number, uint8_t drive);

/* 卸载指定驱动器号的磁盘 */
bool vfs_unmount_disk(uint8_t drive);

/* 读取文件 */
bool vfs_readfile(char *path, char *buffer);

/* 写入文件 */
bool vfs_writefile(char *path, char *buffer, int size);

/* 获取文件大小 */
uint32_t vfs_filesize(char *filename);

/* 列出目录中的文件 */
List *vfs_listfile(char *dictpath);

/* 删除文件 */
bool vfs_delfile(char *filename);

/* 删除目录 */
bool vfs_deldir(char *dictname);

/* 创建文件 */
bool vfs_createfile(char *filename);

/* 创建目录 */
bool vfs_createdict(char *filename);

/* 重命名文件 */
bool vfs_renamefile(char *filename, char *filename_of_new);

/* 设置文件属性 */
bool vfs_attrib(char *filename, ftype type);

/* 格式化磁盘 */
bool vfs_format(uint8_t disk_number, char *FSName);

/* 获取文件信息 */
vfs_file *vfs_fileinfo(char *filename);

/* 切换当前磁盘 */
bool vfs_change_disk(uint8_t drive);

/* 注册新的文件系统类型 */
bool vfs_register_fs(vfs_t vfs);

/* 初始化虚拟文件系统 */
void init_vfs(void);

/* 获取当前目录下与给定文件名匹配的vfs_file结构体的指针 */
vfs_file *get_cur_file(char* filename);

#endif // INCLUDE_VFS_H_
