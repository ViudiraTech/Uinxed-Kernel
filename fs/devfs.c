/*
 *
 *		devfs.c
 *		块设备文件系统
 *
 *		2024/11/24 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "devfs.h"
#include "vdisk.h"
#include "printk.h"
#include "debug.h"
#include "memory.h"
#include "rbtree-strptr.h"
//#include "rbtree.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-conversion"
#define PAGE_SIZE 4096
#define PADDING_DOWN(size, to) ((size_t)(size) / (size_t)(to) * (size_t)(to))
#define PADDING_UP(size, to)   PADDING_DOWN((size_t)(size) + (size_t)(to) - (size_t)1, to)

int devfs_id = 0;
extern vdisk vdisk_ctl[26]; // core/vdisk.c
rbtree_sp_t  dev_rbtree;
vfs_node_t device_fs_node;

static void dummy(void) {}

static int devfs_mkdir(void *parent,const char* name, vfs_node_t node)
{
	printk("You cannot create directory in devfs");
	node->fsid = 0; // 交给vfs处理
	return 0;
}

void print_devfs(void)
{
	rbtree_sp_print_inorder(dev_rbtree);
	rbtree_sp_print_postorder(dev_rbtree);
	rbtree_sp_print_preorder(dev_rbtree);
}

int devfs_mount(const char* src, vfs_node_t node)
{
	if(src) return -1;
	node->fsid = devfs_id;
	for (int i = 0; have_vdisk(i); i++) {
		vfs_child_append(node, vdisk_ctl[i].DriveName, 0);
		rbtree_sp_insert(dev_rbtree, vdisk_ctl[i].DriveName, (void *)i);
	}
	return 0;
}

static int devfs_read(void *file, void *addr, size_t offset, size_t size)
{
	int dev_id = (int)file;
	int sector_size;
	if (vdisk_ctl[dev_id].flag == 0) return -1;
	sector_size                     = vdisk_ctl[dev_id].sector_size;
	int   padding_up_to_sector_size = PADDING_UP(size, sector_size);
	void *buf;
	if (padding_up_to_sector_size == size) {
		buf = addr;
	} else {
		buf = kmalloc(padding_up_to_sector_size * PAGE_SIZE);
	}
	int sectors_to_do = size / sector_size;
	Disk_Read(offset / sector_size, sectors_to_do, buf, dev_id);
	if (padding_up_to_sector_size != size) {
		memcpy(addr, buf, size);
		kfree(buf);
	}
	return 0;
}

static int devfs_write(void *file, const void *addr, size_t offset, size_t size)
{
	int dev_id = (int)file;
	int sector_size;
	if (vdisk_ctl[dev_id].flag == 0) return -1;
	sector_size                     = vdisk_ctl[dev_id].sector_size;
	int   padding_up_to_sector_size = PADDING_UP(size, sector_size);
	void *buf;
	if (padding_up_to_sector_size == size) {
		buf = (void *)addr;
	} else {
		buf = kmalloc(padding_up_to_sector_size * PAGE_SIZE);
		memset(buf, 0, padding_up_to_sector_size);
		memcpy(buf, addr, size);
	}
	int sectors_to_do = size / sector_size;
	Disk_Write(offset / sector_size, sectors_to_do, buf, dev_id);
	if (padding_up_to_sector_size != size) {
		memcpy(buf, addr, size);
		kfree(buf);
	}
	return 0;
}

static void devfs_open(void *parent,const char* name, vfs_node_t node)
{
	node->handle = rbtree_sp_get(dev_rbtree, name);
	node->type   = file_block;
	node->size   = disk_Size((int)node->handle);
}

static struct vfs_callback callbacks = {
	.mount   = devfs_mount,
	.unmount = (void *)dummy,
	.mkdir   = devfs_mkdir,
	.mkfile  = (void *)dummy,
	.open    = devfs_open,
	.close   = (void *)dummy,
	.stat    = (void *)dummy,
	.read    = devfs_read,
	.write   = devfs_write,
};

void devfs_regist(void)
{
	devfs_id = vfs_regist("devfs", &callbacks);
	vfs_mkdir("/dev");
	device_fs_node = vfs_open("/dev");
	vfs_mount(0, device_fs_node);
	print_succ("Device File System initialize.\n");
}

int dev_get_sector_size(char *path)
{
	vfs_node_t node = vfs_open(path);
	if (node == 0) return -1;
	if (node->fsid != devfs_id) return -1; //不是devfs
	int dev_id      = (int)node->handle;
	int sector_size = vdisk_ctl[dev_id].sector_size;
	return sector_size;
}

int dev_get_size(char *path)
{
	vfs_node_t node = vfs_open(path);
	if (node == 0) return -1;
	if (node->fsid != devfs_id) return -1; //不是devfs
	int dev_id = (int)node->handle;
	int size   = disk_Size(dev_id);
	return size;
}

int dev_get_type(char *path)
{
	vfs_node_t node = vfs_open(path);
	if (node == 0) return -1;
	if (node->fsid != devfs_id) return -1; //不是devfs
	int dev_id = (int)node->handle;
	int type   = vdisk_ctl[dev_id].flag;
	return type;
}

#pragma GCC diagnostic pop
