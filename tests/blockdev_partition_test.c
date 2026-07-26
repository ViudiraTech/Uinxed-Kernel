#include <drivers/blockdev.h>
#include <kernel/errno.h>
#include <stdio.h>
#include <string.h>

static int      failures;
static int      backend_calls;
static uint64_t backend_lba;
static uint32_t backend_count;

void ide_read_sectors(uint8_t drive, uint8_t count, uint64_t lba, uint16_t *buffer)
{
    (void)drive;
    (void)count;
    (void)lba;
    (void)buffer;
}

void ide_write_sectors(uint8_t drive, uint8_t count, uint64_t lba, uint16_t *buffer)
{
    (void)drive;
    (void)count;
    (void)lba;
    (void)buffer;
}

int nvme_read_sectors(const blockdev_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    return -ENOSYS;
}

int nvme_write_sectors(const blockdev_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    return -ENOSYS;
}

int ahci_read_sectors(uint8_t drive, uint8_t count, uint64_t lba, void *buffer)
{
    (void)drive;
    (void)count;
    (void)lba;
    (void)buffer;
    return -ENOSYS;
}

int ahci_write_sectors(uint8_t drive, uint8_t count, uint64_t lba, const void *buffer)
{
    (void)drive;
    (void)count;
    (void)lba;
    (void)buffer;
    return -ENOSYS;
}

int ahci_satapi_read_sectors(uint8_t drive, uint8_t count, uint32_t lba, void *buffer)
{
    (void)drive;
    (void)count;
    (void)lba;
    (void)buffer;
    return -ENOSYS;
}

#define CHECK(condition, message)                                    \
    do {                                                             \
        if (!(condition)) {                                          \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, message); \
            failures++;                                              \
            return;                                                  \
        }                                                            \
    } while (0)

static int fake_read(const blockdev_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    backend_calls++;
    backend_lba   = device->base_lba + lba;
    backend_count = count;
    memset(buffer, 0xA5, (size_t)count * device->sector_size);
    return EOK;
}

static int fake_write(const blockdev_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    (void)buffer;
    backend_calls++;
    backend_lba   = device->base_lba + lba;
    backend_count = count;
    return EOK;
}

static blockdev_device_t make_parent(void)
{
    static struct blockdev_ops ops = {
        .read_sectors  = fake_read,
        .write_sectors = fake_write,
    };
    blockdev_device_t device;

    memset(&device, 0, sizeof(device));
    device.ops_id       = (uint8_t)blockdev_register_type(&ops);
    device.sector_size  = 512;
    device.base_lba     = (uint64_t)UINT32_MAX + 100;
    device.sector_count = 1000;
    return device;
}

static void test_partition_view_translates_64bit_lba_once(void)
{
    blockdev_device_t parent = make_parent();
    blockdev_device_t first;
    blockdev_device_t nested;
    uint8_t           buffer[1024];

    CHECK(blockdev_open_partition(&parent, 100, 200, &first) == EOK, "first partition view rejected");
    CHECK(blockdev_open_partition(&first, 25, 50, &nested) == EOK, "nested partition view rejected");
    CHECK(nested.base_lba == parent.base_lba + 125 && nested.sector_count == 50, "nested base was truncated or translated twice");

    backend_calls = 0;
    CHECK(blockdev_read_sectors(&nested, 10, 2, buffer) == EOK, "in-range partition read rejected");
    CHECK(backend_calls == 1 && backend_lba == parent.base_lba + 135 && backend_count == 2, "backend received wrong absolute LBA");
}

static void test_partition_view_rejects_cross_boundary_io(void)
{
    blockdev_device_t parent = make_parent();
    blockdev_device_t partition;
    uint8_t           buffer[1024];

    CHECK(blockdev_open_partition(&parent, 100, 20, &partition) == EOK, "partition view rejected");
    backend_calls = 0;
    CHECK(blockdev_read_sectors(&partition, 19, 2, buffer) == -EINVAL, "read crossing partition end reached backend");
    CHECK(blockdev_write_sectors(&partition, 20, 1, buffer) == -EINVAL, "write at partition end reached backend");
    CHECK(backend_calls == 0, "out-of-range request reached backend");
    CHECK(blockdev_read_sectors(&partition, 20, 0, NULL) == EOK, "zero-length I/O did not follow Linux no-op semantics");
    CHECK(blockdev_open_partition(&parent, 999, 2, &partition) == -EINVAL, "partition exceeding parent accepted");
}

