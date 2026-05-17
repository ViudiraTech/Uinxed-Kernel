/*
 *
 *      simplefs.h
 *      Minimal IDE-backed filesystem using superblock metadata
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SIMPLEFS_H_
#define INCLUDE_SIMPLEFS_H_

#include <blockdev.h>
#include <stdint.h>
#include <superblock.h>

#define SIMPLEFS_INODE_DIRECT_COUNT 12
#define SIMPLEFS_DIRENT_NAME_LENGTH 56

enum {
    simplefs_inode_none    = 0,
    simplefs_inode_file    = 1,
    simplefs_inode_dir     = 2,
    simplefs_inode_symlink = 3,
};

typedef struct simplefs_handle {
        blockdev_device_t device;
        superblock_disk_t disk;
} simplefs_handle_t;

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

int  simplefs_probe(uint8_t drive);
void simplefs_regist(void);
void simplefs_mount_all(void);

#endif // INCLUDE_SIMPLEFS_H_
