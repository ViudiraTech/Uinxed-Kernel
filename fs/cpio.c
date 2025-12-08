/*
 *
 *      cpio.c
 *      CPIO format parsing
 *
 *      2025/11/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cpio.h>
#include <errno.h>
#include <heap.h>
#include <limine_module.h>
#include <printk.h>
#include <stdlib.h>
#include <string.h>
#include <vfs.h>

/* Get the parent directory path of the path */
static char *get_pdir_fpath(char *path)
{
    if (!path) return 0;
    if (!strcmp(path, "/")) return strdup("/");

    size_t len        = strlen(path);
    char  *last_slash = strrchr(path, '/');

    if (!last_slash) return strdup(".");
    if (last_slash == path) return strdup("/");

    size_t new_len = last_slash - path;
    if (path[len - 1] == '/') {
        char *prev_last_slash = (char *)last_slash - 1;
        while (prev_last_slash > path && *prev_last_slash == '/') prev_last_slash--;
        while (prev_last_slash > path && *prev_last_slash != '/') prev_last_slash--;

        if (*prev_last_slash == '/') {
            new_len = prev_last_slash - path;
        } else {
            new_len = 0;
        }
    }

    char *new_path = (char *)malloc(new_len + 1);
    if (!new_path) return 0;

    strncpy(new_path, path, new_len);
    new_path[new_len] = '\0';

    if (!new_len && path[0] == '/') {
        free(new_path);
        return strdup("/");
    }
    return new_path;
}

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
    if (strncmp((const char *)bytes, "070701", 6) == 0 || strncmp((const char *)bytes, "070707", 6) == 0) return COMPRESSION_NONE;

    return COMPRESSION_UNKNOWN;
}

/* Reading values ​​from a hexadecimal string */
static size_t read_num(const char *str, size_t count)
{
    size_t val = 0;
    for (size_t i = 0; i < count; ++i) val = val * 16 + (IS_DIGIT(str[i]) ? str[i] - '0' : str[i] - 'A' + 10);
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

    compression_type_t type    = get_compression_type(init_ramfs->data, init_ramfs->size);
    uint8_t           *data_d  = 0;
    int                is_free = 0;

    char *compress_type;
    switch (type) {
        case COMPRESSION_NONE :
            data_d        = init_ramfs->data;
            is_free       = 0;
            compress_type = "cpio";
            break;
        default :
            plogk("cpio: Cannot load initramfs, unknown format.\n");
            return;
    }

    cpio_newc_header_t hdr;
    size_t             offset       = 0;
    size_t             file_num_all = 0;

    while (1) {
        memcpy(&hdr, data_d + offset, sizeof(hdr));
        offset += sizeof(hdr);

        size_t namesize = read_num(hdr.c_namesize, 8);
        char   filename[namesize + 1];
        filename[0] = '/';
        memcpy(filename + 1, data_d + offset, namesize);
        offset = (offset + namesize + 3) & ~3;

        size_t filesize = read_num(hdr.c_filesize, 8);
        char  *filedata = malloc(filesize);
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
        size_t mode = read_num(hdr.c_mode, 8);
        int    status;

        if (mode & 040000) {
            status = vfs_mkdir(filename);
            if (status != EOK) {
                plogk("cpio: Cannot build initramfs directory(%s), error code: %d\n", filename, status);
                free(filedata);
                return;
            }
        } else if ((mode & 0120000) == 0120000) {
            const size_t len          = strlen(filename) + filesize;
            char        *all_path     = malloc(len);
            char        *dirname      = get_pdir_fpath(filename);
            char        *symlink_path = calloc(1, filesize + 1);

            strncpy(symlink_path, filedata, filesize);
            (void)sprintf(all_path, "%s/%s", dirname, symlink_path);

            char *target_name = normalize_path(all_path);
            status            = vfs_symlink(filename, target_name);

            free(all_path);
            free(target_name);
            free(dirname);
            free(symlink_path);

            if (status != EOK) {
                plogk("cpio: Cannot build initramfs symlink(%s), error code: %d\n", filename, status);
                free(filedata);
                return;
            }
        } else {
            status = vfs_mkfile(filename);
            if (status != EOK) {
                plogk("cpio: Cannot build initramfs file(%s), error code: %d\n", filename, status);
                free(filedata);
                return;
            }

            vfs_node_t file = vfs_open(filename);
            if (!file) {
                plogk("cpio: Cannot build initramfs, open error(%s)\n", filename);
                free(filedata);
                return;
            }

            status = (int)vfs_write(file, filedata, 0, filesize);
            if (status == -1) {
                plogk("cpio: Cannot build initramfs, write error(%s): %d\n", filename, status);
                free(filedata);
                return;
            }
            vfs_close(file);
        }
        free(filedata);
    }
    if (is_free) free(data_d);
    plogk("cpio: Loaded initramfs size: %llu, files: %llu, compress: %s\n", init_ramfs->size, file_num_all, compress_type);
}
