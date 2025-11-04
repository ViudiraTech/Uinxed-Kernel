/*
 *
 *      cpio.h
 *      CPIO format parsing header file
 *
 *      2025/11/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CPIO_H_
#define INCLUDE_CPIO_H_

typedef enum {
    COMPRESSION_NONE,
    COMPRESSION_GZIP,
    COMPRESSION_LZMA,
    COMPRESSION_LZ4,
    COMPRESSION_ZSTD,
    COMPRESSION_XZ,
    COMPRESSION_UNKNOWN
} compression_type_t;

typedef struct {
        char c_magic[6];
        char c_ino[8];
        char c_mode[8];
        char c_uid[8];
        char c_gid[8];
        char c_nlink[8];
        char c_mtime[8];
        char c_filesize[8];
        char c_devmajor[8];
        char c_devminor[8];
        char c_rdevmajor[8];
        char c_rdevminor[8];
        char c_namesize[8];
        char c_check[8];
} cpio_newc_header_t;

/* Initialize the CPIO file system (parse initramfs) */
void init_cpio(void);

#endif // INCLUDE_CPIO_H_
