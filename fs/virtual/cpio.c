/*
 *
 *      cpio.c
 *      CPIO format parsing
 *
 *      2025/11/2 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine_module.h>
#include <fs/core/vfs.h>
#include <fs/virtual/cpio.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/data/gzip.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#define CPIO_MODE_IFMT  0170000
#define CPIO_MODE_IFREG 0100000
#define CPIO_MODE_IFDIR 0040000
#define CPIO_MODE_IFLNK 0120000

/* Determine the compression type of the data */
compression_type_t get_compression_type(const void *data, size_t size)
{
    if (size < 4) return COMPRESSION_UNKNOWN;
    const unsigned char *bytes = (const unsigned char *)data;

    if (bytes[0] == 0x1F && bytes[1] == 0x8B) return COMPRESSION_GZIP;
    if (size >= 6 && bytes[0] == 0xFD && bytes[1] == 0x37 && bytes[2] == 0x7A && bytes[3] == 0x58 && bytes[4] == 0x5A && bytes[5] == 0x00)
        return COMPRESSION_XZ;
    if (bytes[0] == 0x18 && bytes[1] == 0x4D && bytes[2] == 0x22 && bytes[3] == 0x04) return COMPRESSION_LZ4;
    if (bytes[0] == 0x28 && bytes[1] == 0xB5 && bytes[2] == 0x2F && bytes[3] == 0xFD) return COMPRESSION_ZSTD;
    if (bytes[0] == 0x5D && bytes[1] == 0x00 && bytes[2] == 0x00 && bytes[3] == 0x80) return COMPRESSION_LZMA;
    if (size >= 6 && (strncmp((const char *)bytes, "070701", 6) == 0 || strncmp((const char *)bytes, "070702", 6) == 0)) return COMPRESSION_NONE;

    return COMPRESSION_UNKNOWN;
}

/* Reading values ​​from a hexadecimal string */
static int hex_digit_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return 0;
}

static size_t read_num(const char *str, size_t count)
{
    size_t val = 0;
    for (size_t i = 0; i < count; ++i) val = val * 16 + hex_digit_value(str[i]);
    return val;
}

/* Initialize the CPIO file system (parse initramfs) */
void init_cpio(void)
{
    lmodule_t *init_ramfs = get_lmodule("initramfs");
    if (!init_ramfs) return;

    if (vfs_mount(0, get_rootdir()) != 0) {
        plogk("cpio: Cannot mount tmpfs to root_dir.\n");
        return;
    }

    compression_type_t type      = get_compression_type(init_ramfs->data, init_ramfs->size);
    uint8_t           *data_d    = 0;
    size_t             data_size = init_ramfs->size;
    int                is_free   = 0;

    char *compress_type;
    switch (type) {
        case COMPRESSION_NONE :
            data_d        = init_ramfs->data;
            is_free       = 0;
            compress_type = "cpio";
            break;
        case COMPRESSION_GZIP : {
            int status = gzip_decompress(init_ramfs->data, init_ramfs->size, &data_d, &data_size);
            if (status != EOK) {
                plogk("cpio: Cannot decompress gzip initramfs, error code: %d.\n", status);
                return;
            }
            is_free       = 1;
            compress_type = "gzip";
            break;
        }
        default :
            if (init_ramfs->size >= 4) {
                plogk("cpio: Cannot load initramfs, unknown format (magic: %02x %02x %02x %02x).\n", init_ramfs->data[0], init_ramfs->data[1],
                      init_ramfs->data[2], init_ramfs->data[3]);
            } else {
                plogk("cpio: Cannot load initramfs, data is too short (%llu bytes).\n", init_ramfs->size);
            }
            return;
    }

    if (get_compression_type(data_d, data_size) != COMPRESSION_NONE) {
        plogk("cpio: Decompressed initramfs is not a newc archive.\n");
        if (is_free) free(data_d);
        return;
    }

    cpio_newc_header_t hdr;
    size_t             offset       = 0;
    size_t             file_num_all = 0;

    while (1) {
        if (offset + sizeof(hdr) > data_size) break;
        memcpy(&hdr, data_d + offset, sizeof(hdr));
        offset += sizeof(hdr);

        size_t namesize = read_num(hdr.c_namesize, 8);
        if (namesize > 4096 || offset + namesize > data_size) break;
        char filename[4096];
        filename[0]    = '/';
        size_t copy_ns = namesize < sizeof(filename) - 2 ? namesize : sizeof(filename) - 2;
        memcpy(filename + 1, data_d + offset, copy_ns);
        filename[copy_ns + 1] = '\0';
        offset                = (offset + namesize + 3) & ~3;

        size_t filesize = read_num(hdr.c_filesize, 8);
        if (filesize > data_size || offset + filesize > data_size) break;
        char *filedata = malloc(filesize ? filesize : 1);
        if (!filedata) break;
        memcpy(filedata, data_d + offset, filesize);
        offset = (offset + filesize + 3) & ~3;

        if (strcmp(filename, "/TRAILER!!!") == 0) {
            free(filedata);
            break;
        }
        if (strcmp(filename, "/.") == 0) {
            free(filedata);
            continue;
        }

        file_num_all++;
        size_t mode      = read_num(hdr.c_mode, 8);
        size_t file_type = mode & CPIO_MODE_IFMT;
        int    status;

        if (file_type == CPIO_MODE_IFDIR) {
            status = vfs_mkdir(filename);
            if (status != EOK) {
                plogk("cpio: Cannot build initramfs directory(%s), error code: %d\n", filename, status);
                free(filedata);
                if (is_free) free(data_d);
                return;
            }
        } else if (file_type == CPIO_MODE_IFLNK) {
            char *symlink_path = calloc(1, filesize + 1);

            if (!symlink_path) {
                free(filedata);
                if (is_free) free(data_d);
                return;
            }

            strncpy(symlink_path, filedata, filesize);
            status = vfs_symlink(filename, symlink_path);
            free(symlink_path);

            if (status != EOK) {
                plogk("cpio: Cannot build initramfs symlink(%s), error code: %d\n", filename, status);
                free(filedata);
                if (is_free) free(data_d);
                return;
            }
        } else if (file_type == CPIO_MODE_IFREG) {
            status = vfs_mkfile(filename);
            if (status != EOK) {
                plogk("cpio: Cannot build initramfs file(%s), error code: %d\n", filename, status);
                free(filedata);
                if (is_free) free(data_d);
                return;
            }

            vfs_node_t file = vfs_open(filename);
            if (!file) {
                plogk("cpio: Cannot build initramfs, open error(%s)\n", filename);
                free(filedata);
                if (is_free) free(data_d);
                return;
            }

            status = (int)vfs_write(file, filedata, 0, filesize);
            if (status == -1) {
                plogk("cpio: Cannot build initramfs, write error(%s): %d\n", filename, status);
                free(filedata);
                if (is_free) free(data_d);
                return;
            }
            vfs_close(file);
        } else {
            plogk("cpio: Skip unsupported initramfs entry(%s), mode: %llu\n", filename, mode);
        }
        free(filedata);
    }
    if (is_free) free(data_d);
    plogk("cpio: Loaded initramfs size: %llu, files: %llu, compress: %s\n", data_size, file_num_all, compress_type);
}
