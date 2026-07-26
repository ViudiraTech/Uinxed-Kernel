#include <drivers/partition.h>
#include <kernel/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                    \
    do {                                                             \
        if (!(condition)) {                                          \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, message); \
            failures++;                                              \
            return;                                                  \
        }                                                            \
    } while (0)

typedef struct memory_disk {
        uint8_t *data;
        uint64_t sectors;
        uint32_t sector_size;
} memory_disk_t;

static uint16_t load_le16(const void *ptr)
{
    const uint8_t *p = ptr;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t load_le32(const void *ptr)
{
    const uint8_t *p = ptr;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le16(void *ptr, uint16_t value)
{
    uint8_t *p = ptr;
    p[0]       = (uint8_t)value;
    p[1]       = (uint8_t)(value >> 8);
}

static void store_le32(void *ptr, uint32_t value)
{
    uint8_t *p = ptr;
    p[0]       = (uint8_t)value;
    p[1]       = (uint8_t)(value >> 8);
    p[2]       = (uint8_t)(value >> 16);
    p[3]       = (uint8_t)(value >> 24);
}

static void store_le64(void *ptr, uint64_t value)
{
    store_le32(ptr, (uint32_t)value);
    store_le32((uint8_t *)ptr + 4, (uint32_t)(value >> 32));
}

static uint32_t fixture_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t       crc   = UINT32_MAX;

    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ UINT32_MAX;
}

int blockdev_read_sectors(const blockdev_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    memory_disk_t *disk;

    if (!device || !buffer) return -EINVAL;
    if (!count) return EOK;
    if (lba >= device->sector_count || count > device->sector_count - lba) return -EINVAL;
    disk = device->backend_data;
    memcpy(buffer, disk->data + (size_t)lba * disk->sector_size, (size_t)count * disk->sector_size);
    return EOK;
}

static blockdev_device_t make_device(memory_disk_t *disk, uint64_t sectors, uint32_t sector_size)
{
    blockdev_device_t device;

    disk->data = calloc((size_t)sectors, sector_size);
    if (!disk->data) abort();
    disk->sectors     = sectors;
    disk->sector_size = sector_size;
    memset(&device, 0, sizeof(device));
    device.backend_data = disk;
    device.sector_size  = sector_size;
    device.sector_count = sectors;
    return device;
}

static void free_device(memory_disk_t *disk)
{
    free(disk->data);
    memset(disk, 0, sizeof(*disk));
}

static void set_mbr_entry(uint8_t *sector, unsigned int slot, uint8_t status, uint8_t type, uint32_t start, uint32_t count)
{
    uint8_t *entry = sector + 446 + slot * 16;

    entry[0] = status;
    entry[4] = type;
    store_le32(entry + 8, start);
    store_le32(entry + 12, count);
}

static void finish_mbr(uint8_t *sector, uint32_t signature)
{
    store_le32(sector + 440, signature);
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

static void make_pmbr(uint8_t *sector, uint64_t sectors)
{
    uint32_t protected = sectors - 1 > UINT32_MAX ? UINT32_MAX : (uint32_t)(sectors - 1);

    set_mbr_entry(sector, 0, 0, 0xEE, 1, protected);
    finish_mbr(sector, 0xA1B2C3D4);
}

static void set_gpt_entry(uint8_t *entries, uint32_t entry_size, uint32_t index, uint64_t first, uint64_t last, const uint16_t *name,
                          size_t name_len)
{
    static const uint8_t type_guid[16]   = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};
    static const uint8_t unique_guid[16] = {0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78};
    uint8_t             *entry           = entries + (size_t)index * entry_size;

    memcpy(entry, type_guid, sizeof(type_guid));
    memcpy(entry + 16, unique_guid, sizeof(unique_guid));
    store_le64(entry + 32, first);
    store_le64(entry + 40, last);
    store_le64(entry + 48, 1ULL << 60);
    if (name_len > 36) name_len = 36;
    for (size_t i = 0; i < name_len; i++) store_le16(entry + 56 + i * 2, name[i]);
}

static void write_gpt_header(uint8_t *sector, uint32_t sector_size, uint64_t current, uint64_t alternate, uint64_t first_usable,
                             uint64_t last_usable, uint64_t entries_lba, uint32_t entry_count, uint32_t entry_size, uint32_t entries_crc)
{
    static const uint8_t disk_guid[16] = {0xEF, 0xCD, 0xAB, 0x90, 0x34, 0x12, 0x78, 0x56, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78};

    memset(sector, 0, sector_size);
    memcpy(sector, "EFI PART", 8);
    store_le32(sector + 8, 0x00010000);
    store_le32(sector + 12, 92);
    store_le64(sector + 24, current);
    store_le64(sector + 32, alternate);
    store_le64(sector + 40, first_usable);
    store_le64(sector + 48, last_usable);
    memcpy(sector + 56, disk_guid, sizeof(disk_guid));
    store_le64(sector + 72, entries_lba);
    store_le32(sector + 80, entry_count);
    store_le32(sector + 84, entry_size);
    store_le32(sector + 88, entries_crc);
    store_le32(sector + 16, fixture_crc32(sector, 92));
}

static void make_gpt(memory_disk_t *disk, uint32_t entry_size, uint64_t first, uint64_t last, const uint16_t *name, size_t name_len)
{
    const uint32_t entry_count  = 4;
    size_t         entries_size = (size_t)entry_count * entry_size;
    uint64_t       entry_blocks = (entries_size + disk->sector_size - 1) / disk->sector_size;
    uint64_t       backup       = disk->sectors - 1;
    uint64_t       backup_table = backup - entry_blocks;
    uint64_t       first_usable = 2 + entry_blocks;
    uint64_t       last_usable  = backup_table - 1;
    uint8_t       *primary      = disk->data + disk->sector_size * 2;
    uint8_t       *secondary    = disk->data + (size_t)backup_table * disk->sector_size;

    make_pmbr(disk->data, disk->sectors);
    set_gpt_entry(primary, entry_size, 0, first, last, name, name_len);
    memcpy(secondary, primary, entries_size);
    uint32_t entries_crc = fixture_crc32(primary, entries_size);
    write_gpt_header(disk->data + disk->sector_size, disk->sector_size, 1, backup, first_usable, last_usable, 2, entry_count, entry_size,
                     entries_crc);
    write_gpt_header(disk->data + (size_t)backup * disk->sector_size, disk->sector_size, backup, 1, first_usable, last_usable, backup_table,
                     entry_count, entry_size, entries_crc);
}

static void test_mbr_primary_and_extended_use_linux_numbers(void)
{
    memory_disk_t     disk;
    blockdev_device_t device = make_device(&disk, 4096, 512);
    partition_table_t table;

    set_mbr_entry(disk.data, 0, 0x80, 0x83, 2048, 512);
    set_mbr_entry(disk.data, 1, 0, 0x0F, 100, 1000);
    finish_mbr(disk.data, 0x1234ABCD);
    set_mbr_entry(disk.data + 100 * 512, 0, 0, 0x83, 1, 50);
    set_mbr_entry(disk.data + 100 * 512, 1, 0, 0x0F, 100, 900);
    finish_mbr(disk.data + 100 * 512, 0);
    set_mbr_entry(disk.data + 200 * 512, 0, 0, 0x82, 1, 60);
    finish_mbr(disk.data + 200 * 512, 0);

    CHECK(partition_scan(&device, &table) == EOK, "valid MBR rejected");
    CHECK(table.type == PARTITION_TABLE_MBR, "wrong table type");
    CHECK(!table.hybrid, "ordinary MBR was marked hybrid");
    CHECK(table.count == 4, "primary, extended container, and logical partitions not found");
    const partition_info_t *p1 = partition_find(&table, 1);
    const partition_info_t *p2 = partition_find(&table, 2);
    const partition_info_t *p5 = partition_find(&table, 5);
    const partition_info_t *p6 = partition_find(&table, 6);
    CHECK(p1 && p1->start_lba == 2048 && p1->sector_count == 512 && p1->bootable, "primary partition metadata wrong");
    CHECK(p2 && p2->mbr_type == 0x0F && p2->sector_count == 2, "Linux extended-container view wrong");
    CHECK(p5 && p5->start_lba == 101 && p5->sector_count == 50, "first logical partition wrong");
    CHECK(p6 && p6->start_lba == 201 && p6->sector_count == 60, "second logical partition wrong");
    char uuid[PARTITION_UUID_STRING_SIZE];
    CHECK(partition_format_uuid(&table, p6, uuid, sizeof(uuid)) == EOK, "MBR UUID formatting failed");
    CHECK(strcmp(uuid, "1234abcd-06") == 0, "MBR PARTUUID differs from Linux");
    partition_table_destroy(&table);
    free_device(&disk);
}

static void test_ebr_cycle_is_rejected(void)
{
    memory_disk_t     disk;
    blockdev_device_t device = make_device(&disk, 1024, 512);
    partition_table_t table;

    set_mbr_entry(disk.data, 0, 0, 0x05, 10, 500);
    finish_mbr(disk.data, 1);
    set_mbr_entry(disk.data + 10 * 512, 0, 0, 0x83, 1, 5);
    set_mbr_entry(disk.data + 10 * 512, 1, 0, 0x05, 10, 400);
    finish_mbr(disk.data + 10 * 512, 0);
    set_mbr_entry(disk.data + 20 * 512, 0, 0, 0x83, 1, 5);
    set_mbr_entry(disk.data + 20 * 512, 1, 0, 0x05, 0, 500);
    finish_mbr(disk.data + 20 * 512, 0);

    CHECK(partition_scan(&device, &table) == -ELOOP, "cyclic EBR chain accepted");
    free_device(&disk);
}

static void test_ebr_chain_stops_at_linux_partition_limit(void)
{
    memory_disk_t     disk;
    blockdev_device_t device = make_device(&disk, 2048, 512);
    partition_table_t table;
    const uint32_t    extended_start = 10;
    const uint32_t    extended_count = 1000;

    set_mbr_entry(disk.data, 0, 0, 0x0F, extended_start, extended_count);
    finish_mbr(disk.data, 1);
    for (uint32_t number = 5; number <= PARTITION_MAX_COUNT; number++) {
        uint32_t relative = (number - 5) * 2;
        uint8_t *ebr      = disk.data + (size_t)(extended_start + relative) * 512;

        set_mbr_entry(ebr, 0, 0, 0x83, 1, 1);
        set_mbr_entry(ebr, 1, 0, 0x0F, relative + 2, extended_count - relative - 2);
        finish_mbr(ebr, 0);
    }

    CHECK(partition_scan(&device, &table) == EOK, "EBR chain beyond the Linux partition limit was not truncated");
    CHECK(partition_find(&table, PARTITION_MAX_COUNT) != NULL, "last Linux partition number was not exposed");
    CHECK(partition_find(&table, PARTITION_MAX_COUNT + 1) == NULL, "partition beyond the Linux limit was exposed");
    partition_table_destroy(&table);
    free_device(&disk);
}

static void test_gpt_crc_metadata_and_precedence(void)
{
    static const uint16_t name[] = {'r', 'o', 'o', 't'};
    memory_disk_t         disk;
    blockdev_device_t     device = make_device(&disk, 128, 512);
    partition_table_t     table;

    make_gpt(&disk, 128, 40, 80, name, sizeof(name) / sizeof(name[0]));
    set_mbr_entry(disk.data, 1, 0, 0x83, 20, 5);
    CHECK(partition_scan(&device, &table) == EOK, "valid GPT rejected");
    CHECK(table.type == PARTITION_TABLE_GPT && table.count == 1 && table.hybrid, "hybrid MBR precedence or metadata wrong");
    const partition_info_t *part = partition_find(&table, 1);
    CHECK(part && part->start_lba == 40 && part->sector_count == 41, "inclusive GPT end LBA handled incorrectly");
    CHECK(part->attributes == (1ULL << 60) && strcmp(part->name, "root") == 0, "GPT metadata lost");
    char uuid[PARTITION_UUID_STRING_SIZE];
    CHECK(partition_format_uuid(&table, part, uuid, sizeof(uuid)) == EOK, "GPT UUID formatting failed");
    CHECK(strcmp(uuid, "12345678-1234-5678-9abc-def012345678") == 0, "GPT mixed-endian PARTUUID wrong");
    partition_table_destroy(&table);
    free_device(&disk);
}

static void test_gpt_uses_valid_backup_after_primary_damage(void)
{
    static const uint16_t name[] = {'b', 'a', 'c', 'k', 'u', 'p'};
    memory_disk_t         disk;
    blockdev_device_t     device = make_device(&disk, 128, 512);
    partition_table_t     table;

    make_gpt(&disk, 128, 40, 80, name, sizeof(name) / sizeof(name[0]));
    disk.data[512 + 16] ^= 0x80;
    CHECK(partition_scan(&device, &table) == EOK, "valid backup GPT not used");
    CHECK(table.type == PARTITION_TABLE_GPT && table.degraded, "backup recovery not reported");
    CHECK(partition_find(&table, 1) != NULL, "backup entries not returned");
    partition_table_destroy(&table);
    free_device(&disk);
}

static void test_gpt_uses_backup_when_primary_entries_are_semantically_invalid(void)
{
    static const uint16_t name[] = {'s', 'a', 'f', 'e'};
    memory_disk_t         disk;
    blockdev_device_t     device = make_device(&disk, 128, 512);
    partition_table_t     table;

    make_gpt(&disk, 128, 40, 80, name, sizeof(name) / sizeof(name[0]));
    uint8_t *primary = disk.data + 2 * 512;
    store_le64(primary + 32, 126);
    uint32_t crc = fixture_crc32(primary, 4 * 128);
    write_gpt_header(disk.data + 512, 512, 1, 127, 3, 125, 2, 4, 128, crc);

    CHECK(partition_scan(&device, &table) == EOK, "valid backup not used after semantic primary damage");
    CHECK(table.degraded, "semantic recovery not reported");
    const partition_info_t *part = partition_find(&table, 1);
    CHECK(part && part->start_lba == 40 && part->sector_count == 41, "backup entry was not selected");
    partition_table_destroy(&table);
    free_device(&disk);
}

static void test_impossible_device_byte_geometry_is_rejected(void)
{
    memory_disk_t     disk = {0};
    blockdev_device_t device;
    partition_table_t table;

    disk.data        = calloc(1, 512);
    disk.sector_size = 512;
    memset(&device, 0, sizeof(device));
    device.backend_data = &disk;
    device.sector_size  = 512;
    device.sector_count = UINT64_MAX;
    CHECK(partition_scan(&device, &table) == -EOVERFLOW, "overflowing disk byte geometry accepted");
    free(disk.data);
}

static void test_gpt_supports_4k_sectors_larger_entries_and_utf16(void)
{
    static const uint16_t name[] = {'d', 'a', 't', 'a', 0xD83D, 0xDE80};
    memory_disk_t         disk;
    blockdev_device_t     device = make_device(&disk, 32, 4096);
    partition_table_t     table;

    make_gpt(&disk, 256, 4, 20, name, sizeof(name) / sizeof(name[0]));
    CHECK(partition_scan(&device, &table) == EOK, "4Kn GPT with 256-byte entries rejected");
    const partition_info_t *part = partition_find(&table, 1);
    CHECK(part && strcmp(part->name, "data\xF0\x9F\x9A\x80") == 0, "UTF-16 surrogate pair was not converted to UTF-8");
    partition_table_destroy(&table);
    free_device(&disk);
}

static void test_gpt_rejects_corrupt_arrays_and_overlaps(void)
{
    static const uint16_t name[] = {'b', 'a', 'd'};
    memory_disk_t         disk;
    blockdev_device_t     device = make_device(&disk, 128, 512);
    partition_table_t     table;

    make_gpt(&disk, 128, 40, 80, name, sizeof(name) / sizeof(name[0]));
    disk.data[2 * 512 + 32] ^= 1;
    disk.data[126 * 512 + 32] ^= 1;
    CHECK(partition_scan(&device, &table) == -EBADMSG, "corrupt GPT entry arrays accepted");

    memset(disk.data, 0, (size_t)disk.sectors * disk.sector_size);
    make_gpt(&disk, 128, 40, 80, name, sizeof(name) / sizeof(name[0]));
    uint8_t *primary   = disk.data + 2 * 512;
    uint8_t *secondary = disk.data + 126 * 512;
    set_gpt_entry(primary, 128, 1, 70, 90, name, sizeof(name) / sizeof(name[0]));
    memcpy(secondary, primary, 4 * 128);
    uint32_t crc = fixture_crc32(primary, 4 * 128);
    write_gpt_header(disk.data + 512, 512, 1, 127, 3, 125, 2, 4, 128, crc);
    write_gpt_header(disk.data + 127 * 512, 512, 127, 1, 3, 125, 126, 4, 128, crc);
    CHECK(partition_scan(&device, &table) == -EINVAL, "overlapping GPT partitions accepted");
    free_device(&disk);
}

static void test_gpt_rejects_zero_and_duplicate_partition_guids(void)
{
    static const uint16_t name[] = {'g', 'u', 'i', 'd'};
    memory_disk_t         disk;
    blockdev_device_t     device = make_device(&disk, 128, 512);
    partition_table_t     table;

    make_gpt(&disk, 128, 40, 80, name, sizeof(name) / sizeof(name[0]));
    uint8_t *primary   = disk.data + 2 * 512;
    uint8_t *secondary = disk.data + 126 * 512;
    memset(primary + 16, 0, 16);
    memcpy(secondary, primary, 4 * 128);
    uint32_t crc = fixture_crc32(primary, 4 * 128);
    write_gpt_header(disk.data + 512, 512, 1, 127, 3, 125, 2, 4, 128, crc);
    write_gpt_header(disk.data + 127 * 512, 512, 127, 1, 3, 125, 126, 4, 128, crc);
    CHECK(partition_scan(&device, &table) == -EINVAL, "zero GPT partition GUID accepted");

    memset(disk.data, 0, (size_t)disk.sectors * disk.sector_size);
    make_gpt(&disk, 128, 40, 80, name, sizeof(name) / sizeof(name[0]));
    primary   = disk.data + 2 * 512;
    secondary = disk.data + 126 * 512;
    set_gpt_entry(primary, 128, 1, 90, 100, name, sizeof(name) / sizeof(name[0]));
    memcpy(secondary, primary, 4 * 128);
    crc = fixture_crc32(primary, 4 * 128);
    write_gpt_header(disk.data + 512, 512, 1, 127, 3, 125, 2, 4, 128, crc);
    write_gpt_header(disk.data + 127 * 512, 512, 127, 1, 3, 125, 126, 4, 128, crc);
    CHECK(partition_scan(&device, &table) == -EINVAL, "duplicate GPT partition GUID accepted");
    free_device(&disk);
}

static void test_gpt_rejects_nonzero_reserved_header_bytes(void)
{
    static const uint16_t name[] = {'r', 's', 'v'};
    memory_disk_t         disk;
    blockdev_device_t     device = make_device(&disk, 128, 512);
    partition_table_t     table;

    make_gpt(&disk, 128, 40, 80, name, sizeof(name) / sizeof(name[0]));
    disk.data[512 + 100]       = 1;
    disk.data[127 * 512 + 100] = 1;
    CHECK(partition_scan(&device, &table) == -EBADMSG, "nonzero GPT header reserved bytes accepted");
    free_device(&disk);
}

int main(void)
{
    test_mbr_primary_and_extended_use_linux_numbers();
    test_ebr_cycle_is_rejected();
    test_ebr_chain_stops_at_linux_partition_limit();
    test_gpt_crc_metadata_and_precedence();
    test_gpt_uses_valid_backup_after_primary_damage();
    test_gpt_uses_backup_when_primary_entries_are_semantically_invalid();
    test_impossible_device_byte_geometry_is_rejected();
    test_gpt_supports_4k_sectors_larger_entries_and_utf16();
    test_gpt_rejects_corrupt_arrays_and_overlaps();
    test_gpt_rejects_zero_and_duplicate_partition_guids();
    test_gpt_rejects_nonzero_reserved_header_bytes();

    if (failures) {
        printf("%d partition test(s) failed\n", failures);
        return 1;
    }
    printf("PASS MBR/EBR and GPT partition semantics\n");
    return 0;
}
