/*
 *
 *      cpio.c
 *      Validated newc initramfs loader
 *
 *      2025/11/2 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine_module.h>
#include <fs/core/vfs.h>
#include <fs/cpio/cpio.h>
#include <fs/tmpfs/tmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <libs/util/gzip.h>
#include <mem/heap.h>

#define CPIO_MODE_IFMT   0170000
#define CPIO_MODE_IFREG  0100000
#define CPIO_MODE_IFDIR  0040000
#define CPIO_MODE_IFLNK  0120000
#define CPIO_NAME_MAX    4096
#define CPIO_HEADER_SIZE 110

_Static_assert(sizeof(cpio_newc_header_t) == CPIO_HEADER_SIZE, "newc header layout mismatch");

/* Detect the compression format from the archive magic bytes. */
static compression_type_t get_compression_type(const void *data, size_t size)
{
    if (!data || size < 4) return COMPRESSION_UNKNOWN;
    const unsigned char *bytes = data;

    if (bytes[0] == 0x1F && bytes[1] == 0x8B) return COMPRESSION_GZIP;
    if (size >= 6 && bytes[0] == 0xFD && bytes[1] == 0x37 && bytes[2] == 0x7A && bytes[3] == 0x58 && bytes[4] == 0x5A && bytes[5] == 0x00)
        return COMPRESSION_XZ;
    if (bytes[0] == 0x18 && bytes[1] == 0x4D && bytes[2] == 0x22 && bytes[3] == 0x04) return COMPRESSION_LZ4;
    if (bytes[0] == 0x28 && bytes[1] == 0xB5 && bytes[2] == 0x2F && bytes[3] == 0xFD) return COMPRESSION_ZSTD;
    if (bytes[0] == 0x5D && bytes[1] == 0x00 && bytes[2] == 0x00 && bytes[3] == 0x80) return COMPRESSION_LZMA;
    if (size >= 6 && (!memcmp(bytes, "070701", 6) || !memcmp(bytes, "070702", 6))) return COMPRESSION_NONE;
    return COMPRESSION_UNKNOWN;
}

/* Parse a fixed-width hexadecimal field from a newc header. */
static bool cpio_read_hex(const char *text, size_t count, uint32_t *result)
{
    uint32_t value = 0;

    if (!text || !result || count > 8) return false;
    for (size_t i = 0; i < count; i++) {
        unsigned digit;
        if (text[i] >= '0' && text[i] <= '9')
            digit = (unsigned)(text[i] - '0');
        else if (text[i] >= 'a' && text[i] <= 'f')
            digit = (unsigned)(text[i] - 'a' + 10);
        else if (text[i] >= 'A' && text[i] <= 'F')
            digit = (unsigned)(text[i] - 'A' + 10);
        else
            return false;
        value = (value << 4) | digit;
    }
    *result = value;
    return true;
}

/* Advance an archive offset by amount, keeping it 4-byte aligned. */
static bool cpio_advance_aligned(size_t *offset, size_t amount, size_t limit)
{
    size_t next;

    if (!offset || *offset > limit || amount > limit - *offset) return false;
    next = *offset + amount;
    if (next > SIZE_MAX - 3) return false;
    next = (next + 3) & ~(size_t)3;
    if (next > limit) return false;
    *offset = next;
    return true;
}

/* Reject absolute paths, empty components and traversal before touching VFS. */
static bool cpio_make_path(const char *archive_name, size_t namesize, char path[CPIO_NAME_MAX + 2])
{
    const char *name = archive_name;
    size_t      length;

    if (!archive_name || namesize < 2 || namesize > CPIO_NAME_MAX || archive_name[namesize - 1] != '\0') return false;
    length = namesize - 1;
    if (memchr(archive_name, '\0', length)) return false;
    while (length >= 2 && name[0] == '.' && name[1] == '/') {
        name += 2;
        length -= 2;
    }
    if (!length || name[0] == '/' || length > CPIO_NAME_MAX) return false;
    if (length == 1 && name[0] == '.') {
        memcpy(path, "/.", 3);
        return true;
    }

    size_t component_start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i != length && name[i] != '/') continue;
        size_t component_length = i - component_start;
        if (!component_length || (component_length == 1 && name[component_start] == '.')
            || (component_length == 2 && name[component_start] == '.' && name[component_start + 1] == '.')) {
            return false;
        }
        component_start = i + 1;
    }

    path[0] = '/';
    memcpy(path + 1, name, length);
    path[length + 1] = '\0';
    return true;
}

