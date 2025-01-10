/*
 *
 *		iso9660.c
 *		iso9660（CDFS）文件系统
 *
 *		2025/1/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "iso9660.h"
#include "memory.h"
#include "string.h"
#include "printk.h"

static l9660_status l9660_seek(l9660_file *f, int whence, int32_t offset);
static l9660_status l9660_readdir(l9660_dir *dir, l9660_dirent **pdirent);

#define l9660_seekdir(dir, pos) (l9660_seek(&(dir)->file, L9660_SEEK_SET, (pos)))

#define SEEK_END L9660_SEEK_END
#define SEEK_SET L9660_SEEK_SET
#define SEEK_CUR L9660_SEEK_CUR

#define DENT_ISDIR (1 << 1)

#define PVD(vdesc) ((l9660_vdesc_primary *)(vdesc))

#ifdef L9660_BIG_ENDIAN
#	define READ32(v) (((v).be[3]) | ((v).be[2] << 8) | ((v).be[1]) << 16 | ((v).be[0] << 24))
#else
#	define READ32(v) (((v).le[0]) | ((v).le[1] << 8) | ((v).le[2]) << 16 | ((v).le[3] << 24))
#endif

#ifndef L9660_SINGLEBUFFER
#	define HAVEBUFFER(f)	(1)
#	define BUF(f)			((f)->buf)
#else
#	define HAVEBUFFER(f)	((f) == last_file)
#	define BUF(f)			(gbuf)
static l9660_file *last_file;
static char gbuf[2048];
#endif

int iso9660_id = -1;

static inline uint16_t fsectoff(l9660_file *f)
{
	return f->position % 2048;
}

static inline uint32_t fsector(l9660_file *f)
{
	return f->position / 2048;
}

static inline uint32_t fnextsectpos(l9660_file *f)
{
	return (f->position + 2047) & ~2047;
}

static l9660_status l9660_openfs(l9660_fs *fs, int (*read_sector)
                         (l9660_fs *fs, void *buf, uint32_t sector), vfs_node_t device)
{
	fs->read_sector = read_sector;
	fs->device = device;
#ifndef L9660_SINGLEBUFFER
	l9660_vdesc_primary *pvd = PVD(&fs->pvd);
#else
	last_file  = 0;
	l9660_vdesc_primary *pvd = PVD(gbuf);
#endif
	uint32_t idx = 0x10;
	while (1) {
		if (!read_sector(fs, pvd, idx)) return L9660_EIO;
		if (memcmp(pvd->hdr.magic, "CD001", 5) != 0) return L9660_EBADFS;
		if (pvd->hdr.type == 1)
			break;
		else if (pvd->hdr.type == 255)
			return L9660_EBADFS;
	}
#ifdef L9660_SINGLEBUFFER
	memcpy(&fs->root_dir_ent, &pvd->root_dir_ent, pvd->root_dir_ent.length);
#endif
	return L9660_OK;
}

static l9660_status l9660_fs_open_root(l9660_dir *dir, l9660_fs *fs)
{
	l9660_file *f = &dir->file;
#ifndef L9660_SINGLEBUFFER
	l9660_dirent *dirent = &PVD(&fs->pvd)->root_dir_ent;
#else
	l9660_dirent *dirent = &fs->root_dir_ent;
#endif
	f->fs = fs;
	f->first_sector = READ32(dirent->sector);
	f->length = READ32(dirent->size);
	f->position = 0;
	return L9660_OK;
}

static l9660_status buffer(l9660_file *f)
{
#ifdef L9660_SINGLEBUFFER
	last_file = f;
#endif
	if (!f->fs->read_sector(f->fs, BUF(f), f->first_sector + f->position / 2048))
		return L9660_EIO;
	else
		return L9660_OK;
}

static l9660_status prebuffer(l9660_file *f)
{
	if (!HAVEBUFFER(f) || (f->position % 2048) == 0)
		return buffer(f);
	else
		return L9660_OK;
}

static l9660_status openat_raw(l9660_file *child, l9660_dir *parent, const char *name, int isdir)
{
	l9660_status rv;
	l9660_dirent *dent = 0;
	if ((rv = l9660_seekdir(parent, 0))) return rv;
	do {
		const char *seg = name;
		name = strchrnul(name, '/');
		size_t seglen = name - seg;

		/* ISO9660 stores '.' as '\0' */
		if (seglen == 1 && *seg == '.') seg = "\0";

		/* ISO9660 stores ".." as '\1' */
		if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
			seg = "\1";
			seglen = 1;
		}
		while (1) {
			if ((rv = l9660_readdir(parent, &dent))) return rv;

			/* EOD */
			if (!dent) return L9660_ENOENT;

			/* wrong length */
			if (seglen > dent->name_len) continue;

			/* check name */
			if (memcmp(seg, dent->name, seglen) != 0) continue;

			/* check for a revision tag */
			if (dent->name_len > seglen && dent->name[seglen] != ';') continue;

			/* all tests pass */
			break;
		}
		child->fs = parent->file.fs;
		child->first_sector = READ32(dent->sector) + dent->xattr_length;
		child->length = READ32(dent->size);
		child->position = 0;
		parent->file.position = 0;
		if (*name && (dent->flags & DENT_ISDIR) != 0) return L9660_ENOTDIR;

		parent = (l9660_dir *)child;
	} while (*name);

	if (isdir) {
		if ((dent->flags & DENT_ISDIR) == 0) return L9660_ENOTDIR;
	} else {
		if ((dent->flags & DENT_ISDIR) != 0) return L9660_ENOTFILE;
	}
	return L9660_OK;
}

