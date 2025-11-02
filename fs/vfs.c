/*
 * 
 *      vfs.c
 *      Virtual file system
 *
 *      2025/11/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include "vfs.h"
#include "alloc.h"
#include "page.h"
#include "printk.h"
#include "string.h"

vfs_node_t rootdir = 0;

struct vfs_callback   vfs_empty_callback;
static vfs_callback_t fs_callbacks[256] = {[0] = &vfs_empty_callback};
static int            fs_nextid         = 1;

/* Default callback function (does nothing) */
static void empty_func(void)
{
    /* Empty Function */
}

/* Tokenize the path string, splitting it by '/' */
static char *pathtok(char **sp)
{
    char *e = *sp;
    while (*e == '/') e++;
    if (*e == '\0') {
        *sp = e;
        return 0;
    }

    char *s = e;
    while (*e != '\0' && *e != '/') e++;

    char *next = e;
    if (*e == '/') next++;
    if (*e != '\0') *e = '\0';

    *sp = next;
    return s;
}

/* Open a file or directory, invoking the appropriate callback */
static void do_open(vfs_node_t file)
{
    if (file->handle) {
        callbackof(file, stat)(file->handle, file);
    } else {
        callbackof(file, open)(file->parent->handle, file->name, file);
    }
}

/* Update a file or directory node, if necessary */
static void do_update(vfs_node_t file)
{
    if (file->type & file_none || !file->handle || file->type & file_dir || file->type & file_symlink || file->type & file_pipe) do_open(file);
}

/* Add a child node to a parent directory */
static vfs_node_t vfs_child_append(vfs_node_t parent, const char *name, void *handle)
{
    vfs_node_t node = vfs_node_alloc(parent, name);
    if (!node) return 0;

    node->handle = handle;
    return node;
}

/* Find a child node by name within a parent directory */
static vfs_node_t vfs_child_find(vfs_node_t parent, const char *name)
{
    return clist_first(parent->child, data, streq(name, ((vfs_node_t)data)->name));
}

/* Allocate a new vfs node with the given parent and name */
vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name)
{
    vfs_node_t node = (vfs_node_t)(malloc(sizeof(struct vfs_node)));
    if (!node) return 0;

    memset(node, 0, sizeof(struct vfs_node));
    node->parent   = parent;
    node->name     = name ? strdup(name) : 0;
    node->type     = file_none;
    node->fsid     = parent ? parent->fsid : 0;
    node->root     = parent ? parent->root : node;
    node->dev      = parent ? parent->dev : 0;
    node->refcount = 0;
    node->blksz    = PAGE_SIZE;
    node->mode     = 0777;
    node->linkto   = NULL;

    if (parent) clist_prepend(parent->child, node);
    return node;
}

/* Get the root directory node */
vfs_node_t get_rootdir(void)
{
    return rootdir;
}

/* Set the root directory node of the Virtual File System (VFS) */
void set_rootdir(vfs_node_t node)
{
    rootdir         = node;
    rootdir->parent = 0;
}

/* Search for a file or directory by name in the specified directory */
vfs_node_t vfs_do_search(vfs_node_t dir, const char *name)
{
    return clist_first(dir->child, data, streq(name, ((vfs_node_t)data)->name));
}

/* Update a file or directory, ensuring it is open and ready */
void vfs_update(vfs_node_t node)
{
    do_update(node);
}

/* Open a file or directory by path */
vfs_node_t vfs_open(const char *str)
{
    if (!str || str[0] != '/') return 0;
    if (str[1] == '\0') return rootdir;

    char *path = strdup(str + 1);
    if (!path) return 0;

    char      *save_ptr = path;
    vfs_node_t current  = rootdir;

    for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) { continue; }
        if (streq(buf, "..")) {
            if (current->parent) { current = current->parent; }
            continue;
        }

        current = vfs_child_find(current, buf);
        if (!current) goto err;

        do_update(current);
        if (current->type & file_symlink) {
            if (!current->parent || !current->linkto) goto err;
            current->type = file_symlink | file_proxy;

            vfs_node_t target = current->linkto;
            if (!target) goto err;

            target->refcount++;
            current = target;
            continue;
        }
    }
    free(path);
    return current;