/* Create every intermediate directory of a path. */
static int cpio_ensure_parents(char *path)
{
    for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash     = '\0';
        int status = vfs_mkdir_mode(path, 0755);
        *slash     = '/';
        if (status != EOK && status != -EEXIST) return status;
    }
    return EOK;
}

/* Apply archive mode, ownership and timestamp to a VFS node. */
static void cpio_set_metadata(vfs_node_t node, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t mtime)
{
    node->mode = node->permissions = (uint16_t)(mode & 07777);
    node->owner                    = uid;
    node->group                    = gid;
    node->createtime = node->readtime = node->writetime = mtime;
}

/* Compute the newc CRC variant checksum over the file data. */
static uint32_t cpio_data_checksum(const uint8_t *data, size_t size)
{
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i++) checksum += data[i];
    return checksum;
}

/* Install one archive entry (dir, symlink or regular file) into the VFS. */
static int cpio_install_entry(char *path, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t mtime, const uint8_t *filedata, size_t filesize,
                              bool zero_copy)
{
    uint32_t file_type = mode & CPIO_MODE_IFMT;
    int      status;

    status = cpio_ensure_parents(path);
    if (status != EOK) return status;

    if (file_type == CPIO_MODE_IFDIR) {
        status = vfs_mkdir_mode(path, (uint16_t)(mode & 07777));
        if (status != EOK && status != -EEXIST) return status;
        vfs_node_t node = vfs_open(path);
        if (!node) return -ENOENT;
        if (!(node->type & file_dir)) {
            vfs_close(node);
            return -ENOTDIR;
        }
        cpio_set_metadata(node, mode, uid, gid, mtime);
        vfs_close(node);
        return EOK;
    }

    if (file_type == CPIO_MODE_IFLNK) {
        if (memchr(filedata, '\0', filesize)) return -EINVAL;
        char *target = malloc(filesize + 1);
        if (!target) return -ENOMEM;
        memcpy(target, filedata, filesize);
        target[filesize] = '\0';
        status           = vfs_symlink(path, target);
        free(target);
        return status == -EEXIST ? EOK : status;
    }

    if (file_type != CPIO_MODE_IFREG) return -EOPNOTSUPP;
    status = vfs_mkfile_mode(path, (uint16_t)(mode & 07777));
    if (status != EOK && status != -EEXIST) return status;

    vfs_node_t node = vfs_open(path);
    if (!node) return -ENOENT;
    if ((node->type & (file_dir | file_symlink | file_block)) || !(node->handle)) {
        vfs_close(node);
        return -EINVAL;
    }
    cpio_set_metadata(node, mode, uid, gid, mtime);
    if (zero_copy) {
        status = tmpfs_adopt_file_data(node, filedata, filesize);
    } else {
        status = tmpfs_resize(node->handle, 0);
        if (status == EOK && filesize) {
            int64_t written = vfs_write(node, filedata, 0, filesize);
            status          = written < 0 ? (int)written : (size_t)written == filesize ? EOK : -EIO;
        }
    }
    vfs_close(node);
    return status;
}

