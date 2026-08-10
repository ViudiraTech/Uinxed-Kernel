/*
 *
 *      mkfs_simplefs.c
 *      Host-side image builder for simplefs
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define SUPERBLOCK_MAGIC        0x53424C4B
#define SUPERBLOCK_SECTOR       1
#define SUPERBLOCK_NAME_LENGTH  32
#define SUPERBLOCK_BLOCK_SECTOR 512

#define SIMPLEFS_INODE_DIRECT_COUNT 12
#define SIMPLEFS_DIRENT_NAME_LENGTH 56

enum {
    simplefs_inode_none    = 0,
    simplefs_inode_file    = 1,
    simplefs_inode_dir     = 2,
    simplefs_inode_symlink = 3,
};

typedef struct superblock_disk {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t inode_count;
    uint32_t block_count;
    uint32_t inode_table_start;
    uint32_t data_block_start;
    uint32_t root_inode;
    uint32_t features;
    char     volume_name[SUPERBLOCK_NAME_LENGTH];
} __attribute__((packed)) superblock_disk_t;

typedef struct simplefs_inode_disk {
    uint32_t inode;
    uint16_t type;
    uint16_t links;
    uint32_t permissions;
    uint32_t owner;
    uint32_t group;
    uint64_t size;
    uint64_t create_time;
    uint64_t write_time;
    uint32_t direct[SIMPLEFS_INODE_DIRECT_COUNT];
    uint32_t indirect;
} __attribute__((packed)) simplefs_inode_disk_t;

typedef struct simplefs_dirent_disk {
    uint32_t inode;
    uint16_t type;
    uint16_t name_length;
    char     name[SIMPLEFS_DIRENT_NAME_LENGTH];
} __attribute__((packed)) simplefs_dirent_disk_t;

#define SIMPLEFS_DEFAULT_IMAGE_MB 64
#define SIMPLEFS_DEFAULT_BLOCKS   16

static int write_zeros(FILE *fp, size_t size)
{
    unsigned char zero[4096] = {0};

    while (size) {
        size_t chunk = size > sizeof(zero) ? sizeof(zero) : size;
        if (fwrite(zero, 1, chunk, fp) != chunk) return -1;
        size -= chunk;
    }
    return 0;
}

static int write_at(FILE *fp, long offset, const void *buf, size_t size)
{
    if (fseek(fp, offset, SEEK_SET) != 0) return -1;
    return fwrite(buf, 1, size, fp) == size ? 0 : -1;
}

static int parse_size_mb(const char *str, uint32_t *size_mb)
{
    char *end;
    unsigned long value;

    if (!str || !size_mb) return -1;

    errno = 0;
    value = strtoul(str, &end, 10);
    if (errno || !end || *end != '\0' || value == 0 || value > 4096) return -1;

    *size_mb = (uint32_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    const char             *image_path;
    uint32_t                image_mb = SIMPLEFS_DEFAULT_IMAGE_MB;
    uint32_t                block_size = 4096;
    uint32_t                image_size;
    uint32_t                block_count;
    FILE                   *fp;
    superblock_disk_t       sb;
    simplefs_inode_disk_t   inodes[SIMPLEFS_DEFAULT_BLOCKS] = {0};
    simplefs_dirent_disk_t  dirents[1]                      = {0};
    unsigned char          *root_block;
    unsigned char          *file_block;
    const char              hello[] = "simplefs is mounted from ide0\n";

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <image> [size_mb]\n", argv[0]);
        return 1;
    }

    image_path = argv[1];
    if (argc == 3 && parse_size_mb(argv[2], &image_mb) != 0) {
        fprintf(stderr, "mkfs_simplefs: invalid image size '%s'\n", argv[2]);
        return 1;
    }

    image_size  = image_mb * 1024 * 1024;
    block_count = image_size / block_size;
    if (block_count < 4) {
        fprintf(stderr, "mkfs_simplefs: image is too small\n");
        return 1;
    }

    fp = fopen(image_path, "wb+");
    if (!fp) {
        fprintf(stderr, "mkfs_simplefs: cannot open %s\n", image_path);
        return 1;
    }

    if (write_zeros(fp, image_size) != 0) {
        fprintf(stderr, "mkfs_simplefs: cannot initialize image\n");
        fclose(fp);
        return 1;
    }

    memset(&sb, 0, sizeof(sb));
    sb.magic             = SUPERBLOCK_MAGIC;
    sb.version           = 1;
    sb.block_size        = block_size;
    sb.inode_size        = sizeof(simplefs_inode_disk_t);
    sb.inode_count       = SIMPLEFS_DEFAULT_BLOCKS;
    sb.block_count       = block_count;
    sb.inode_table_start = 1;
    sb.data_block_start  = 2;
    sb.root_inode        = 1;
    sb.features          = 0;
    strncpy(sb.volume_name, "simplefs-demo", sizeof(sb.volume_name));

    inodes[0].inode       = 1;
    inodes[0].type        = simplefs_inode_dir;
    inodes[0].links       = 1;
    inodes[0].permissions = 0755;
    inodes[0].size        = sizeof(dirents);
    inodes[0].direct[0]   = sb.data_block_start;

    inodes[1].inode       = 2;
    inodes[1].type        = simplefs_inode_file;
    inodes[1].links       = 1;
    inodes[1].permissions = 0644;
    inodes[1].size        = sizeof(hello) - 1;
    inodes[1].direct[0]   = sb.data_block_start + 1;

    dirents[0].inode       = 2;
    dirents[0].type        = simplefs_inode_file;
    dirents[0].name_length = 9;
    memcpy(dirents[0].name, "hello.txt", 9);

    root_block = calloc(1, block_size);
    file_block = calloc(1, block_size);
    if (!root_block || !file_block) {
        fprintf(stderr, "mkfs_simplefs: out of memory\n");
        free(root_block);
        free(file_block);
        fclose(fp);
        return 1;
    }

    memcpy(root_block, dirents, sizeof(dirents));
    memcpy(file_block, hello, sizeof(hello) - 1);

    if (write_at(fp, SUPERBLOCK_SECTOR * SUPERBLOCK_BLOCK_SECTOR, &sb, sizeof(sb)) != 0 ||
        write_at(fp, (long)sb.inode_table_start * block_size, inodes, sizeof(inodes)) != 0 ||
        write_at(fp, (long)sb.data_block_start * block_size, root_block, block_size) != 0 ||
        write_at(fp, (long)(sb.data_block_start + 1) * block_size, file_block, block_size) != 0) {
        fprintf(stderr, "mkfs_simplefs: write failed\n");
        free(root_block);
        free(file_block);
        fclose(fp);
        return 1;
    }

    free(root_block);
    free(file_block);

    if (fclose(fp) != 0) {
        fprintf(stderr, "mkfs_simplefs: close failed\n");
        return 1;
    }

    printf("simplefs image created: %s (%u MiB)\n", image_path, image_mb);
    printf("volume=%s root_inode=%u file=/hello.txt\n", sb.volume_name, sb.root_inode);
    return 0;
}