static void test_byte_io_checks_device_size_before_allocation(void)
{
    blockdev_device_t parent = make_parent();
    blockdev_device_t partition;
    uint8_t           buffer[16];

    CHECK(blockdev_open_partition(&parent, 0, 4, &partition) == EOK, "partition view rejected");
    backend_calls = 0;
    CHECK(blockdev_read_bytes(&partition, 2040, buffer, sizeof(buffer)) == -EINVAL, "byte read crossing partition end accepted");
    CHECK(blockdev_write_bytes(&partition, UINT64_MAX - 3, buffer, sizeof(buffer)) == -EINVAL, "overflowing byte write accepted");
    CHECK(backend_calls == 0, "invalid byte I/O reached backend");
}

static void test_read_only_partition_rejects_writes(void)
{
    blockdev_device_t device      = make_parent();
    uint8_t           buffer[512] = {0};

    device.read_only = true;
    backend_calls    = 0;
    CHECK(blockdev_read_sectors(&device, 0, 1, buffer) == EOK, "read-only device rejected a read");
    CHECK(blockdev_write_sectors(&device, 0, 1, buffer) == -EROFS, "read-only sector write accepted");
    CHECK(blockdev_write_bytes(&device, 0, buffer, sizeof(buffer)) == -EROFS, "read-only byte write accepted");
    CHECK(backend_calls == 1, "read-only write reached backend");
}

static void test_linux_disk_and_partition_names_are_strict(void)
{
    uint8_t  drive;
    uint32_t partition;

    CHECK(blockdev_parse_name("sda", &drive, &partition) == EOK && drive == BLKDEV_AHCI_FLAG && partition == 0, "sda parse failed");
    CHECK(blockdev_parse_name("sda15", &drive, &partition) == EOK && drive == BLKDEV_AHCI_FLAG && partition == 15, "sda15 parse failed");
    CHECK(blockdev_parse_name("hdb5", &drive, &partition) == EOK && drive == 1 && partition == 5, "hdb5 parse failed");
    CHECK(blockdev_parse_name("nvme2n1p17", &drive, &partition) == EOK && drive == (BLKDEV_NVME_FLAG | 2) && partition == 17,
          "NVMe partition parse failed");
    CHECK(blockdev_parse_name("sdaa3", &drive, &partition) == EOK && drive == (BLKDEV_AHCI_FLAG | 26) && partition == 3,
          "multi-letter SCSI disk name parse failed");
    char name[16];
    CHECK(blockdev_format_disk_name(name, sizeof(name), 26) == EOK && strcmp(name, "sdaa") == 0, "multi-letter SCSI disk name format failed");
    CHECK(blockdev_parse_name("sr0", &drive, &partition) == EOK && partition == 0, "optical disk parse failed");
    CHECK(blockdev_parse_name("sda0", &drive, &partition) == -EINVAL, "zero partition number accepted");
    CHECK(blockdev_parse_name("sda1junk", &drive, &partition) == -EINVAL, "trailing junk accepted");
    CHECK(blockdev_parse_name("nvme2n0p1", &drive, &partition) == -EINVAL, "zero namespace accepted");
    CHECK(blockdev_parse_name("sr999", &drive, &partition) == -EINVAL, "out-of-range optical disk accepted");
}

int main(void)
{
    test_partition_view_translates_64bit_lba_once();
    test_partition_view_rejects_cross_boundary_io();
    test_byte_io_checks_device_size_before_allocation();
    test_read_only_partition_rejects_writes();
    test_linux_disk_and_partition_names_are_strict();

    if (failures) {
        printf("%d blockdev partition test(s) failed\n", failures);
        return 1;
    }
    printf("PASS blockdev partition boundaries and Linux names\n");
    return 0;
}
