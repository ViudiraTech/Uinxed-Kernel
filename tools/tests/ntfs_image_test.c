/*
 *
 *      ntfs_image_test.c
 *      Host interoperability test against ntfs-3g-created images
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
#define CONFIG_NTFS_FS 1
#define NTFS_HOST_TEST 1
#include "../../fs/ntfs/ntfs_vfs.c"

typedef struct image_backend {
        uint8_t *data;
        size_t   size;
} image_backend_t;

/* Unused full-VFS entry points retained by sanitizer instrumentation must never run in this image harness. */
vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name)
{
    (void)parent;
    (void)name;
    assert(0);
    return NULL;
}
vfs_node_t vfs_do_search(vfs_node_t dir, const char *name)
{
    (void)dir;
    (void)name;
    assert(0);
    return NULL;
}
vfs_node_t vfs_open(const char *path)
{
    (void)path;
    assert(0);
    return NULL;
}
int vfs_close(vfs_node_t node)
{
    (void)node;
    assert(0);
    return -ENOSYS;
}
int vfs_regist_fs(const char *name, vfs_callback_t callback)
{
    (void)name;
    (void)callback;
    assert(0);
    return -ENOSYS;
}
int blockdev_open_name(const char *name, blockdev_device_t *device)
{
    (void)name;
    (void)device;
    assert(0);
    return -ENOSYS;
}
void plogk(const char *format, ...)
{
    (void)format;
    assert(0);
}

uint32_t timer_realtime_seconds32(void) { return 1780000000U; }

void spin_lock(spinlock_t *lock) { assert(!lock->lock); lock->lock = 1; }
void spin_unlock(spinlock_t *lock) { assert(lock->lock); lock->lock = 0; }
void sched_yield(void) {}
void blockdev_retain(const blockdev_device_t *device) { (void)device; }
void blockdev_release(const blockdev_device_t *device) { (void)device; }
int blockdev_flush(const blockdev_device_t *device) { (void)device; return EOK; }

int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size)
{
    image_backend_t *image = device->backend_data;
    if (offset > image->size || size > image->size - offset) return -EIO;
    memcpy(buffer, image->data + offset, size);
    return EOK;
}

int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size)
{
    image_backend_t *image = device->backend_data;
    if (device->read_only) return -EROFS;
    if (offset > image->size || size > image->size - offset) return -EIO;
    memcpy(image->data + offset, buffer, size);
    return EOK;
}

static void initialize_mount(ntfs_mount_t *mnt, blockdev_device_t *device)
{
    struct ntfs_boot_sector *boot = (struct ntfs_boot_sector *)((image_backend_t *)device->backend_data)->data;
    memset(mnt, 0, sizeof(*mnt));
    mnt->dev = *device;
    assert(le64((u8 *)&boot->oem_id) == magicNTFS);
    u32 sectors_per_cluster = boot->bpb.sectors_per_cluster;
    assert(sectors_per_cluster && !(sectors_per_cluster & (sectors_per_cluster - 1)));
    mnt->sector_size = le16((u8 *)&boot->bpb.bytes_per_sector);
    mnt->cluster_size = mnt->sector_size * sectors_per_cluster;
    mnt->cluster_bits = __builtin_ctz(mnt->cluster_size);
    mnt->cluster_mask = mnt->cluster_size - 1;
    s8 mft_record = boot->clusters_per_mft_record;
    mnt->mft_size = mft_record > 0 ? mnt->cluster_size << (__builtin_ffs(mft_record) - 1) : 1U << -mft_record;
    mnt->mft_bits = __builtin_ctz(mnt->mft_size);
    s8 index_record = boot->clusters_per_index_record;
    mnt->indx_size = index_record > 0 ? mnt->cluster_size << (__builtin_ffs(index_record) - 1) : 1U << -index_record;
    mnt->mft_lcn = (s64)le64((u8 *)&boot->mft_lcn);
    mnt->mftmirr_lcn = (s64)le64((u8 *)&boot->mftmirr_lcn);
    mnt->nr_clusters = (s64)(le64((u8 *)&boot->number_of_sectors) / sectors_per_cluster);
    mnt->indx_vcn_per_cluster = mnt->cluster_size / mnt->indx_size;
    if (!mnt->indx_vcn_per_cluster) mnt->indx_vcn_per_cluster = 1;
    assert(fs_txn_log_init(&mnt->transaction_log, &mnt->dev, mnt->dev.sector_size, NULL, NULL) == EOK);
    mnt->transaction_log_initialized = 1;
    assert(fs_txn_recover(&mnt->transaction_log) == EOK);
    assert(ntfs_prepare_write(mnt) == EOK && mnt->write_enabled);
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *file = fopen(argv[1], "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    image_backend_t image = {.data = malloc((size_t)length), .size = (size_t)length};
    assert(image.data && fread(image.data, 1, image.size, file) == image.size);
    fclose(file);
    blockdev_device_t device = {.backend_data = &image,
                                .sector_size = 512,
                                .sector_count = image.size / 512};
    ntfs_mount_t mount;
    initialize_mount(&mount, &device);
    ntfs_handle_t root = {.mnt = &mount, .mft_no = 5, .is_dir = 1};

    assert(ntfs_set_volume_dirty(&mount, 1) == EOK);
    mount.dirty_owned = 1;
    fs_txn_t transaction;
    assert(ntfs_transaction_begin(&mount, &transaction, 65536) == EOK);
    vfs_node_t node = calloc(1, sizeof(*node));
    assert(node);
    assert(ntfs_namespace_create_locked(&root, "native-c.txt", node, 0) == EOK);
    assert(ntfs_transaction_finish(&mount, &transaction, EOK) == EOK);

    static const char payload[] = "native-c-ntfs\n";
    ntfs_handle_t *handle = node->handle;
    assert(handle);
    assert(ntfs_transaction_begin(&mount, &transaction, 65536) == EOK);
    assert(ntfs_vfs_write_locked(handle, payload, 0, sizeof(payload) - 1) == sizeof(payload) - 1);
    assert(ntfs_transaction_finish(&mount, &transaction, EOK) == EOK);
    assert(blockdev_flush(&mount.dev) == EOK);
    assert(ntfs_set_volume_dirty(&mount, 0) == EOK);
    mount.dirty_owned = 0;

    file = fopen(argv[2], "wb");
    assert(file && fwrite(image.data, 1, image.size, file) == image.size);
    fclose(file);
    free(handle->runlist_buf);
    free(handle->name);
    free(handle);
    free(node);
    free(mount.upcase);
    free(mount.bitmap_runlist);
    free(mount.mft_runlist);
    fs_txn_log_destroy(&mount.transaction_log);
    free(image.data);
    puts("PASS NTFS transactional create and write image mutation");
    return 0;
}
