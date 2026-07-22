/*
 *
 *      superblock.h
 *      Superblock metadata definitions
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SUPERBLOCK_H_
#define INCLUDE_SUPERBLOCK_H_

#include <libs/std/stdint.h>
#define SUPERBLOCK_MAGIC        0x53424C4B
#define SUPERBLOCK_SECTOR       1
#define SUPERBLOCK_NAME_LENGTH  32
#define SUPERBLOCK_BLOCK_SECTOR 512

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

int superblock_read(uint8_t drive, superblock_disk_t *sb);
int superblock_write(uint8_t drive, const superblock_disk_t *sb);
int superblock_valid(const superblock_disk_t *sb);
int superblock_probe(uint8_t drive);

#endif // INCLUDE_SUPERBLOCK_H_