/* Extract the initramfs module into the VFS as the root filesystem. */
void init_cpio(void)
{
    lmodule_t *module = get_lmodule("initramfs");
    if (!module) return;

    vfs_node_t root = get_rootdir();
    if (!root->is_mount && vfs_mount(0, root) != EOK) {
        plogk("cpio: Cannot mount tmpfs on the root directory.\n");
        return;
    }

    compression_type_t type      = get_compression_type(module->data, module->size);
    uint8_t           *archive   = module->data;
    size_t             size      = module->size;
    bool               allocated = false;
    const char        *format    = "cpio";

    if (type == COMPRESSION_GZIP) {
        int status = gzip_decompress(module->data, module->size, &archive, &size);
        if (status != EOK) {
            plogk("cpio: Gzip decompression failed: %d\n", status);
            return;
        }
        allocated = true;
        format    = "gzip";
    } else if (type != COMPRESSION_NONE) {
        plogk("cpio: Unsupported or invalid initramfs format.\n");
        return;
    }
    if (get_compression_type(archive, size) != COMPRESSION_NONE) {
        plogk("cpio: Decompressed data is not a newc archive.\n");
        if (allocated) free(archive);
        return;
    }

    size_t offset  = 0;
    size_t entries = 0;
    bool   trailer = false;
    int    failure = EOK;

    while (offset < size) {
        cpio_newc_header_t header;
        uint32_t           namesize, filesize, mode, uid, gid, mtime, expected_checksum;
        char               path[CPIO_NAME_MAX + 2];

        if (size - offset < sizeof(header)) {
            failure = -EBADMSG;
            break;
        }
        memcpy(&header, archive + offset, sizeof(header));
        offset += sizeof(header);

        bool crc = !memcmp(header.c_magic, "070702", 6);
        if (!crc && memcmp(header.c_magic, "070701", 6) != 0) {
            failure = -EBADMSG;
            break;
        }
        if (!cpio_read_hex(header.c_namesize, 8, &namesize) || !cpio_read_hex(header.c_filesize, 8, &filesize)
            || !cpio_read_hex(header.c_mode, 8, &mode) || !cpio_read_hex(header.c_uid, 8, &uid) || !cpio_read_hex(header.c_gid, 8, &gid)
            || !cpio_read_hex(header.c_mtime, 8, &mtime) || !cpio_read_hex(header.c_check, 8, &expected_checksum)) {
            failure = -EBADMSG;
            break;
        }
        if (!namesize || namesize > CPIO_NAME_MAX || namesize > size - offset) {
            failure = -EBADMSG;
            break;
        }
        const char *archive_name = (const char *)archive + offset;
        if (!cpio_make_path(archive_name, namesize, path)) {
            failure = -EINVAL;
            break;
        }
        if (!cpio_advance_aligned(&offset, namesize, size) || filesize > size - offset) {
            failure = -EBADMSG;
            break;
        }
        const uint8_t *filedata = archive + offset;
        if (crc && cpio_data_checksum(filedata, filesize) != expected_checksum) {
            failure = -EBADMSG;
            break;
        }
        if (!cpio_advance_aligned(&offset, filesize, size)) {
            failure = -EBADMSG;
            break;
        }

        if (!strcmp(path, "/TRAILER!!!")) {
            trailer = true;
            break;
        }
        if (!strcmp(path, "/.")) continue;

        /*
         * devtmpfs is mounted before the initramfs is unpacked and owns the
         * complete /dev namespace.  Alpine's minirootfs contains a regular
         * zero-length /dev/null placeholder; attempting to replace the live
         * character device with that file makes tmpfs_adopt_file_data()
         * reject the device handle with -EINVAL.
         */
        if (!strcmp(path, "/dev") || !strncmp(path, "/dev/", 5)) continue;

        int status = cpio_install_entry(path, mode, uid, gid, mtime, filedata, filesize, !allocated);
        if (status == -EOPNOTSUPP) {
            plogk("cpio: Skipping unsupported entry %s (mode %o)\n", path, mode);
            continue;
        }
        if (status != EOK) {
            plogk("cpio: Cannot install %s: %d\n", path, status);
            failure = status;
            break;
        }
        entries++;
    }

    if (allocated) free(archive);
    if (failure != EOK || !trailer) {
        plogk("cpio: Initramfs rejected after %llu entries: %d%s\n", entries, failure, trailer ? "" : " (missing trailer)");
        return;
    }
    plogk("cpio: Loaded initramfs: %llu bytes, %llu entries, format=%s, storage=%s\n", size, entries, format,
          allocated ? "copied" : "module-backed COW");
}