err:
    free(path);
    return 0;
}

/* Create a new directory at the specified path */
int vfs_mkdir(const char *name)
{
    if (name[0] != '/') return -1;

    char      *path     = strdup(name + 1);
    char      *save_ptr = path;
    vfs_node_t current  = rootdir;

    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        const vfs_node_t father = current;
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (current->parent && current->type & file_dir) {
                current = current->parent;
                goto upd;
            } else {
                goto err;
            }
        }
        current = vfs_child_find(current, buf);
upd:
        if (!current) {
            current       = vfs_node_alloc(father, buf);
            current->type = file_dir;
            callbackof(father, mkdir)(father->handle, buf, current);
            do_update(current);
        } else {
            do_update(current);
            if (!(current->type & file_dir)) goto err;
        }
    }
    free(path);
    return 0;
err:
    free(path);
    return -1;
}

/* Create a new file at the specified path */
int vfs_mkfile(const char *name)
{
    if (name[0] != '/') return -1;

    char *fullpath  = strdup(name);
    char *filename  = fullpath;
    char *lastslash = strrchr(fullpath, '/');

    if (lastslash == fullpath) {
        filename   = fullpath + 1;
        *lastslash = '\0';
    } else if (lastslash) {
        *lastslash = '\0';
        filename   = lastslash + 1;
    }

    vfs_node_t parent;
    if (lastslash == fullpath) {
        parent = rootdir;
    } else {
        parent = vfs_open(fullpath);
    }
    if (!parent || parent->type != file_dir) {
        free(fullpath);
        return -1;
    }

    vfs_node_t node = vfs_child_append(parent, filename, 0);
    node->type      = file_none;

    int status = callbackof(parent, mkfile)(parent->handle, filename, node);
    free(fullpath);
    return status;
}

/* Create a hard link at the specified path */
int vfs_link(const char *name, const char *target_name)
{
    vfs_node_t node;
    vfs_node_t current = rootdir;
    char      *path    = strdup(name + 1);

    char *save_ptr = path;
    char *filename = path + strlen(path);

    while (*--filename != '/' && filename != path) {}
    if (filename != path) {
        *filename++ = '\0';
    } else {
        goto create;
    }

    if (!strlen(path)) {
        free(path);
        return -1;
    }
    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (!current->parent || !(current->type & file_dir)) goto err;
            current = current->parent;
            continue;
        }
        vfs_node_t new_current = vfs_child_find(current, buf);
        if (!new_current) {
            new_current       = vfs_node_alloc(current, buf);
            new_current->type = file_dir;
            callbackof(current, mkdir)(current->handle, buf, new_current);
        }
        current = new_current;
        do_update(current);

        if (!(current->type & file_dir)) goto err;
    }
create:
    node       = vfs_child_append(current, filename, 0);
    node->type = file_none;

    callbackof(current, link)(current->handle, target_name, node);
    node->linkto = vfs_open(target_name);

    free(path);
    return 0;
err:
    free(path);
    return -1;
}

/* Create a symlink at the specified path */
int vfs_symlink(const char *name, const char *target_name)
{
    vfs_node_t node;
    vfs_node_t current = rootdir;
    char      *path    = strdup(name + 1);

    char *save_ptr = path;
    char *filename = path + strlen(path);

    while (*--filename != '/' && filename != path) {}
    if (filename != path) {
        *filename++ = '\0';
    } else {
        goto create;
    }

    if (!strlen(path)) {
        free(path);
        return -1;
    }
    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (!current->parent || !(current->type & file_dir)) goto err;
            current = current->parent;
            continue;
        }
        vfs_node_t new_current = vfs_child_find(current, buf);
        if (!new_current) {
            new_current       = vfs_node_alloc(current, buf);
            new_current->type = file_dir;
            callbackof(current, mkdir)(current->handle, buf, new_current);
        }
        current = new_current;
        do_update(current);

        if (!(current->type & file_dir)) goto err;
    }