static l9660_status l9660_opendirat(l9660_dir *dir, l9660_dir *parent, const char *path)
{
	return openat_raw(&dir->file, parent, path, 1);
}

static inline unsigned aligneven(unsigned v)
{
	return v + (v & 1);
}

static l9660_status l9660_readdir(l9660_dir *dir, l9660_dirent **pdirent)
{
	l9660_status rv;
	l9660_file *f = &dir->file;

	rebuffer:
	if (f->position >= f->length) {
		*pdirent = 0;
		f->position = 0;
		return L9660_OK;
	}
	if ((rv = prebuffer(f))) return rv;
	char *off = BUF(f) + fsectoff(f);

	if (*off == 0) {
		f->position = fnextsectpos(f);
		goto rebuffer;
	}
	l9660_dirent *dirent = (l9660_dirent *)off;
	f->position += aligneven(dirent->length);

	*pdirent = dirent;
	return L9660_OK;
}

static l9660_status l9660_openat(l9660_file *child, l9660_dir *parent, const char *name)
{
	return openat_raw(child, parent, name, 0);
}

static l9660_status l9660_seek(l9660_file *f, int whence, int32_t offset)
{
	l9660_status rv;
	uint32_t cursect = fsector(f);

	switch (whence) {
		case SEEK_SET: f->position = offset; break;
		case SEEK_CUR: f->position = f->position + offset; break;
		case SEEK_END: f->position = f->length - offset; break;
	}
	if (fsector(f) != cursect && fsectoff(f) != 0) {
		if ((rv = buffer(f))) return rv;
	}
	return L9660_OK;
}

static l9660_status l9660_read(l9660_file *f, void *buf, size_t size, size_t *read)
{
	l9660_status rv;

	if ((rv = prebuffer(f))) return rv;
	uint16_t rem = 2048 - fsectoff(f);

	if (rem > f->length - f->position) rem = f->length - f->position;
	if (rem < size) size = rem;

	memcpy(buf, (const uint8_t *)BUF(f) + fsectoff(f), size);

	*read = size;
	f->position += size;
	return L9660_OK;
}

static int read_sector(l9660_fs *fs, void *buf, uint32_t sector)
{
	return vfs_read(fs->device, buf, sector * 2048, 2048) == -1 ? 0 : 1;
}

static int iso9660_mkdir(void *parent, const char* name, vfs_node_t node)
{
	return -1;
}

static int iso9660_mkfile(void *parent, const char* name, vfs_node_t node)
{
	return -1;
}

static int iso9660_readfile(file_t file, void *addr, size_t offset, size_t size)
{
	if (file->type == file_dir) return -1;

	l9660_file *fp = file->handle;
	size = fp->length < size ? fp->length : size;
	l9660_status st;
	st = l9660_seek(fp, SEEK_SET, offset);

	if (st != L9660_OK) return -1;

	size_t read = 0;
	size_t total_read = 0;
	while (total_read < size) {
		st = l9660_read(fp, (char *)addr + total_read, size - total_read, &read);
		if (st != L9660_OK) return -1;
		total_read += read;
		if (read == 0) break;
	}
	if (st != L9660_OK) return -1;
	return 0;
}

