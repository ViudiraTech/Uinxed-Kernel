/*
 *
 *      ntfs_write_test.c
 *      NTFS write-path host tests
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <stdio.h>

#define CONFIG_NTFS_FS 0
#define NTFS_HOST_TEST 1
#include "../fs/ntfs/ntfs_vfs.c"

#define TEST_IMAGE_SIZE (128 * 1024)

static u8     test_image[TEST_IMAGE_SIZE];
static u8     linear_mft_runs[] = {0x11, 0x20, 0x04, 0x00};
static size_t test_writes;
static size_t test_fail_write_at = (size_t)-1;
static int    tests_run;
static int    tests_failed;

#define EXPECT_TRUE(condition)                                          \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            tests_failed++;                                             \
            return;                                                     \
        }                                                               \
    } while (0)

int blockdev_read_bytes(const blockdev_device_t *device, uint64_t offset, void *buffer, size_t size)
{
    (void)device;
    if (!buffer || offset > TEST_IMAGE_SIZE || size > TEST_IMAGE_SIZE - offset) return -EIO;
    memcpy(buffer, test_image + offset, size);
    return EOK;
}

int blockdev_write_bytes(const blockdev_device_t *device, uint64_t offset, const void *buffer, size_t size)
{
    if (!device || device->read_only) return -EROFS;
    if (!buffer || offset > TEST_IMAGE_SIZE || size > TEST_IMAGE_SIZE - offset) return -EIO;
    if (test_writes == test_fail_write_at) {
        test_fail_write_at = (size_t)-1;
        return -EIO;
    }
    memcpy(test_image + offset, buffer, size);
    test_writes++;
    return EOK;
}

static void put16(u8 *buffer, size_t offset, u16 value)
{
    buffer[offset]     = (u8)value;
    buffer[offset + 1] = (u8)(value >> 8);
}

static void put32(u8 *buffer, size_t offset, u32 value)
{
    buffer[offset]     = (u8)value;
    buffer[offset + 1] = (u8)(value >> 8);
    buffer[offset + 2] = (u8)(value >> 16);
    buffer[offset + 3] = (u8)(value >> 24);
}

static void make_resident_record(u8 *record)
{
    const size_t attribute = 0x38;

    memset(record, 0, 1024);
    put32(record, 0x00, MFT_MAGIC);
    put16(record, 0x04, 0x30);
    put16(record, 0x06, 3);
    put16(record, 0x14, (u16)attribute);
    put16(record, 0x16, 1);
    put32(record, 0x18, (u32)(attribute + 36));
    put32(record, 0x1c, 1024);

    put16(record, 0x30, 0xa55a);
    put16(record, 0x32, 0x1122);
    put16(record, 0x34, 0x3344);
    put16(record, 510, 0xa55a);
    put16(record, 1022, 0xa55a);

    put32(record, attribute + 0x00, AT_DATA);
    put32(record, attribute + 0x04, 32);
    put32(record, attribute + 0x10, 5);
    put16(record, attribute + 0x14, 24);
    memcpy(record + attribute + 24, "hello", 5);
    put32(record, attribute + 32, AT_END);
}

static void put64(u8 *buffer, size_t offset, u64 value)
{
    put32(buffer, offset, (u32)value);
    put32(buffer, offset + 4, (u32)(value >> 32));
}

static void make_nonresident_record(u8 *record)
{
    const size_t attribute = 0x38;
    const u8     runs[]    = {0x11, 0x01, 0x14, 0x11, 0x01, 0x0a, 0x00};

    memset(record, 0, 1024);
    put32(record, 0x00, MFT_MAGIC);
    put16(record, 0x04, 0x30);
    put16(record, 0x06, 3);
    put16(record, 0x14, (u16)attribute);
    put16(record, 0x16, 1);
    put32(record, 0x18, (u32)(attribute + 76));
    put32(record, 0x1c, 1024);
    put16(record, 0x30, 0xa55a);
    put16(record, 0x32, 0x1122);
    put16(record, 0x34, 0x3344);
    put16(record, 510, 0xa55a);
    put16(record, 1022, 0xa55a);

    put32(record, attribute + 0x00, AT_DATA);
    put32(record, attribute + 0x04, 72);
    record[attribute + 0x08] = 1;
    put64(record, attribute + 0x10, 0);
    put64(record, attribute + 0x18, 1);
    put16(record, attribute + 0x20, 64);
    put64(record, attribute + 0x28, 1024);
    put64(record, attribute + 0x30, 1024);
    put64(record, attribute + 0x38, 1024);
    memcpy(record + attribute + 64, runs, sizeof(runs));
    put32(record, attribute + 72, AT_END);
}

static void make_mft_record_zero(u8 *record)
{
    const size_t attribute = 0x38;
    const u8     runs[]    = {0x11, 0x04, 0x04, 0x11, 0x14, 0x24, 0x00};

    memset(record, 0, 1024);
    put32(record, 0x00, MFT_MAGIC);
    put16(record, 0x04, 0x30);
    put16(record, 0x06, 3);
    put16(record, 0x14, (u16)attribute);
    put16(record, 0x16, 1);
    put32(record, 0x18, (u32)(attribute + 76));
    put32(record, 0x1c, 1024);
    put16(record, 0x30, 0xa55a);
    put16(record, 0x32, 0x1122);
    put16(record, 0x34, 0x3344);
    put16(record, 510, 0xa55a);
    put16(record, 1022, 0xa55a);
    put32(record, attribute + 0x00, AT_DATA);
    put32(record, attribute + 0x04, 72);
    record[attribute + 0x08] = 1;
    put64(record, attribute + 0x10, 0);
    put64(record, attribute + 0x18, 23);
    put16(record, attribute + 0x20, 64);
    put64(record, attribute + 0x28, 24 * 512);
    put64(record, attribute + 0x30, 24 * 512);
    put64(record, attribute + 0x38, 24 * 512);
    memcpy(record + attribute + 64, runs, sizeof(runs));
    put32(record, attribute + 72, AT_END);
}

static int unpack_record_for_test(u8 *record)
{
    u16 usa_offset = le16(record + 4);
    u16 usa_count  = le16(record + 6);
    u16 sequence   = le16(record + usa_offset);

    if (usa_offset + usa_count * 2 > 1024 || usa_count != 3) return -1;
    for (u16 i = 1; i < usa_count; i++) {
        size_t trailer = (size_t)i * 512 - 2;
        if (le16(record + trailer) != sequence) return -1;
        put16(record, trailer, le16(record + usa_offset + i * 2));
    }
    return 0;
}

static void init_resident_handle(ntfs_mount_t *mount, ntfs_handle_t *handle)
{
    memset(mount, 0, sizeof(*mount));
    memset(handle, 0, sizeof(*handle));
    mount->dev.sector_size  = 512;
    mount->dev.sector_count = TEST_IMAGE_SIZE / 512;
    mount->sector_size      = 512;
    mount->cluster_size     = 512;
    mount->cluster_bits     = 9;
    mount->cluster_mask     = 511;
    mount->mft_size         = 1024;
    mount->mft_lcn          = 4;
    mount->nr_clusters      = TEST_IMAGE_SIZE / 512;
    mount->mft_runlist      = linear_mft_runs;
    mount->mft_runlist_size = sizeof(linear_mft_runs);
    mount->mft_data_size    = 32 * 512;
    mount->write_enabled    = 1;
    mount->dirty_owned      = 1;
    handle->mnt             = mount;
    handle->mft_no          = 7;
    handle->file_size       = 5;
    handle->is_resident     = 1;
    handle->runlist_buf     = malloc(5);
    handle->runlist_sz      = 5;
    memcpy(handle->runlist_buf, "hello", 5);
}

static void test_resident_write_persists_protected_record(void)
{
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    u8            unpacked[1024];

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    test_writes = 0;
    init_resident_handle(&mount, &handle);
    make_resident_record(test_image + record_offset);

    EXPECT_TRUE(ntfs_vfs_write(&handle, "XYZ", 1, 3) == 3);
    EXPECT_TRUE(test_writes == 1);

    memcpy(unpacked, test_image + record_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE(memcmp(unpacked + 0x38 + 24, "hXYZo", 5) == 0);
    EXPECT_TRUE(memcmp(handle.runlist_buf, "hXYZo", 5) == 0);
    free(handle.runlist_buf);
}

static void test_resident_growth_zero_fills_gap(void)
{
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    const u8      expected[]    = {'h', 'e', 'l', 'l', 'o', 0, 0, '!'};
    u8            unpacked[1024];

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    test_writes = 0;
    init_resident_handle(&mount, &handle);
    make_resident_record(test_image + record_offset);

    EXPECT_TRUE(ntfs_vfs_write(&handle, "!", 7, 1) == 1);
    EXPECT_TRUE(test_writes == 1);
    memcpy(unpacked, test_image + record_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE(le32(unpacked + 0x38 + 0x10) == sizeof(expected));
    EXPECT_TRUE(memcmp(unpacked + 0x38 + 24, expected, sizeof(expected)) == 0);
    EXPECT_TRUE(handle.file_size == sizeof(expected));
    EXPECT_TRUE(handle.runlist_sz == sizeof(expected));
    EXPECT_TRUE(memcmp(handle.runlist_buf, expected, sizeof(expected)) == 0);
    free(handle.runlist_buf);
}

static void test_resident_growth_converts_to_nonresident_data(void)
{
    static u8     bitmap_runs[]   = {0x11, 0x01, 0x3c, 0x00};
    const u8      expected_runs[] = {0x11, 0x01, 0x1f, 0x00};
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    u8            unpacked[1024];

    tests_run++;
    memset(test_image, 0xcc, sizeof(test_image));
    test_writes = 0;
    init_resident_handle(&mount, &handle);
    mount.bitmap_runlist      = bitmap_runs;
    mount.bitmap_runlist_size = sizeof(bitmap_runs);
    mount.bitmap_data_size    = 8;
    make_resident_record(test_image + record_offset);
    test_image[60 * 512 + 0] = 0xff;
    test_image[60 * 512 + 1] = 0xff;
    test_image[60 * 512 + 2] = 0xff;
    test_image[60 * 512 + 3] = 0x7f;

    EXPECT_TRUE(ntfs_vfs_write(&handle, "R", 8, 1) == 1);
    EXPECT_TRUE(test_image[60 * 512 + 3] == 0xff);
    EXPECT_TRUE(memcmp(test_image + 31 * 512, "hello", 5) == 0);
    EXPECT_TRUE(test_image[31 * 512 + 5] == 0);
    EXPECT_TRUE(test_image[31 * 512 + 6] == 0);
    EXPECT_TRUE(test_image[31 * 512 + 7] == 0);
    EXPECT_TRUE(test_image[31 * 512 + 8] == 'R');
    memcpy(unpacked, test_image + record_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE(unpacked[0x38 + 8] == 1);
    EXPECT_TRUE(le64(unpacked + 0x38 + 0x30) == 9);
    EXPECT_TRUE(memcmp(unpacked + 0x38 + 64, expected_runs, sizeof(expected_runs)) == 0);
    EXPECT_TRUE(handle.is_resident == 0);
    EXPECT_TRUE(handle.file_size == 9);
    EXPECT_TRUE(memcmp(handle.runlist_buf, expected_runs, sizeof(expected_runs)) == 0);
    free(handle.runlist_buf);
}

static void test_bad_update_sequence_rejects_without_mutation(void)
{
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    u8            before[1024];

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    test_writes = 0;
    init_resident_handle(&mount, &handle);
    make_resident_record(test_image + record_offset);
    test_image[record_offset + 510] ^= 1;
    memcpy(before, test_image + record_offset, sizeof(before));

    EXPECT_TRUE(ntfs_vfs_write(&handle, "X", 0, 1) == 0);
    EXPECT_TRUE(test_writes == 0);
    EXPECT_TRUE(memcmp(test_image + record_offset, before, sizeof(before)) == 0);
    EXPECT_TRUE(memcmp(handle.runlist_buf, "hello", 5) == 0);
    free(handle.runlist_buf);
}

static void test_mount_without_write_permission_rejects_without_mutation(void)
{
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    u8            before[1024];

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    test_writes = 0;
    init_resident_handle(&mount, &handle);
    mount.write_enabled = 0;
    make_resident_record(test_image + record_offset);
    memcpy(before, test_image + record_offset, sizeof(before));

    EXPECT_TRUE(ntfs_vfs_write(&handle, "X", 0, 1) == 0);
    EXPECT_TRUE(test_writes == 0);
    EXPECT_TRUE(memcmp(test_image + record_offset, before, sizeof(before)) == 0);
    EXPECT_TRUE(memcmp(handle.runlist_buf, "hello", 5) == 0);
    free(handle.runlist_buf);
}

static void test_attribute_list_file_rejects_without_mutation(void)
{
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    u8            before[1024];
    u8           *record = test_image + record_offset;

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    test_writes = 0;
    init_resident_handle(&mount, &handle);
    make_resident_record(record);
    memmove(record + 0x38 + 24, record + 0x38, 36);
    memset(record + 0x38, 0, 24);
    put32(record, 0x38, AT_ATTRIBUTE_LIST);
    put32(record, 0x38 + 4, 24);
    put16(record, 0x38 + 20, 24);
    put32(record, 0x18, le32(record + 0x18) + 24);
    memcpy(before, record, sizeof(before));

    EXPECT_TRUE(ntfs_vfs_write(&handle, "X", 0, 1) == 0);
    EXPECT_TRUE(test_writes == 0);
    EXPECT_TRUE(memcmp(record, before, sizeof(before)) == 0);
    free(handle.runlist_buf);
}

static void test_nonresident_write_crosses_physical_runs(void)
{
    const u8      runs[] = {0x11, 0x01, 0x14, 0x11, 0x01, 0x0a, 0x00};
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;

    tests_run++;
    memset(test_image, 0xcc, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    memset(&handle, 0, sizeof(handle));
    test_writes            = 0;
    mount.dev.sector_size  = 512;
    mount.dev.sector_count = TEST_IMAGE_SIZE / 512;
    mount.sector_size      = 512;
    mount.cluster_size     = 512;
    mount.cluster_bits     = 9;
    mount.cluster_mask     = 511;
    mount.mft_size         = 1024;
    mount.mft_lcn          = 4;
    mount.nr_clusters      = TEST_IMAGE_SIZE / 512;
    mount.mft_runlist      = linear_mft_runs;
    mount.mft_runlist_size = sizeof(linear_mft_runs);
    mount.mft_data_size    = 32 * 512;
    mount.write_enabled    = 1;
    mount.dirty_owned      = 1;
    handle.mnt             = &mount;
    handle.mft_no          = 7;
    handle.file_size       = 1024;
    handle.runlist_buf     = malloc(sizeof(runs));
    handle.runlist_sz      = sizeof(runs);
    memcpy(handle.runlist_buf, runs, sizeof(runs));
    make_nonresident_record(test_image + record_offset);

    EXPECT_TRUE(ntfs_vfs_write(&handle, "ABCD", 510, 4) == 4);
    EXPECT_TRUE(test_writes == 2);
    EXPECT_TRUE(test_image[20 * 512 + 509] == 0xcc);
    EXPECT_TRUE(memcmp(test_image + 20 * 512 + 510, "AB", 2) == 0);
    EXPECT_TRUE(memcmp(test_image + 30 * 512, "CD", 2) == 0);
    EXPECT_TRUE(test_image[30 * 512 + 2] == 0xcc);
    EXPECT_TRUE(le16(test_image + record_offset + 510) == 0xa55a);
    free(handle.runlist_buf);
}

static void test_nonresident_initialized_growth_zeroes_before_metadata(void)
{
    const u8      runs[] = {0x11, 0x01, 0x14, 0x11, 0x01, 0x0a, 0x00};
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    u8            unpacked[1024];

    tests_run++;
    memset(test_image, 0xcc, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    memset(&handle, 0, sizeof(handle));
    test_writes            = 0;
    mount.dev.sector_size  = 512;
    mount.dev.sector_count = TEST_IMAGE_SIZE / 512;
    mount.sector_size      = 512;
    mount.cluster_size     = 512;
    mount.cluster_bits     = 9;
    mount.cluster_mask     = 511;
    mount.mft_size         = 1024;
    mount.mft_lcn          = 4;
    mount.nr_clusters      = TEST_IMAGE_SIZE / 512;
    mount.mft_runlist      = linear_mft_runs;
    mount.mft_runlist_size = sizeof(linear_mft_runs);
    mount.mft_data_size    = 32 * 512;
    mount.write_enabled    = 1;
    mount.dirty_owned      = 1;
    handle.mnt             = &mount;
    handle.mft_no          = 7;
    handle.file_size       = 1024;
    handle.runlist_buf     = malloc(sizeof(runs));
    handle.runlist_sz      = sizeof(runs);
    memcpy(handle.runlist_buf, runs, sizeof(runs));
    make_nonresident_record(test_image + record_offset);
    put64(test_image + record_offset, 0x38 + 0x38, 512);

    EXPECT_TRUE(ntfs_vfs_write(&handle, "Z", 600, 1) == 1);
    EXPECT_TRUE(test_writes == 3);
    for (size_t i = 0; i < 88; i++) EXPECT_TRUE(test_image[30 * 512 + i] == 0);
    EXPECT_TRUE(test_image[30 * 512 + 88] == 'Z');
    EXPECT_TRUE(test_image[30 * 512 + 89] == 0xcc);
    memcpy(unpacked, test_image + record_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE(le64(unpacked + 0x38 + 0x38) == 601);
    EXPECT_TRUE(le64(unpacked + 0x38 + 0x30) == 1024);
    free(handle.runlist_buf);
}

static void test_nonresident_growth_allocates_bitmap_cluster_before_mft(void)
{
    static u8     bitmap_runs[]   = {0x11, 0x01, 0x3c, 0x00};
    const u8      runs[]          = {0x11, 0x01, 0x14, 0x11, 0x01, 0x0a, 0x00};
    const u8      expected_runs[] = {0x11, 0x01, 0x14, 0x11, 0x02, 0x0a, 0x00};
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    u8            unpacked[1024];

    tests_run++;
    memset(test_image, 0xcc, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    memset(&handle, 0, sizeof(handle));
    test_writes               = 0;
    mount.dev.sector_size     = 512;
    mount.dev.sector_count    = TEST_IMAGE_SIZE / 512;
    mount.sector_size         = 512;
    mount.cluster_size        = 512;
    mount.cluster_bits        = 9;
    mount.cluster_mask        = 511;
    mount.mft_size            = 1024;
    mount.mft_lcn             = 4;
    mount.nr_clusters         = TEST_IMAGE_SIZE / 512;
    mount.mft_runlist         = linear_mft_runs;
    mount.mft_runlist_size    = sizeof(linear_mft_runs);
    mount.mft_data_size       = 32 * 512;
    mount.bitmap_runlist      = bitmap_runs;
    mount.bitmap_runlist_size = sizeof(bitmap_runs);
    mount.bitmap_data_size    = 8;
    mount.write_enabled       = 1;
    mount.dirty_owned         = 1;
    handle.mnt                = &mount;
    handle.mft_no             = 7;
    handle.file_size          = 1024;
    handle.runlist_buf        = malloc(sizeof(runs));
    handle.runlist_sz         = sizeof(runs);
    memcpy(handle.runlist_buf, runs, sizeof(runs));
    make_nonresident_record(test_image + record_offset);
    test_image[60 * 512 + 0] = 0xff;
    test_image[60 * 512 + 1] = 0xff;
    test_image[60 * 512 + 2] = 0xff;
    test_image[60 * 512 + 3] = 0x7f;

    EXPECT_TRUE(ntfs_vfs_write(&handle, "Q", 1024, 1) == 1);
    EXPECT_TRUE(test_image[60 * 512 + 3] == 0xff);
    EXPECT_TRUE(test_image[31 * 512] == 'Q');
    EXPECT_TRUE(test_image[31 * 512 + 1] == 0);
    memcpy(unpacked, test_image + record_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE(le64(unpacked + 0x38 + 0x18) == 2);
    EXPECT_TRUE(le64(unpacked + 0x38 + 0x28) == 1536);
    EXPECT_TRUE(le64(unpacked + 0x38 + 0x30) == 1025);
    EXPECT_TRUE(le64(unpacked + 0x38 + 0x38) == 1025);
    EXPECT_TRUE(memcmp(unpacked + 0x38 + 64, expected_runs, sizeof(expected_runs)) == 0);
    EXPECT_TRUE(handle.file_size == 1025);
    EXPECT_TRUE(memcmp(handle.runlist_buf, expected_runs, sizeof(expected_runs)) == 0);
    free(handle.runlist_buf);
}

static void test_mft_record_read_uses_mft_data_runs(void)
{
    const u8     mft_runs[] = {0x11, 0x04, 0x04, 0x11, 0x14, 0x24, 0x00};
    ntfs_mount_t mount;
    u8           record[1024];
    const u64    mapped_offset = 50 * 512;

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    mount.dev.sector_size  = 512;
    mount.dev.sector_count = TEST_IMAGE_SIZE / 512;
    mount.sector_size      = 512;
    mount.cluster_size     = 512;
    mount.cluster_bits     = 9;
    mount.cluster_mask     = 511;
    mount.mft_size         = 1024;
    mount.mft_lcn          = 4;
    mount.nr_clusters      = TEST_IMAGE_SIZE / 512;
    mount.mft_runlist      = (u8 *)mft_runs;
    mount.mft_runlist_size = sizeof(mft_runs);
    mount.mft_data_size    = 24 * 512;
    make_resident_record(test_image + mapped_offset);

    EXPECT_TRUE(mft_read(&mount, 7, record) == 0);
    EXPECT_TRUE(le32(record) == MFT_MAGIC);
    EXPECT_TRUE(memcmp(record + 0x38 + 24, "hello", 5) == 0);
}

static void test_mft_record_read_bootstraps_data_runs_from_record_zero(void)
{
    ntfs_mount_t mount;
    u8           record[1024];

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    mount.dev.sector_size  = 512;
    mount.dev.sector_count = TEST_IMAGE_SIZE / 512;
    mount.sector_size      = 512;
    mount.cluster_size     = 512;
    mount.cluster_bits     = 9;
    mount.cluster_mask     = 511;
    mount.mft_size         = 1024;
    mount.mft_lcn          = 4;
    mount.nr_clusters      = TEST_IMAGE_SIZE / 512;
    make_mft_record_zero(test_image + 4 * 512);
    make_resident_record(test_image + 50 * 512);

    EXPECT_TRUE(mft_read(&mount, 7, record) == 0);
    EXPECT_TRUE(mount.mft_runlist != NULL);
    EXPECT_TRUE(mount.mft_data_size == 24 * 512);
    EXPECT_TRUE(memcmp(record + 0x38 + 24, "hello", 5) == 0);
    free(mount.mft_runlist);
}

static void test_mft_system_record_write_updates_mirror(void)
{
    ntfs_mount_t mount;
    u8           record[1024];
    const u64    mirror_offset = 52 * 512 + 3 * 1024;

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    mount.dev.sector_size  = 512;
    mount.dev.sector_count = TEST_IMAGE_SIZE / 512;
    mount.sector_size      = 512;
    mount.cluster_size     = 512;
    mount.cluster_bits     = 9;
    mount.cluster_mask     = 511;
    mount.mft_size         = 1024;
    mount.mft_lcn          = 4;
    mount.mftmirr_lcn      = 52;
    mount.nr_clusters      = TEST_IMAGE_SIZE / 512;
    mount.mft_runlist      = linear_mft_runs;
    mount.mft_runlist_size = sizeof(linear_mft_runs);
    mount.mft_data_size    = 32 * 512;
    make_resident_record(record);
    EXPECT_TRUE(ntfs_record_unpack(&mount, record, sizeof(record)) == 0);
    test_writes = 0;

    EXPECT_TRUE(mft_write(&mount, 3, record) == 0);
    EXPECT_TRUE(test_writes == 2);
    EXPECT_TRUE(le32(test_image + mirror_offset) == MFT_MAGIC);
    EXPECT_TRUE(le16(test_image + mirror_offset + 510) == le16(test_image + mirror_offset + 0x30));
}

static void make_index_root_record(u8 *record)
{
    const size_t attribute = 0x38;
    const size_t value     = attribute + 32;

    memset(record, 0, 1024);
    put32(record, 0x00, MFT_MAGIC);
    put16(record, 0x04, 0x30);
    put16(record, 0x06, 3);
    put16(record, 0x14, (u16)attribute);
    put16(record, 0x10, 1);
    put16(record, 0x12, 1);
    put16(record, 0x16, 3);
    put32(record, 0x18, (u32)(attribute + 80 + 8));
    put32(record, 0x1c, 1024);
    put16(record, 0x30, 0xa55a);
    put16(record, 0x32, 0x1122);
    put16(record, 0x34, 0x3344);
    put16(record, 510, 0xa55a);
    put16(record, 1022, 0xa55a);

    put32(record, attribute + 0x00, AT_INDEX_ROOT);
    put32(record, attribute + 0x04, 80);
    record[attribute + 0x09] = 4;
    put16(record, attribute + 0x0a, 24);
    put32(record, attribute + 0x10, 48);
    put16(record, attribute + 0x14, 32);
    put16(record, attribute + 24, '$');
    put16(record, attribute + 26, 'I');
    put16(record, attribute + 28, '3');
    put16(record, attribute + 30, '0');

    put32(record, value + 0x00, AT_FILE_NAME);
    put32(record, value + 0x04, 1);
    put32(record, value + 0x08, 4096);
    record[value + 0x0c] = 1;
    put32(record, value + 0x10, 0x10);
    put32(record, value + 0x14, 0x20);
    put32(record, value + 0x18, 0x20);
    put16(record, value + 0x20 + 8, 16);
    put16(record, value + 0x20 + 12, 0x02);
    put32(record, attribute + 80, AT_END);
}

static void make_mft_record_zero_with_bitmap(u8 *record)
{
    const size_t bitmap = 0x38 + 72;

    make_mft_record_zero(record);
    put32(record, 0x18, (u32)(bitmap + 32 + 8));
    put32(record, bitmap + 0x00, AT_BITMAP);
    put32(record, bitmap + 0x04, 32);
    put32(record, bitmap + 0x10, 4);
    put16(record, bitmap + 0x14, 24);
    record[bitmap + 24] = 0xff;
    record[bitmap + 25] = 0xff;
    put32(record, bitmap + 32, AT_END);
}

static u16 *index_entry_name(u8 *entry)
{
    return (u16 *)(entry + 0x52);
}

static void test_resident_i30_insert_orders_and_rejects_duplicates(void)
{
    ntfs_mount_t mount;
    u8           record[1024];
    u8           before[1024];
    const u16    beta[]  = {'b', 'e', 't', 'a'};
    const u16    alpha[] = {'a', 'l', 'p', 'h', 'a'};
    u8          *value;
    u8          *first;
    u8          *second;

    tests_run++;
    memset(&mount, 0, sizeof(mount));
    mount.mft_size = sizeof(record);
    make_index_root_record(record);

    EXPECT_TRUE(ntfs_index_root_insert(&mount, record, 42, 5, beta, 4, 0x20, 7, 7) == 0);
    EXPECT_TRUE(ntfs_index_root_insert(&mount, record, 43, 5, alpha, 5, 0x20, 9, 9) == 0);

    value  = record + 0x38 + le16(record + 0x38 + 0x14);
    first  = value + 0x10 + le32(value + 0x10);
    second = first + le16(first + 8);
    EXPECT_TRUE(le64(first) == 43);
    EXPECT_TRUE(((struct fname_attr *)(first + 0x10))->name_len == 5);
    EXPECT_TRUE(index_entry_name(first)[0] == 'a');
    EXPECT_TRUE(le64(second) == 42);
    EXPECT_TRUE(index_entry_name(second)[0] == 'b');
    EXPECT_TRUE(le16(second + le16(second + 8) + 12) == 0x02);

    memcpy(before, record, sizeof(before));
    EXPECT_TRUE(ntfs_index_root_insert(&mount, record, 44, 5, beta, 4, 0x20, 1, 1) == -EEXIST);
    EXPECT_TRUE(memcmp(before, record, sizeof(before)) == 0);
    const u16 upper_beta[] = {'B', 'E', 'T', 'A'};
    EXPECT_TRUE(ntfs_index_root_insert(&mount, record, 44, 5, upper_beta, 4, 0x20, 1, 1) == -EEXIST);
    EXPECT_TRUE(memcmp(before, record, sizeof(before)) == 0);

    EXPECT_TRUE(ntfs_index_root_remove(&mount, record, 42, beta, 4) == 0);
    value = record + 0x38 + le16(record + 0x38 + 0x14);
    first = value + 0x10 + le32(value + 0x10);
    EXPECT_TRUE(le64(first) == 43);
    EXPECT_TRUE(le16(first + le16(first + 8) + 12) == INDEX_ENTRY_END);
    memcpy(before, record, sizeof(before));
    EXPECT_TRUE(ntfs_index_root_remove(&mount, record, 42, beta, 4) == -ENOENT);
    EXPECT_TRUE(memcmp(before, record, sizeof(before)) == 0);
}

static void test_mkfile_allocates_mft_record_before_parent_entry(void)
{
    static u8       namespace_mft_runs[] = {0x11, 0x30, 0x04, 0x00};
    ntfs_mount_t    mount;
    ntfs_handle_t   parent_handle;
    struct vfs_node parent_node;
    struct vfs_node child_node;
    u8              unpacked[1024];
    const u64       record_zero_offset = 4 * 512;
    const u64       parent_offset      = record_zero_offset + 5 * 1024;
    const u64       child_offset       = record_zero_offset + 16 * 1024;

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    memset(&parent_handle, 0, sizeof(parent_handle));
    memset(&parent_node, 0, sizeof(parent_node));
    memset(&child_node, 0, sizeof(child_node));
    mount.dev.sector_size  = 512;
    mount.dev.sector_count = TEST_IMAGE_SIZE / 512;
    mount.sector_size      = 512;
    mount.cluster_size     = 512;
    mount.cluster_bits     = 9;
    mount.cluster_mask     = 511;
    mount.mft_size         = 1024;
    mount.mft_lcn          = 4;
    mount.nr_clusters      = TEST_IMAGE_SIZE / 512;
    mount.mft_runlist      = namespace_mft_runs;
    mount.mft_runlist_size = sizeof(namespace_mft_runs);
    mount.mft_data_size    = 48 * 512;
    mount.write_enabled    = 1;
    mount.dirty_owned      = 1;
    parent_handle.mnt      = &mount;
    parent_handle.mft_no   = 5;
    parent_handle.is_dir   = 1;
    parent_node.handle     = &parent_handle;
    parent_node.type       = file_dir;
    child_node.parent      = &parent_node;
    child_node.name        = "created.txt";
    make_mft_record_zero_with_bitmap(test_image + record_zero_offset);
    make_index_root_record(test_image + parent_offset);
    test_writes = 0;

    EXPECT_TRUE(ntfs_vfs_mkfile(&parent_handle, child_node.name, &child_node) == 0);
    EXPECT_TRUE(child_node.handle != NULL);
    EXPECT_TRUE(child_node.inode == 16);
    EXPECT_TRUE(test_writes == 3);

    memcpy(unpacked, test_image + child_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE(le32(unpacked) == MFT_MAGIC);
    EXPECT_TRUE((le16(unpacked + 0x16) & 3) == 1);

    memcpy(unpacked, test_image + record_zero_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE((unpacked[0x38 + 72 + 24 + 2] & 1) != 0);

    memcpy(unpacked, test_image + parent_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    u8 *value = unpacked + 0x38 + le16(unpacked + 0x38 + 0x14);
    u8 *entry = value + 0x10 + le32(value + 0x10);
    EXPECT_TRUE((le64(entry) & 0x0000ffffffffffffULL) == 16);

    EXPECT_TRUE(ntfs_vfs_rename(child_node.handle, "a.txt") == 0);
    memcpy(unpacked, test_image + parent_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    value = unpacked + 0x38 + le16(unpacked + 0x38 + 0x14);
    entry = value + 0x10 + le32(value + 0x10);
    EXPECT_TRUE(((struct fname_attr *)(entry + 0x10))->name_len == 5);
    EXPECT_TRUE(index_entry_name(entry)[0] == 'a');
    memcpy(unpacked, test_image + child_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    u32 child_attribute = le16(unpacked + 0x14);
    while (le32(unpacked + child_attribute) != AT_FILE_NAME) child_attribute += le32(unpacked + child_attribute + 4);
    u16 child_value = le16(unpacked + child_attribute + 0x14);
    EXPECT_TRUE(unpacked[child_attribute + child_value + 64] == 5);
    EXPECT_TRUE(le16(unpacked + child_attribute + child_value + 66) == 'a');
    EXPECT_TRUE(((ntfs_handle_t *)child_node.handle)->name_length == 5);
    EXPECT_TRUE(ntfs_vfs_rename(child_node.handle, "A.TXT") == 0);
    memcpy(unpacked, test_image + parent_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    value = unpacked + 0x38 + le16(unpacked + 0x38 + 0x14);
    entry = value + 0x10 + le32(value + 0x10);
    EXPECT_TRUE(index_entry_name(entry)[0] == 'A');
    EXPECT_TRUE(index_entry_name(entry)[2] == 'T');

    struct vfs_node alias_node;
    memset(&alias_node, 0, sizeof(alias_node));
    alias_node.parent = &parent_node;
    alias_node.name   = "alias.txt";
    EXPECT_TRUE(ntfs_namespace_hardlink_locked(&parent_handle, child_node.handle, alias_node.name, &alias_node) == 0);
    memcpy(unpacked, test_image + child_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE(le16(unpacked + 0x12) == 2);
    memcpy(unpacked, test_image + record_zero_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE((unpacked[0x38 + 72 + 24 + 2] & 1) != 0);
    EXPECT_TRUE(ntfs_vfs_delete(&parent_handle, &alias_node) == 0);
    memcpy(unpacked, test_image + child_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE(le16(unpacked + 0x12) == 1);
    memcpy(unpacked, test_image + record_zero_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE((unpacked[0x38 + 72 + 24 + 2] & 1) != 0);
    ntfs_vfs_free(alias_node.handle);

    EXPECT_TRUE(ntfs_vfs_delete(&parent_handle, &child_node) == 0);
    memcpy(unpacked, test_image + record_zero_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE((unpacked[0x38 + 72 + 24 + 2] & 1) == 0);
    memcpy(unpacked, test_image + parent_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    value = unpacked + 0x38 + le16(unpacked + 0x38 + 0x14);
    entry = value + 0x10 + le32(value + 0x10);
    EXPECT_TRUE((le16(entry + 12) & INDEX_ENTRY_END) != 0);
    memcpy(unpacked, test_image + child_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    EXPECT_TRUE((le16(unpacked + 0x16) & 1) == 0);
    ntfs_vfs_free(child_node.handle);

    struct vfs_node symlink_node;
    char            link_target[32];
    memset(&symlink_node, 0, sizeof(symlink_node));
    memset(link_target, 0, sizeof(link_target));
    symlink_node.parent = &parent_node;
    symlink_node.name   = "shortcut";
    EXPECT_TRUE(ntfs_namespace_symlink_locked(&parent_handle, symlink_node.name, "../target", &symlink_node) == 0);
    EXPECT_TRUE((symlink_node.type & file_symlink) != 0);
    EXPECT_TRUE(ntfs_vfs_readlink(&symlink_node, link_target, 0, sizeof(link_target)) == 9);
    EXPECT_TRUE(memcmp(link_target, "../target", 9) == 0);
    memcpy(unpacked, test_image + child_offset, sizeof(unpacked));
    EXPECT_TRUE(unpack_record_for_test(unpacked) == 0);
    child_attribute = le16(unpacked + 0x14);
    while (le32(unpacked + child_attribute) != AT_REPARSE_POINT) child_attribute += le32(unpacked + child_attribute + 4);
    child_value = le16(unpacked + child_attribute + 0x14);
    EXPECT_TRUE(le32(unpacked + child_attribute + child_value) == IO_REPARSE_TAG_SYMLINK);
    EXPECT_TRUE(le32(unpacked + child_attribute + child_value + 16) == 1);
    EXPECT_TRUE(ntfs_vfs_delete(&parent_handle, &symlink_node) == 0);
    ntfs_vfs_free(symlink_node.handle);
}

static void test_unlink_nonresident_file_releases_data_and_mft_bits(void)
{
    static u8       namespace_mft_runs[] = {0x11, 0x30, 0x04, 0x00};
    static u8       bitmap_runs[]        = {0x11, 0x01, 0x3c, 0x00};
    ntfs_mount_t    mount;
    ntfs_handle_t   parent_handle;
    ntfs_handle_t   child_handle;
    struct vfs_node child_node;
    const u16       name[]             = {'l', 'a', 'r', 'g', 'e'};
    const u64       record_zero_offset = 4 * 512;
    const u64       child_offset       = record_zero_offset + 16 * 1024;
    u8              record[1024];

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    memset(&parent_handle, 0, sizeof(parent_handle));
    memset(&child_handle, 0, sizeof(child_handle));
    memset(&child_node, 0, sizeof(child_node));
    mount.dev.sector_size      = 512;
    mount.dev.sector_count     = TEST_IMAGE_SIZE / 512;
    mount.sector_size          = 512;
    mount.cluster_size         = 512;
    mount.cluster_bits         = 9;
    mount.cluster_mask         = 511;
    mount.mft_size             = 1024;
    mount.mft_lcn              = 4;
    mount.nr_clusters          = TEST_IMAGE_SIZE / 512;
    mount.mft_runlist          = namespace_mft_runs;
    mount.mft_runlist_size     = sizeof(namespace_mft_runs);
    mount.mft_data_size        = 48 * 512;
    mount.bitmap_runlist       = bitmap_runs;
    mount.bitmap_runlist_size  = sizeof(bitmap_runs);
    mount.bitmap_data_size     = 8;
    mount.write_enabled        = 1;
    mount.dirty_owned          = 1;
    parent_handle.mnt          = &mount;
    parent_handle.mft_no       = 5;
    parent_handle.is_dir       = 1;
    child_handle.mnt           = &mount;
    child_handle.mft_no        = 16;
    child_handle.parent_mft_no = 5;
    child_handle.file_size     = 1024;
    child_handle.file_attr     = 0x20;
    child_handle.name          = malloc(sizeof(name));
    child_handle.name_length   = 5;
    memcpy(child_handle.name, name, sizeof(name));
    child_node.handle = &child_handle;
    make_mft_record_zero_with_bitmap(test_image + record_zero_offset);
    test_image[record_zero_offset + 0x38 + 72 + 24 + 2] = 1;
    make_index_root_record(record);
    EXPECT_TRUE(ntfs_record_unpack(&mount, record, sizeof(record)) == 0);
    EXPECT_TRUE(ntfs_index_root_insert(&mount, record, (1ULL << 48) | 16, (1ULL << 48) | 5, name, 5, 0x20, 1024, 1024) == 0);
    EXPECT_TRUE(mft_write(&mount, 5, record) == 0);
    make_nonresident_record(test_image + child_offset);
    put16(test_image + child_offset, 0x10, 1);
    put16(test_image + child_offset, 0x12, 1);
    put16(test_image + child_offset, 0x16, 1);
    put32(test_image + child_offset, 0x2c, 16);
    test_image[60 * 512 + 2] |= 1U << 4;
    test_image[60 * 512 + 3] |= 1U << 6;

    EXPECT_TRUE(ntfs_vfs_delete(&parent_handle, &child_node) == 0);
    EXPECT_TRUE((test_image[60 * 512 + 2] & (1U << 4)) == 0);
    EXPECT_TRUE((test_image[60 * 512 + 3] & (1U << 6)) == 0);
    memcpy(record, test_image + record_zero_offset, sizeof(record));
    EXPECT_TRUE(unpack_record_for_test(record) == 0);
    EXPECT_TRUE((record[0x38 + 72 + 24 + 2] & 1) == 0);
    free(child_handle.name);
}

static void test_resident_i30_overflow_promotes_entries_to_indx(void)
{
    static u8    bitmap_runs[] = {0x21, 0x01, 0xf0, 0x00, 0x00};
    ntfs_mount_t mount;
    u8           record[1024];
    u8           indx[2048];
    u8          *value;
    u8          *entry;
    u16          name[2];
    int          inserted = 0;

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    mount.dev.sector_size     = 512;
    mount.dev.sector_count    = TEST_IMAGE_SIZE / 512;
    mount.sector_size         = 512;
    mount.cluster_size        = 512;
    mount.cluster_bits        = 9;
    mount.cluster_mask        = 511;
    mount.mft_size            = 1024;
    mount.indx_size           = 2048;
    mount.nr_clusters         = TEST_IMAGE_SIZE / 512;
    mount.bitmap_runlist      = bitmap_runs;
    mount.bitmap_runlist_size = sizeof(bitmap_runs);
    mount.bitmap_data_size    = 32;
    memset(test_image + 240 * 512, 0xff, 30);
    test_image[240 * 512 + 30] = 0x01;
    make_index_root_record(record);

    for (u16 digit = 0; digit < 10; digit++) {
        name[0]    = 'a';
        name[1]    = '0' + digit;
        int status = ntfs_index_root_insert(&mount, record, 40 + digit, 5, name, 2, 0x20, 0, 0);
        if (status == -ENOSPC) break;
        EXPECT_TRUE(status == 0);
        inserted++;
    }
    EXPECT_TRUE(inserted > 0);
    name[0]            = 'z';
    name[1]            = 'z';
    int promote_status = ntfs_index_root_promote(&mount, record, 99, 5, name, 2, 0x20, 0, 0);
    EXPECT_TRUE(promote_status != -ENOSPC);
    EXPECT_TRUE(promote_status != -EIO);
    EXPECT_TRUE(promote_status == 0);
    EXPECT_TRUE((test_image[240 * 512 + 30] & 0x1e) == 0x1e);
    memcpy(indx, test_image + 241 * 512, sizeof(indx));
    EXPECT_TRUE(le32(indx) == INDX_MAGIC);
    EXPECT_TRUE(ntfs_record_unpack(&mount, indx, sizeof(indx)) == 0);
    EXPECT_TRUE((indx[0x18 + 12] & INDEX_ENTRY_NODE) == 0);
    EXPECT_TRUE(le16(indx + 0x18 + le32(indx + 0x18) + 12) != INDEX_ENTRY_END);
    u32 attr           = le16(record + 0x14);
    int saw_allocation = 0;
    int saw_bitmap     = 0;
    while (attr + 8 <= le32(record + 0x18) && le32(record + attr) != AT_END) {
        if (le32(record + attr) == AT_INDEX_ALLOCATION) saw_allocation = 1;
        if (le32(record + attr) == AT_BITMAP) saw_bitmap = 1;
        attr += le32(record + attr + 4);
    }
    EXPECT_TRUE(saw_allocation && saw_bitmap);

    const u16 later[] = {'z', '2'};
    EXPECT_TRUE(ntfs_directory_index_insert(&mount, record, 100, 5, later, 2, 0x20, 0, 0) == 0);
    memcpy(indx, test_image + 241 * 512, sizeof(indx));
    EXPECT_TRUE(ntfs_record_unpack(&mount, indx, sizeof(indx)) == 0);
    u32 position    = 0x18 + le32(indx + 0x18);
    int found_later = 0;
    while (position + 16 <= sizeof(indx)) {
        u16 length = le16(indx + position + 8);
        u16 flags  = le16(indx + position + 12);
        if (flags & INDEX_ENTRY_END) break;
        struct fname_attr *file_name = (struct fname_attr *)(indx + position + 16);
        if (file_name->name_len == 2 && le16((u8 *)file_name->name) == 'z' && le16((u8 *)file_name->name + 2) == '2') found_later = 1;
        position += length;
    }
    EXPECT_TRUE(found_later);

    for (u16 digit = 0; digit < 20; digit++) {
        u16 split_name[] = {'m', (u16)('A' + digit)};
        EXPECT_TRUE(ntfs_directory_index_insert(&mount, record, 120 + digit, 5, split_name, 2, 0x20, 0, 0) == 0);
    }
    const u16 reparse_name[] = {'r', 'P'};
    EXPECT_TRUE(ntfs_directory_index_insert(&mount, record, 180, 5, reparse_name, 2, 0x20 | FILE_ATTRIBUTE_REPARSE_POINT, 0, 0) == 0);
    EXPECT_TRUE(ntfs_directory_index_set_reparse_tag(&mount, record, 180, reparse_name, 2) == 0);
    struct ntfs_index_item *items      = NULL;
    u32                     item_count = 0;
    EXPECT_TRUE(ntfs_directory_index_collect(&mount, record, &items, &item_count, NULL) == 0);
    int found_reparse = 0;
    for (u32 item = 0; item < item_count; item++) {
        struct fname_attr *file_name = (struct fname_attr *)(items[item].entry + 16);
        if ((le64(items[item].entry) & 0x0000ffffffffffffULL) == 180) {
            EXPECT_TRUE(le32((u8 *)&file_name->rp_tag) == IO_REPARSE_TAG_SYMLINK);
            found_reparse = 1;
        }
    }
    ntfs_index_items_free(items, item_count);
    EXPECT_TRUE(found_reparse);
    EXPECT_TRUE((test_image[240 * 512 + 31] & 1) != 0);
    attr = le16(record + 0x14);
    while (le32(record + attr) != AT_INDEX_ROOT) attr += le32(record + attr + 4);
    value = record + attr + le16(record + attr + 0x14);
    entry = value + 0x10 + le32(value + 0x10);
    EXPECT_TRUE((le16(entry + 12) & INDEX_ENTRY_END) == 0);
    EXPECT_TRUE((le16(entry + 12) & INDEX_ENTRY_NODE) != 0);

    int later_remove_status = ntfs_directory_index_remove(&mount, record, 100, later, 2);
    EXPECT_TRUE(later_remove_status == 0);
    EXPECT_TRUE(ntfs_directory_index_remove(&mount, record, 100, later, 2) == -ENOENT);

    attr = le16(record + 0x14);
    while (le32(record + attr) != AT_INDEX_ROOT) attr += le32(record + attr + 4);
    value                                 = record + attr + le16(record + attr + 0x14);
    entry                                 = value + 0x10 + le32(value + 0x10);
    u64                promoted_reference = le64(entry);
    struct fname_attr *promoted_name      = (struct fname_attr *)(entry + 16);
    u16                promoted_utf16[255];
    for (u32 i = 0; i < promoted_name->name_len; i++) promoted_utf16[i] = le16((u8 *)promoted_name->name + i * 2);
    EXPECT_TRUE(ntfs_directory_index_remove(&mount, record, promoted_reference, promoted_utf16, promoted_name->name_len) == 0);
    EXPECT_TRUE(ntfs_directory_index_remove(&mount, record, promoted_reference, promoted_utf16, promoted_name->name_len) == -ENOENT);

    for (u16 digit = 0; digit < (u16)inserted; digit++) {
        u16 remove_name[] = {'a', (u16)('0' + digit)};
        int status        = ntfs_directory_index_remove(&mount, record, 40 + digit, remove_name, 2);
        EXPECT_TRUE(status == 0 || ((u64)(40 + digit) == promoted_reference && status == -ENOENT));
    }
    const u16 zz[]      = {'z', 'z'};
    int       zz_status = ntfs_directory_index_remove(&mount, record, 99, zz, 2);
    EXPECT_TRUE(zz_status == 0 || (promoted_reference == 99 && zz_status == -ENOENT));
    for (u16 digit = 0; digit < 20; digit++) {
        u16 remove_name[] = {'m', (u16)('A' + digit)};
        int status        = ntfs_directory_index_remove(&mount, record, 120 + digit, remove_name, 2);
        EXPECT_TRUE(status == 0 || ((u64)(120 + digit) == promoted_reference && status == -ENOENT));
    }
    int reparse_status = ntfs_directory_index_remove(&mount, record, 180, reparse_name, 2);
    EXPECT_TRUE(reparse_status == 0 || (promoted_reference == 180 && reparse_status == -ENOENT));
    EXPECT_TRUE(ntfs_index_reclaim_commit(&mount) == 0);
    attr           = le16(record + 0x14);
    saw_allocation = 0;
    saw_bitmap     = 0;
    while (attr + 8 <= le32(record + 0x18) && le32(record + attr) != AT_END) {
        if (le32(record + attr) == AT_INDEX_ALLOCATION) saw_allocation = 1;
        if (le32(record + attr) == AT_BITMAP && ((struct attr_rec *)(record + attr))->name_length) saw_bitmap = 1;
        attr += le32(record + attr + 4);
    }
    EXPECT_TRUE(!saw_allocation && !saw_bitmap);
    EXPECT_TRUE((test_image[240 * 512 + 30] & 0x1e) == 0);
    EXPECT_TRUE((test_image[240 * 512 + 31] & 1) == 0);
}

static void test_bitmap_update_rolls_back_partial_device_failure(void)
{
    static u8    bitmap_runs[] = {0x11, 0x01, 0x3c, 0x00};
    ntfs_mount_t mount;
    const s64    lcn[]    = {1, 9};
    const s64    length[] = {1, 1};

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    mount.dev.sector_size     = 512;
    mount.dev.sector_count    = TEST_IMAGE_SIZE / 512;
    mount.cluster_size        = 512;
    mount.cluster_bits        = 9;
    mount.nr_clusters         = TEST_IMAGE_SIZE / 512;
    mount.bitmap_runlist      = bitmap_runs;
    mount.bitmap_runlist_size = sizeof(bitmap_runs);
    mount.bitmap_data_size    = 32;
    test_writes               = 0;
    test_fail_write_at        = 1;

    EXPECT_TRUE(bitmap_change_extents(&mount, lcn, length, 2, 1) == -EIO);
    EXPECT_TRUE(test_image[60 * 512] == 0);
    EXPECT_TRUE(test_image[60 * 512 + 1] == 0);
    test_fail_write_at = (size_t)-1;
}

static void test_stat_loads_resident_data_from_mft(void)
{
    ntfs_mount_t  mount;
    ntfs_handle_t handle;
    struct vfs_node node;
    const u64     record_offset = 4 * 512 + 7 * 1024;
    char          buffer[5];

    tests_run++;
    memset(test_image, 0, sizeof(test_image));
    memset(&mount, 0, sizeof(mount));
    memset(&handle, 0, sizeof(handle));
    memset(&node, 0, sizeof(node));
    make_resident_record(test_image + record_offset);

    mount.dev.sector_size  = 512;
    mount.dev.sector_count = TEST_IMAGE_SIZE / 512;
    mount.sector_size      = 512;
    mount.cluster_size     = 512;
    mount.cluster_bits     = 9;
    mount.cluster_mask     = 511;
    mount.mft_size         = 1024;
    mount.mft_lcn          = 4;
    mount.nr_clusters      = TEST_IMAGE_SIZE / 512;
    mount.mft_runlist      = linear_mft_runs;
    mount.mft_runlist_size = sizeof(linear_mft_runs);
    mount.mft_data_size    = 32 * 512;
    handle.mnt             = &mount;
    handle.mft_no          = 7;

    EXPECT_TRUE(ntfs_vfs_stat(&handle, &node) == 0);
    EXPECT_TRUE(node.size == 5);
    EXPECT_TRUE(ntfs_vfs_read(&handle, buffer, 0, sizeof(buffer)) == sizeof(buffer));
    EXPECT_TRUE(memcmp(buffer, "hello", sizeof(buffer)) == 0);
    free(handle.runlist_buf);
}

int main(void)
{
    test_resident_write_persists_protected_record();
    if (!tests_failed) printf("PASS resident NTFS write persists a protected MFT record\n");
    test_resident_growth_zero_fills_gap();
    if (!tests_failed) printf("PASS resident NTFS growth zero-fills its gap\n");
    test_resident_growth_converts_to_nonresident_data();
    if (!tests_failed) printf("PASS resident NTFS growth converts to non-resident data\n");
    test_bad_update_sequence_rejects_without_mutation();
    if (!tests_failed) printf("PASS invalid NTFS update sequence is rejected before mutation\n");
    test_mount_without_write_permission_rejects_without_mutation();
    if (!tests_failed) printf("PASS NTFS mount without write permission rejects mutation\n");
    test_attribute_list_file_rejects_without_mutation();
    if (!tests_failed) printf("PASS NTFS attribute-list file rejects mutation\n");
    test_nonresident_write_crosses_physical_runs();
    if (!tests_failed) printf("PASS non-resident NTFS write crosses physical runs\n");
    test_nonresident_initialized_growth_zeroes_before_metadata();
    if (!tests_failed) printf("PASS non-resident NTFS initialized growth commits data before metadata\n");
    test_nonresident_growth_allocates_bitmap_cluster_before_mft();
    if (!tests_failed) printf("PASS non-resident NTFS growth allocates data before bitmap and MFT\n");
    test_mft_record_read_uses_mft_data_runs();
    if (!tests_failed) printf("PASS MFT record I/O follows the MFT data runlist\n");
    test_mft_record_read_bootstraps_data_runs_from_record_zero();
    if (!tests_failed) printf("PASS MFT record I/O bootstraps its runlist from record zero\n");
    test_mft_system_record_write_updates_mirror();
    if (!tests_failed) printf("PASS MFT system record writes update the MFT mirror\n");
    test_resident_i30_insert_orders_and_rejects_duplicates();
    if (!tests_failed) printf("PASS resident I30 insert orders entries and rejects duplicates\n");
    test_mkfile_allocates_mft_record_before_parent_entry();
    if (!tests_failed) printf("PASS mkfile commits MFT allocation before parent namespace entry\n");
    test_unlink_nonresident_file_releases_data_and_mft_bits();
    if (!tests_failed) printf("PASS unlink releases non-resident data and MFT allocation bits\n");
    test_resident_i30_overflow_promotes_entries_to_indx();
    if (!tests_failed) printf("PASS resident I30 overflow promotes entries into protected INDX\n");
    test_bitmap_update_rolls_back_partial_device_failure();
    if (!tests_failed) printf("PASS NTFS bitmap update rolls back partial device failure\n");
    test_stat_loads_resident_data_from_mft();
    if (!tests_failed) printf("PASS NTFS stat loads authoritative file data from MFT\n");
    printf("%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
