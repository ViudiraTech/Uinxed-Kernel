/*
 * 
 * 		vfs.c
 * 		文件系统抽象层
 * 
 * 		2024/11/23 By Rainy101112
 * 		基于MIT协议 兼容GPL-3.0
 * 		Copyright plos-clan by min0911Y & zhouzhihao & copi143
 * 
 */

#include "string.h"
#include "printk.h"
#include "vfs.h"
#include "list.h"
#include "string.h"
#include "memory.h"

#define finline static
#define ALL_IMPLEMENTATION

vfs_node_t rootdir = 0;

static void empty_func(void) {}

struct vfs_callback vfs_empty_callback;
static vfs_callback_t fs_callbacks[256] = {
	[0] = &vfs_empty_callback,
};
static int fs_nextid = 1;

#define callbackof(node, _name_) (fs_callbacks[(node)->fsid]->_name_)

finline char *pathtok(char **sp)
{
	char *s = *sp, *e = *sp;
	if (*s == '\0') return 0;
	for (; *e != '\0' && *e != '/'; e++) {}
	*sp = e + (*e != '\0' ? 1 : 0);
	*e = '\0';
	return s;
}

finline void do_open(vfs_node_t file)
{
	if (file->handle != 0) {
		callbackof(file, stat)(file->handle, file);
	} else {
		callbackof(file, open)(file->parent->handle, file->name, file);
	}
}

finline void do_update(vfs_node_t file)
{
	if (file->type == file_none || file->handle == 0) do_open(file);
}

vfs_node_t vfs_child_append(vfs_node_t parent, const char* name, void *handle)
{
	vfs_node_t node = vfs_node_alloc(parent, name);
	if (node == 0) return 0;
	node->handle = handle;
	return node;
}

static vfs_node_t vfs_child_find(vfs_node_t parent, const char* name)
{
	return list_first(parent->child, data, streq(name, ((vfs_node_t)data)->name));
}

int vfs_mkdir(const char* name)
{
	if (name[0] != '/') return -1;
	char *path = strdup(name + 1);
	char *save_ptr = path;
	vfs_node_t current = rootdir;
	for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
		vfs_node_t father = current;
		if (streq(buf, ".")) {
			goto upd;
		} else if (streq(buf, "..")) {
			if (current->parent && current->type == file_dir) {
				current = current->parent;
				goto upd;
			} else {
				goto err;
			}
		}
		current = vfs_child_find(current, buf);
		upd:
		if (current == 0) {
			current = vfs_node_alloc(father, buf);
			current->type = file_dir;
			callbackof(father, mkdir)(father->handle, buf, current);
		} else {
			do_update(current);
			if (current->type != file_dir) goto err;
		}
	}
	kfree(path);
	return 0;
	err:
	kfree(path);
	return -1;
}

int vfs_mkfile(const char* name)
{
	if (name[0] != '/') return -1;
	char *path = strdup(name + 1);
	char *save_ptr = path;
	vfs_node_t current = rootdir;
	char *filename = path + strlen(path);

	while (*--filename != '/' && filename != path);
	if (filename != path) {
		*filename++ = '\0';
	} else {
		goto create;
	}

	if (strlen(path) == 0) {
		kfree(path);
		return -1;
	}
	for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
		if (streq(buf, ".")) continue;
		if (streq(buf, "..")) {
			if (!current->parent || current->type != file_dir) goto err;
				current = current->parent;
				continue;
			}
		current = vfs_child_find(current, buf);
		if (current == null || current->type != file_dir) goto err;
	}
create:
	vfs_node_t node = vfs_child_append(current, filename, null);
	node->type = file_block;
	callbackof(current, mkfile)(current->handle, filename, node);

	kfree(path);
	return 0;
err:
	kfree(path);
	return -1;
}

int vfs_regist(const char* name, vfs_callback_t callback)
{
	if (callback == 0) return -1;
	for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) {
		if (((void **)callback)[i] == 0) return -1;
	}
	int id = fs_nextid++;
	fs_callbacks[id] = callback;
	return id;
}