static int iso9660_writefile(file_t file, const void *addr, size_t offset, size_t size)
{
	return -1;
}

static void iso9660_process_dir(l9660_dir *dir, vfs_node_t parent)
{
	while (1) {
		l9660_dirent *dent = 0;
		l9660_readdir(dir, &dent);

		if (dent == 0) break;
		int j = 0;
		if (memcmp("\0", dent->name, dent->name_len) == 0) {
			continue;
		} else if (memcmp("\1", dent->name, dent->name_len) == 0) {
			continue;
		} else {
			char name[255] = {0};
			for (; j < dent->name_len; j++) {
				if (dent->name[j] == ';') break;
				name[j] = dent->name[j];
			}
			name[j] = 0;
			vfs_child_append(parent, name, 0);
		}
	}
}

static void iso9660_open(void *parent, const char* name, vfs_node_t node)
{
	file_t p = parent;
	l9660_dir *p_dir = (l9660_dir *)p->handle;
	l9660_dir *c_dir = (l9660_dir *)kmalloc(sizeof(l9660_dir));
	l9660_file *c_file = (l9660_file *)kmalloc(sizeof(l9660_file));
	l9660_status status;
	file_t new = (file_t)kmalloc(sizeof(struct file));
	status = l9660_openat(c_file, p_dir, name);

	if (status != L9660_OK) {
		status = l9660_opendirat(c_dir, p_dir, name);
		if (status != L9660_OK) {
			kfree(c_dir);
			kfree(c_file);
			return;
		}
		node->fsid = iso9660_id;
		iso9660_process_dir(c_dir, node);
		kfree(c_file);
		new->type = file_dir;
		new->handle = (void *)c_dir;
		node->type = file_dir;
		node->handle = (void *)new;
		return;
	}
	node->fsid = iso9660_id;
	node->type = file_block;
	new->type = file_block;
	new->handle = (void *)c_file;
	node->handle = (void *)new;
	node->size = c_file->length;
	kfree(c_dir);
	return;
}

static void iso9660_close(file_t handle)
{
	kfree(handle->handle);
	kfree(handle);
}

static int iso9660_mount(const char* src, vfs_node_t node)
{
	vfs_node_t device = vfs_open(src);
	if (device == 0 || device->type == file_dir) return -1;

	l9660_fs *fs = (l9660_fs *)kmalloc(sizeof(l9660_fs));
	l9660_status status = l9660_openfs(fs, read_sector, device);

	if (status != L9660_OK) return -1;
	l9660_dir *root_dir = (l9660_dir *)kmalloc(sizeof(l9660_dir));
	l9660_fs_open_root(root_dir, fs);

	file_t handle = (file_t)kmalloc(sizeof(struct file));
	handle->type = file_dir;
	handle->handle = (void *)root_dir;
	node->fsid = iso9660_id;
	iso9660_process_dir(root_dir, node);
	node->handle = handle;
	return 0;
}

static void iso9660_umount(void *root)
{
	file_t f = root;
	kfree(f->handle);
	kfree(f);
}

static int iso9660_stat(void *handle, vfs_node_t node)
{
	file_t f = handle;
	if (f->type == file_dir) {
		node->type = file_dir;
		return 0;
	} else {
		node->type = file_block;
		l9660_file *file = (l9660_file *)f->handle;
		node->size = file->length;
	}
	return 0;
}

static struct vfs_callback callbacks = {
	.mount = iso9660_mount,
	.umount = iso9660_umount,
	.open = iso9660_open,
	.close = (vfs_close_t)iso9660_close,
	.read = (vfs_read_t)iso9660_readfile,
	.write = (vfs_write_t)iso9660_writefile,
	.mkdir = iso9660_mkdir,
	.mkfile = iso9660_mkfile,
	.stat = iso9660_stat,
};

void iso9660_regist(void)
{
	iso9660_id = vfs_regist("iso9660", &callbacks);
	print_succ("iso9660(CDFS) File System initialize.\n");
}
