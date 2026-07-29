/*
 *
 *      extfs_image_test.c
 *      Host interoperability test against e2fsprogs-created images
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INCLUDE_STDINT_H_
#define INCLUDE_STDDEF_H_
#include <fs/extfs/extfs.h>
#include <kernel/errno.h>

typedef struct image_backend {
        uint8_t *data;
        size_t   size;
} image_backend_t;

uint32_t timer_realtime_seconds32(void) { return 1774780800U; }

void spin_lock(spinlock_t *lock) { assert(!lock->lock); lock->lock = 1; }
void spin_unlock(spinlock_t *lock) { assert(lock->lock); lock->lock = 0; }
void sched_yield(void) {}
void blockdev_retain(const blockdev_device_t *device) { (void)device; }
void blockdev_release(const blockdev_device_t *device) { (void)device; }
int blockdev_flush(const blockdev_device_t *device) { (void)device; return EOK; }

int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size)
{
    image_backend_t *image = device->backend_data;
    if (offset + size > image->size) return -EIO;
    memcpy(buffer, image->data + offset, size);
    return EOK;
}

int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size)
{
    image_backend_t *image = device->backend_data;
    if (device->read_only) return -EROFS;
    if (offset + size > image->size) return -EIO;
    memcpy(image->data + offset, buffer, size);
    return EOK;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return 2;
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    image_backend_t image = {.data = malloc((size_t)length), .size = (size_t)length};
    assert(image.data && fread(image.data, 1, image.size, file) == image.size);
    fclose(file);
    blockdev_device_t device = {
        .backend_data = &image,
        .sector_size = 512,
        .sector_count = image.size / 512,
        .read_only = argc == 2,
    };
    extfs_sb_info_t sb;
    int status = extfs_read_super(&sb, &device);
    if (status != EOK) fprintf(stderr, "extfs_read_super failed: %d\n", status);
    assert(status == EOK);
    extfs_handle_t *root = extfs_alloc_handle(&sb, EXT2_ROOT_INO);
    assert(root);
    uint8_t invalid_buffer[8];
    uint32_t invalid_ino;
    assert(extfs_dir_lookup(NULL, "invalid", &invalid_ino) == -EINVAL);
    assert(extfs_dir_read_entries(root, invalid_buffer, sizeof(invalid_buffer), NULL) == -EINVAL);
    assert(extfs_alloc_block(NULL, 0, &invalid_ino) == -EINVAL);
    assert(extfs_truncate(root, (uint64_t)UINT32_MAX * sb.block_size + 1) == -EFBIG);
    uint32_t ino;
    assert(extfs_dir_lookup(root, "lost+found", &ino) == EOK && ino >= sb.s_first_ino);
    if (argc == 3) {
        extfs_handle_t *parent = root;
        uint32_t many_ino;
        if (extfs_dir_lookup(root, "many", &many_ino) == EOK) {
            parent = extfs_alloc_handle(&sb, many_ino);
            assert(parent);
        }
        fs_txn_t transaction;
        assert(extfs_transaction_begin(&sb, &transaction, 8192) == EOK);

        /* Force ext4 lazy block-bitmap initialization outside block group zero. */
        for (uint32_t group = 1; group < sb.groups_count; group++) {
            if (!(sb.group_desc[group].bg_flags & EXT4_BG_BLOCK_UNINIT)) continue;
            uint32_t block;
            uint32_t goal = sb.s_first_data_block + group * sb.blocks_per_group;
            assert(extfs_alloc_block(&sb, goal, &block) == EOK);
            assert(block >= goal && block < goal + sb.blocks_per_group);
            extfs_free_block(&sb, block);
            assert(!(sb.group_desc[group].bg_flags & EXT4_BG_BLOCK_UNINIT));
            break;
        }

        if (sb.groups_count > 1 && (sb.group_desc[1].bg_flags & EXT4_BG_INODE_UNINIT)
            && sb.group_desc[0].bg_free_inodes_count < 128) {
            uint32_t temporary[128];
            uint32_t count = sb.group_desc[0].bg_free_inodes_count + 1;
            assert(count <= sizeof(temporary) / sizeof(temporary[0]));
            for (uint32_t i = 0; i < count; i++) assert(extfs_alloc_inode(&sb, &temporary[i]) == EOK);
            assert((temporary[count - 1] - 1) / sb.inodes_per_group == 1);
            assert(!(sb.group_desc[1].bg_flags & EXT4_BG_INODE_UNINIT));
            for (uint32_t i = 0; i < count; i++) extfs_free_inode(&sb, temporary[i]);
        }

        assert(extfs_alloc_inode(&sb, &ino) == EOK);
        ext2_inode_t raw;
        memset(&raw, 0, sizeof(raw));
        raw.i_mode = EXT2_S_IFREG | 0644;
        raw.i_links_count = 1;
        if (sb.es->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
            raw.i_flags = EXT4_EXTENTS_FL;
            uint8_t extent_header[12] = {0x0a, 0xf3, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
            memcpy(raw.i_block, extent_header, sizeof(extent_header));
        }
        assert(extfs_write_inode_raw(&sb, ino, &raw) == EOK);
        extfs_handle_t *file_handle = extfs_alloc_handle(&sb, ino);
        assert(file_handle);
        static const char payload[] = "native-c-ext4\n";
        assert(extfs_write_data(file_handle, payload, 0, sizeof(payload) - 1) == (int)sizeof(payload) - 1);
        if (sb.es->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
            for (uint32_t logical = 2; logical <= 8; logical += 2)
                assert(extfs_write_data(file_handle, payload, (uint64_t)logical * sb.block_size,
                                        sizeof(payload) - 1) == (int)sizeof(payload) - 1);
        }
        char readback[sizeof(payload)] = {0};
        assert(extfs_read_data(file_handle, readback, 0, sizeof(payload) - 1) == (int)sizeof(payload) - 1);
        assert(memcmp(readback, payload, sizeof(payload) - 1) == 0);
        if (sb.es->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
            memset(readback, 0, sizeof(readback));
            assert(extfs_read_data(file_handle, readback, 2ULL * sb.block_size, sizeof(payload) - 1)
                   == (int)sizeof(payload) - 1);
            assert(memcmp(readback, payload, sizeof(payload) - 1) == 0);
            memset(readback, 0x5a, sizeof(readback));
            assert(extfs_read_data(file_handle, readback, sb.block_size, sizeof(payload) - 1)
                   == (int)sizeof(payload) - 1);
            for (size_t i = 0; i < sizeof(payload) - 1; i++) assert(readback[i] == 0);
        }
        assert(extfs_dir_add_entry(parent, "native-c.txt", ino, EXT2_FT_REG_FILE) == EOK);

        uint32_t directory_ino;
        assert(extfs_alloc_inode(&sb, &directory_ino) == EOK);
        ext2_inode_t directory_raw;
        memset(&directory_raw, 0, sizeof(directory_raw));
        directory_raw.i_mode = EXT2_S_IFDIR | 0755;
        directory_raw.i_links_count = 2;
        if (sb.es->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
            directory_raw.i_flags = EXT4_EXTENTS_FL;
            uint8_t extent_header[12] = {0x0a, 0xf3, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
            memcpy(directory_raw.i_block, extent_header, sizeof(extent_header));
        }
        assert(extfs_write_inode_raw(&sb, directory_ino, &directory_raw) == EOK);
        extfs_handle_t *directory = extfs_alloc_handle(&sb, directory_ino);
        assert(directory);
        assert(extfs_make_empty_dir(directory, directory_ino, EXT2_ROOT_INO) == EOK);
        assert(extfs_dir_add_entry(root, "native-dir", directory_ino, EXT2_FT_DIR) == EOK);
        assert(extfs_adjust_used_dirs(&sb, directory_ino, 1) == EOK);
        ext2_inode_t root_raw;
        assert(extfs_read_inode_raw(&sb, EXT2_ROOT_INO, &root_raw) == EOK);
        root_raw.i_links_count++;
        assert(extfs_write_inode_raw(&sb, EXT2_ROOT_INO, &root_raw) == EOK);
        assert(extfs_transaction_commit(&sb, &transaction) == EOK);

        assert(extfs_transaction_begin(&sb, &transaction, 8192) == EOK);
        uint32_t removed_directories = 1;
        assert(extfs_dir_remove_entry(root, "native-dir") == EOK);
        extfs_free_inode_blocks(directory);
        assert(extfs_read_inode_raw(&sb, directory_ino, &directory_raw) == EOK);
        memset(directory_raw.i_block, 0, sizeof(directory_raw.i_block));
        directory_raw.i_links_count = 0;
        directory_raw.i_dtime = timer_realtime_seconds32();
        directory_raw.i_size = 0;
        directory_raw.i_blocks = 0;
        assert(extfs_write_inode_raw(&sb, directory_ino, &directory_raw) == EOK);
        extfs_free_inode(&sb, directory_ino);
        assert(extfs_adjust_used_dirs(&sb, directory_ino, -1) == EOK);

        /* Exercise external EA-block checksum/refcount/free semantics when supplied by the image fixture. */
        uint32_t xattr_ino;
        if (extfs_dir_lookup(root, "xattr-victim", &xattr_ino) == EOK) {
            ext2_inode_t xattr_raw;
            assert(extfs_read_inode_raw(&sb, xattr_ino, &xattr_raw) == EOK);
            assert((xattr_raw.i_mode & 0xF000) == EXT2_S_IFDIR);
            assert(xattr_raw.i_file_acl || xattr_raw.l_i_file_acl_high);
            extfs_handle_t *xattr_handle = extfs_alloc_handle(&sb, xattr_ino);
            assert(xattr_handle && extfs_dir_empty(xattr_handle) == 1);
            assert(extfs_dir_remove_entry(root, "xattr-victim") == EOK);
            extfs_free_inode_blocks(xattr_handle);
            assert(extfs_release_xattr_block(xattr_handle) == EOK);
            memset(xattr_raw.i_block, 0, sizeof(xattr_raw.i_block));
            xattr_raw.i_links_count = 0;
            xattr_raw.i_size = 0;
            xattr_raw.i_blocks = 0;
            xattr_raw.i_file_acl = 0;
            xattr_raw.l_i_file_acl_high = 0;
            xattr_raw.i_dtime = timer_realtime_seconds32();
            assert(extfs_write_inode_raw(&sb, xattr_ino, &xattr_raw) == EOK);
            extfs_free_inode(&sb, xattr_ino);
            assert(extfs_adjust_used_dirs(&sb, xattr_ino, -1) == EOK);
            free(xattr_handle);
            removed_directories++;
        }

        assert(extfs_read_inode_raw(&sb, EXT2_ROOT_INO, &root_raw) == EOK);
        assert(root_raw.i_links_count >= 2 + removed_directories);
        root_raw.i_links_count -= removed_directories;
        assert(extfs_write_inode_raw(&sb, EXT2_ROOT_INO, &root_raw) == EOK);
        assert(extfs_transaction_commit(&sb, &transaction) == EOK);
        free(directory);
        free(file_handle);
        if (parent != root) free(parent);
        FILE *output = fopen(argv[2], "wb");
        assert(output && fwrite(image.data, 1, image.size, output) == image.size);
        fclose(output);
    }
    free(root);
    extfs_free_super(&sb);
    free(image.data);
    puts(argc == 3 ? "PASS extfs journaled image mutation" :
                     "PASS extfs image mount, journal open, root inode and directory lookup");
    return 0;
}