create:
    node       = vfs_child_append(current, filename, 0);
    node->type = file_symlink;

    callbackof(current, symlink)(current->handle, target_name, node);
    node->linkto = vfs_open(target_name);

    free(path);
    return 0;
err:
    free(path);
    return -1;
}

/* Register a vfs callback */
int vfs_regist(vfs_callback_t callback)
{
    if (!callback) return -1;
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) {
        if (((void **)callback)[i] == NULL) return -1;
    }

    int id           = fs_nextid++;
    fs_callbacks[id] = callback;
    return id;
}

/* Mount a file system to a directory */
int vfs_mount(const char *src, vfs_node_t node)
{
    if (!node) return -1;
    if (node->type != file_dir) return -1;
    for (int i = 1; i < fs_nextid; i++) {
        if (!fs_callbacks[i]->mount(src, node)) {
            node->fsid     = i;
            node->root     = node;
            node->is_mount = 1;
            return 0;
        }
    }
    return -1;
}

/* Unmount a file system from a directory */
int vfs_umount(const char *path)
{
    vfs_node_t node = vfs_open(path);

    if (!node || node->type != file_dir || !node->fsid) return -1;
    if (node->parent) {
        vfs_node_t cur = node;
        node           = node->parent;
        if (cur->root == cur) {
            vfs_free_child(cur);
            callbackof(cur, unmount)(cur->handle);
            cur->fsid     = node->fsid;
            cur->root     = node->root;
            cur->handle   = 0;
            cur->child    = 0;
            cur->is_mount = 0;
            if (cur->fsid) do_update(cur);
            return 0;
        }
    }
    return -1;
}

/* Read data from a file node into the provided memory buffer */
size_t vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size)
{
    if (!file || !addr) return -1;
    do_update(file);

    if (file->type == file_dir) return -1;
    return callbackof(file, read)(file->handle, addr, offset, size);
}

/* Read data from a link file node into the provided memory buffer */
size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize)
{
    size_t ret = callbackof(node, readlink)(node, buf, 0, bufsize);
    return ret;
}

/* Write data from the provided memory buffer to a file node */
size_t vfs_write(vfs_node_t file, void *addr, size_t offset, size_t size)
{
    if (!file || !addr) return -1;
    do_update(file);

    if (file->type == file_dir) return -1;
    size_t ret = callbackof(file, write)(file->handle, addr, offset, size);

    do_update(file);
    return ret;
}

/* Close the file or directory node */
int vfs_close(vfs_node_t node)
{
    if (!node) return -1;
    if (node == rootdir || !node->handle) return 0;

    node->refcount--;

    if (node->type & file_proxy || node->type & file_dir || node->refcount) return 0;
    if (node->type & file_delete) {
        int res = callbackof(node, delete)(node->parent->handle, node);
        if (res < 0) return res;
        clist_delete(node->parent->child, node);
        node->handle = 0;
        vfs_free(node);
    } else {
        callbackof(node, close)(node->handle);
        node->handle = 0;
    }
    return 0;
}

/* Delete a VFS (Virtual File System) node and clean up associated resources */
int vfs_delete(vfs_node_t node)
{
    if (node == rootdir) return -1;
    node->type |= file_delete;
    return 0;
}

/* Rename a VFS (Virtual File System) node to a new name */
int vfs_rename(vfs_node_t node, const char *new)
{
    return callbackof(node, rename)(node->handle, new);
}

/* Free all child nodes of a VFS node */
void vfs_free_child(vfs_node_t vfs)
{
    if (!vfs) return;
    clist_free_with(vfs->child, (void (*)(void *))vfs_free);
}

/* Free the memory associated with a vfs node */
void vfs_free(vfs_node_t vfs)
{
    if (!vfs) return;
    clist_free_with(vfs->child, (void (*)(void *))vfs_free);
    vfs_close(vfs);
    free(vfs->name);
    free(vfs);
}

/* Initialize the virtual file system */
void init_vfs(void)
{
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) ((void **)&vfs_empty_callback)[i] = empty_func;
    rootdir       = vfs_node_alloc(0, "/");
    rootdir->type = file_dir;
    plogk("vfs: Initial root directory of the virtual file system: '/'\n");
}