vfs_node_t vfs_do_search(vfs_node_t dir, const char* name)
{
	return list_first(dir->child, data, streq(name, ((vfs_node_t)data)->name));
}

vfs_node_t vfs_open(const char* str)
{
	if (str == 0) return 0;
	if (str[1] == '\0') return rootdir;
	char *path = strdup(str + 1);
	if (path == 0) return 0;
	char *save_ptr = path;
	vfs_node_t current = rootdir;
	for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
		if (streq(buf, ".")) {
			goto upd;
		} else if (streq(buf, "..")) {
			if (current->parent && current->type == file_dir) {
				current = current->parent;
				goto upd;
			} else {
				goto err;
			}
		}
		current = vfs_child_find(current, buf);
		if (current == 0) goto err;
		upd:
		do_update(current);
	}
	kfree(path);
	return current;
err:
	kfree(path);
	return 0;
}

void vfs_update(vfs_node_t node)
{
	do_update(node);
}

vfs_node_t get_rootdir(void)
{
	return rootdir;
}

vfs_node_t vfs_node_alloc(vfs_node_t parent, const char* name)
{
	vfs_node_t node = (vfs_node_t)(kmalloc(sizeof(struct vfs_node)));
	if (node == 0) return 0;
	memset(node, 0, sizeof(struct vfs_node));
	node->parent = parent;
	node->name = name ? strdup(name) : 0;
	node->type = file_none;
	node->fsid = parent ? parent->fsid : 0;
	node->root = parent ? parent->root : node;
	if (parent) list_prepend(parent->child, node);
	return node;
}

int vfs_close(vfs_node_t node)
{
	if (node == 0) return -1;
	if (node->handle == 0) return 0;
	callbackof(node, close)(node->handle);
	node->handle = 0;
	return 0;
}

void vfs_free(vfs_node_t vfs)
{
	if (vfs == 0) return;
	list_free_with(vfs->child, (void (*)(void *))vfs_free);
	vfs_close(vfs);
	kfree(vfs->name);
	kfree(vfs);
}

void vfs_free_child(vfs_node_t vfs)
{
	if (vfs == 0) return;
	list_free_with(vfs->child, (void (*)(void *))vfs_free);
}

int vfs_mount(const char* src, vfs_node_t node)
{
	if (node == 0) return -1;
	if (node->type != file_dir) return -1;
	for (int i = 1; i < fs_nextid; i++) {
		if (fs_callbacks[i]->mount(src, node) == 0) {
			node->fsid = i;
			node->root = node;
			return 0;
		}
	}
	return -1;
}

int vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size)
{
	do_update(file);
	if (file->type != file_block) return -1;
	return callbackof(file, read)(file->handle, addr, offset, size);
}

int vfs_write(vfs_node_t file, void *addr, size_t offset, size_t size)
{
	do_update(file);
	if (file->type != file_block) return -1;
	return callbackof(file, write)(file->handle, addr, offset, size);
}

int vfs_umount(const char* path)
{
	vfs_node_t node = vfs_open(path);
	if (node == 0) return -1;
	if (node->type != file_dir) return -1;
	if (node->fsid == 0) return -1;
	if (node->parent) {
		vfs_node_t cur = node;
		node = node->parent;
		if (cur->root == cur) {
			vfs_free_child(cur);
			callbackof(cur, umount)(cur->handle);
			cur->fsid = node->fsid; // 交给上级
			cur->root = node->root;
			cur->handle = 0;
			cur->child = 0;
			if (cur->fsid) do_update(cur);
			return 0;
		}
	}
	return -1;
}

int vfs_init(void)
{
	for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) {
		((void **)&vfs_empty_callback)[i] = empty_func;
	}
	rootdir = vfs_node_alloc(0, 0);
	rootdir->type = file_dir;
	print_succ("Virtual File System initialize.\n");
	return 1;
}
