/*
 *
 *      partition.c
 *      MBR and GPT partition table support
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/core/partition.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#define MBR_SIGNATURE_OFFSET 510
#define MBR_DISK_ID_OFFSET   440
#define MBR_PARTITION_OFFSET 446
#define MBR_PARTITION_SIZE   16
#define MBR_PARTITION_COUNT  4
#define MBR_PROTECTIVE_TYPE  0xEE
#define GPT_HEADER_SIZE      92
#define GPT_PRIMARY_LBA      1
#define GPT_NAME_CODE_UNITS  36
#define GPT_READ_ONLY        (1ULL << 60)
#define GPT_SIGNATURE        "EFI PART"
#define GPT_REVISION_1_0     0x00010000U

/*
 * GPT on-disk layout summary
 * A GPT header records the location and CRC of both the primary and
 * backup partition-entry arrays. When the primary header is invalid
 * the backup is tried, and a mismatch between the two is reported
 * through the degraded flag.
 */

typedef struct gpt_location {
        uint64_t current_lba;
        uint64_t alternate_lba;
        uint64_t first_usable_lba;
        uint64_t last_usable_lba;
        uint64_t entries_lba;
        uint64_t entries_bytes;
        uint32_t entry_count;
        uint32_t entry_size;
        uint32_t entries_crc;
        uint8_t  disk_guid[16];
        bool     valid;
} gpt_location_t;

/*
 * Little-endian access helpers
 * Partition tables and GPT headers are stored in little-endian byte
 * order regardless of the host, so all fields are decoded through
 * these helpers.
 */

static uint16_t load_le16(const void *pointer)
{
    const uint8_t *bytes = pointer;
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t load_le32(const void *pointer)
{
    const uint8_t *bytes = pointer;
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t load_le64(const void *pointer)
{
    const uint8_t *bytes = pointer;
    return (uint64_t)load_le32(bytes) | ((uint64_t)load_le32(bytes + 4) << 32);
}

static void store_le32(void *pointer, uint32_t value)
{
    uint8_t *bytes = pointer;
    bytes[0]       = (uint8_t)value;
    bytes[1]       = (uint8_t)(value >> 8);
    bytes[2]       = (uint8_t)(value >> 16);
    bytes[3]       = (uint8_t)(value >> 24);
}

static bool guid_is_zero(const uint8_t guid[16])
{
    uint8_t value = 0;

    for (size_t i = 0; i < 16; i++) value |= guid[i];
    return value == 0;
}

/* Check that [start, start+count) lies within the disk's LBA range */
static bool range_valid(uint64_t start, uint64_t count, uint64_t limit)
{
    return count != 0 && start < limit && count <= limit - start;
}

static bool is_power_of_two(uint32_t value)
{
    return value && !(value & (value - 1));
}

static bool mbr_is_extended(uint8_t type)
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/* Read a single sector, rejecting out-of-range LBAs */
static int read_sector(const blockdev_device_t *device, uint64_t lba, uint8_t *sector)
{
    if (lba >= device->sector_count) return -EINVAL;
    return blockdev_read_sectors(device, lba, 1, sector);
}

/* Copy an arbitrary byte range from the disk, crossing sector boundaries */
static int read_disk_bytes(const blockdev_device_t *device, uint64_t byte_offset, void *buffer, size_t size, uint8_t *scratch)
{
    uint8_t *output = buffer;

    while (size) {
        uint64_t lba           = byte_offset / device->sector_size;
        size_t   sector_offset = (size_t)(byte_offset % device->sector_size);
        size_t   copy_size     = device->sector_size - sector_offset;
        int      status;

        if (copy_size > size) copy_size = size;
        status = read_sector(device, lba, scratch);
        if (status != EOK) return status;
        memcpy(output, scratch + sector_offset, copy_size);
        output += copy_size;
        byte_offset += copy_size;
        size -= copy_size;
    }
    return EOK;
}

/* Standard CRC-32 (IEEE 802.3, reflected), used for GPT integrity checks */
static uint32_t crc32_update(uint32_t crc, const uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        crc ^= buffer[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t) - (int32_t)(crc & 1));
    }
    return crc;
}

/* Compute CRC-32 over a byte range on disk without loading it into RAM */
static int disk_crc32(const blockdev_device_t *device, uint64_t byte_offset, uint64_t size, uint32_t *result, uint8_t *scratch)
{
    uint32_t crc = UINT32_MAX;

    while (size) {
        uint64_t lba           = byte_offset / device->sector_size;
        size_t   sector_offset = (size_t)(byte_offset % device->sector_size);
        size_t   count         = device->sector_size - sector_offset;
        int      status;

        if ((uint64_t)count > size) count = (size_t)size;
        status = read_sector(device, lba, scratch);
        if (status != EOK) return status;
        crc = crc32_update(crc, scratch + sector_offset, count);
        byte_offset += count;
        size -= count;
    }
    *result = crc ^ UINT32_MAX;
    return EOK;
}

/*
 * MBR parsing
 * The MBR holds up to four primary entries; a special "extended"
 * type points to a chain of EBR sectors that hold logical partitions
 * numbered from 5 upward.
 */

/* Add a partition to the table, rejecting duplicates, overlaps and overflows. */
static int table_add(partition_table_t *table, const partition_info_t *partition)
{
    if (partition->number > PARTITION_MAX_COUNT) return EOK;
    if (table->count >= PARTITION_MAX_COUNT) return -ENOSPC;

    for (size_t i = 0; i < table->count; i++) {
        const partition_info_t *other = &table->partitions[i];

        if (other->number == partition->number) return -EINVAL;
        if (!guid_is_zero(partition->unique_guid) && !memcmp(other->unique_guid, partition->unique_guid, 16)) return -EINVAL;
        if (other->extended || partition->extended) continue;
        if (partition->start_lba < other->start_lba + other->sector_count && other->start_lba < partition->start_lba + partition->sector_count) return -EINVAL;
    }
    table->partitions[table->count++] = *partition;
    return EOK;
}

/* Validate one MBR entry's status and LBA range. */
static int validate_mbr_entry(const uint8_t *entry, uint64_t disk_sectors, uint64_t *start, uint64_t *count)
{
    uint8_t status = entry[0];

    if (status != 0 && status != 0x80) return -EINVAL;
    *start = load_le32(entry + 8);
    *count = load_le32(entry + 12);
    if (!entry[4] && !*count) return EOK;
    if (!entry[4] || !range_valid(*start, *count, disk_sectors)) return -EINVAL;
    return EOK;
}

/* Walk a chain of extended-boot-record (EBR) sectors for logical partitions */
static int parse_ebr_chain(const blockdev_device_t *device, partition_table_t *table, uint64_t extended_start, uint64_t extended_count, uint32_t *next_number, uint8_t *sector)
{
    uint64_t current = extended_start;
    uint64_t visited[PARTITION_MAX_COUNT];
    size_t   visited_count = 0;

    while (1) {
        const uint8_t *link = NULL;
        int            status;

        if (*next_number > PARTITION_MAX_COUNT) return EOK;
        for (size_t i = 0; i < visited_count; i++)
            if (visited[i] == current) return -ELOOP;
        if (visited_count >= PARTITION_MAX_COUNT) return -ELOOP;
        visited[visited_count++] = current;

        status = read_sector(device, current, sector);
        if (status != EOK) return status;
        if (sector[MBR_SIGNATURE_OFFSET] != 0x55 || sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA) return -EBADMSG;

        for (unsigned int slot = 0; slot < MBR_PARTITION_COUNT; slot++) {
            const uint8_t *entry = sector + MBR_PARTITION_OFFSET + (size_t)slot * MBR_PARTITION_SIZE;
            uint64_t       relative_start;
            uint64_t       count;
            uint8_t        type = entry[4];

            if (!type && !load_le32(entry + 12)) continue;
            if (entry[0] != 0 && entry[0] != 0x80) return -EINVAL;
            relative_start = load_le32(entry + 8);
            count          = load_le32(entry + 12);
            if (!type || !count) return -EINVAL;

            if (mbr_is_extended(type)) {
                if (link) return -EINVAL;
                link = entry;
                continue;
            }

            if (relative_start > UINT64_MAX - current) return -EOVERFLOW;
            uint64_t absolute_start = current + relative_start;
            if (!range_valid(absolute_start, count, device->sector_count)) return -EINVAL;
            if (absolute_start < extended_start || absolute_start - extended_start >= extended_count || count > extended_count - (absolute_start - extended_start)) return -EINVAL;

            partition_info_t partition;
            memset(&partition, 0, sizeof(partition));
            partition.number       = (*next_number)++;
            partition.start_lba    = absolute_start;
            partition.sector_count = count;
            partition.mbr_type     = type;
            partition.bootable     = entry[0] == 0x80;
            status                 = table_add(table, &partition);
            if (status != EOK) return status;
        }

        if (!link) return EOK;
        uint64_t relative = load_le32(link + 8);
        uint64_t count    = load_le32(link + 12);
        if (relative > UINT64_MAX - extended_start) return -EOVERFLOW;
        uint64_t next = extended_start + relative;
        if (!range_valid(next, count, device->sector_count)) return -EINVAL;
        if (next < extended_start || next - extended_start >= extended_count || count > extended_count - (next - extended_start)) return -EINVAL;
        current = next;
    }
}

/* Parse a primary MBR, collecting the four primary entries plus EBR chains */
static int parse_mbr(const blockdev_device_t *device, partition_table_t *table, const uint8_t *mbr, uint8_t *sector)
{
    uint64_t extended_start[MBR_PARTITION_COUNT];
    uint64_t extended_count[MBR_PARTITION_COUNT];
    size_t   extended_entries = 0;
    int      status;

    table->type               = PARTITION_TABLE_MBR;
    table->mbr_disk_signature = load_le32(mbr + MBR_DISK_ID_OFFSET);

    for (unsigned int slot = 0; slot < MBR_PARTITION_COUNT; slot++) {
        const uint8_t *entry = mbr + MBR_PARTITION_OFFSET + (size_t)slot * MBR_PARTITION_SIZE;
        uint64_t       start;
        uint64_t       count;
        uint8_t        type = entry[4];

        status = validate_mbr_entry(entry, device->sector_count, &start, &count);
        if (status != EOK) return status;
        if (!type && !count) continue;

        partition_info_t partition;
        memset(&partition, 0, sizeof(partition));
        partition.number       = slot + 1;
        partition.start_lba    = start;
        partition.sector_count = count;
        partition.mbr_type     = type;
        partition.bootable     = entry[0] == 0x80;

        if (mbr_is_extended(type)) {
            uint64_t bytes         = device->sector_size > 1024 ? device->sector_size : 1024;
            partition.sector_count = bytes / device->sector_size;
            if (partition.sector_count > count) partition.sector_count = count;
            partition.extended               = true;
            extended_start[extended_entries] = start;
            extended_count[extended_entries] = count;
            extended_entries++;
        }
        status = table_add(table, &partition);
        if (status != EOK) return status;
    }

    uint32_t next_number = 5;
    for (size_t i = 0; i < extended_entries; i++) {
        status = parse_ebr_chain(device, table, extended_start[i], extended_count[i], &next_number, sector);
        if (status != EOK) return status;
    }
    return table->count ? EOK : -ENOENT;
}

/* GPT parsing */

/* Read and validate a GPT header at header_lba, filling in location */
static int validate_gpt_header(const blockdev_device_t *device, uint64_t header_lba, gpt_location_t *location, uint8_t *sector, uint8_t *scratch)
{
    uint64_t entry_blocks;
    uint64_t table_end;
    uint32_t stored_crc;
    uint32_t computed_crc;
    int      status;

    memset(location, 0, sizeof(*location));
    status = read_sector(device, header_lba, sector);
    if (status != EOK) return status;
    if (memcmp(sector, GPT_SIGNATURE, 8) != 0) return -EBADMSG;
    if (load_le32(sector + 8) != GPT_REVISION_1_0 || load_le32(sector + 20) != 0) return -EINVAL;

    uint32_t header_size = load_le32(sector + 12);
    if (header_size < GPT_HEADER_SIZE || header_size > device->sector_size) return -EINVAL;
    stored_crc = load_le32(sector + 16);
    store_le32(sector + 16, 0);
    computed_crc = crc32_update(UINT32_MAX, sector, header_size) ^ UINT32_MAX;
    store_le32(sector + 16, stored_crc);
    if (stored_crc != computed_crc) return -EBADMSG;
    for (uint32_t i = header_size; i < device->sector_size; i++)
        if (sector[i] != 0) return -EINVAL;

    location->current_lba      = load_le64(sector + 24);
    location->alternate_lba    = load_le64(sector + 32);
    location->first_usable_lba = load_le64(sector + 40);
    location->last_usable_lba  = load_le64(sector + 48);
    memcpy(location->disk_guid, sector + 56, 16);
    location->entries_lba = load_le64(sector + 72);
    location->entry_count = load_le32(sector + 80);
    location->entry_size  = load_le32(sector + 84);
    location->entries_crc = load_le32(sector + 88);

    if (location->current_lba != header_lba || location->alternate_lba >= device->sector_count || location->alternate_lba == header_lba) return -EINVAL;
    if (location->first_usable_lba > location->last_usable_lba || location->last_usable_lba >= device->sector_count) return -EINVAL;
    if (!location->entry_count || location->entry_size < 128 || location->entry_size % 128 || !is_power_of_two(location->entry_size / 128)) return -EINVAL;
    if (location->entry_count > UINT64_MAX / location->entry_size) return -EOVERFLOW;
    location->entries_bytes = (uint64_t)location->entry_count * location->entry_size;
    entry_blocks            = location->entries_bytes / device->sector_size + (location->entries_bytes % device->sector_size != 0);
    if (!entry_blocks || location->entries_lba >= device->sector_count || entry_blocks > device->sector_count - location->entries_lba) return -EINVAL;
    table_end = location->entries_lba + entry_blocks;

    if (header_lba == GPT_PRIMARY_LBA) {
        if (location->alternate_lba != device->sector_count - 1 || location->entries_lba <= header_lba || table_end > location->first_usable_lba) return -EINVAL;
    } else {
        if (header_lba != device->sector_count - 1 || location->alternate_lba != GPT_PRIMARY_LBA || location->entries_lba <= location->last_usable_lba || table_end > header_lba) return -EINVAL;
    }

    uint64_t byte_offset = location->entries_lba * (uint64_t)device->sector_size;
    status               = disk_crc32(device, byte_offset, location->entries_bytes, &computed_crc, scratch);
    if (status != EOK) return status;
    if (computed_crc != location->entries_crc) return -EBADMSG;
    location->valid = true;
    return EOK;
}

/* Encode a Unicode codepoint as UTF-8, appending it to the output buffer */
static size_t append_utf8(char *output, size_t capacity, size_t used, uint32_t codepoint)
{
    uint8_t encoded[4];
    size_t  count;

    if (codepoint <= 0x7F) {
        encoded[0] = (uint8_t)codepoint;
        count      = 1;
    } else if (codepoint <= 0x7FF) {
        encoded[0] = 0xC0 | (uint8_t)(codepoint >> 6);
        encoded[1] = 0x80 | (uint8_t)(codepoint & 0x3F);
        count      = 2;
    } else if (codepoint <= 0xFFFF) {
        encoded[0] = 0xE0 | (uint8_t)(codepoint >> 12);
        encoded[1] = 0x80 | (uint8_t)((codepoint >> 6) & 0x3F);
        encoded[2] = 0x80 | (uint8_t)(codepoint & 0x3F);
        count      = 3;
    } else {
        encoded[0] = 0xF0 | (uint8_t)(codepoint >> 18);
        encoded[1] = 0x80 | (uint8_t)((codepoint >> 12) & 0x3F);
        encoded[2] = 0x80 | (uint8_t)((codepoint >> 6) & 0x3F);
        encoded[3] = 0x80 | (uint8_t)(codepoint & 0x3F);
        count      = 4;
    }
    if (count >= capacity - used) return used;
    memcpy(output + used, encoded, count);
    return used + count;
}

/* Convert a GPT partition name (UTF-16LE, possibly with surrogate pairs) to UTF-8 */
static void gpt_name_to_utf8(const uint8_t *input, char output[PARTITION_NAME_SIZE])
{
    size_t used = 0;

    for (size_t i = 0; i < GPT_NAME_CODE_UNITS; i++) {
        uint32_t codepoint = load_le16(input + i * 2);

        if (!codepoint) break;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (++i < GPT_NAME_CODE_UNITS) {
                uint32_t low = load_le16(input + i * 2);
                if (low >= 0xDC00 && low <= 0xDFFF)
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                else {
                    codepoint = 0xFFFD;
                    i--;
                }
            } else {
                codepoint = 0xFFFD;
            }
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            codepoint = 0xFFFD;
        }
        used = append_utf8(output, PARTITION_NAME_SIZE, used, codepoint);
    }
    output[used] = '\0';
}

/* Compare the primary and backup GPT headers for consistency. */
static bool gpt_locations_match(const gpt_location_t *primary, const gpt_location_t *backup)
{
    return primary->current_lba == backup->alternate_lba && primary->alternate_lba == backup->current_lba && primary->first_usable_lba == backup->first_usable_lba
           && primary->last_usable_lba == backup->last_usable_lba && primary->entry_count == backup->entry_count && primary->entry_size == backup->entry_size
           && primary->entries_crc == backup->entries_crc && !memcmp(primary->disk_guid, backup->disk_guid, 16);
}

/* Parse the partition-entry array described by a validated GPT header */
static int parse_gpt_entries(const blockdev_device_t *device, partition_table_t *table, const gpt_location_t *location, uint8_t *scratch)
{
    uint8_t entry[128];

    uint32_t exposed_count = location->entry_count < PARTITION_MAX_COUNT ? location->entry_count : PARTITION_MAX_COUNT;
    for (uint32_t index = 0; index < exposed_count; index++) {
        if ((uint64_t)index > (UINT64_MAX - location->entries_lba * device->sector_size) / location->entry_size) return -EOVERFLOW;
        uint64_t offset = location->entries_lba * (uint64_t)device->sector_size + (uint64_t)index * location->entry_size;
        int      status = read_disk_bytes(device, offset, entry, sizeof(entry), scratch);
        if (status != EOK) return status;
        if (guid_is_zero(entry)) continue;
        if (guid_is_zero(entry + 16)) return -EINVAL;

        uint64_t start = load_le64(entry + 32);
        uint64_t end   = load_le64(entry + 40);
        if (start > end || start < location->first_usable_lba || end > location->last_usable_lba) return -EINVAL;

        partition_info_t partition;
        memset(&partition, 0, sizeof(partition));
        partition.number       = index + 1;
        partition.start_lba    = start;
        partition.sector_count = end - start + 1;
        partition.attributes   = load_le64(entry + 48);
        partition.read_only    = (partition.attributes & GPT_READ_ONLY) != 0;
        memcpy(partition.type_guid, entry, 16);
        memcpy(partition.unique_guid, entry + 16, 16);
        gpt_name_to_utf8(entry + 56, partition.name);
        status = table_add(table, &partition);
        if (status != EOK) return status;
    }
    return EOK;
}

static int format_guid(const uint8_t guid[16], char *buffer, size_t size)
{
    if (size < PARTITION_UUID_STRING_SIZE) return -ENOSPC;
    (void)snprintf(buffer, size, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid[3], guid[2], guid[1], guid[0], guid[4], guid[4], guid[7], guid[6], guid[8], guid[9],
                   guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
    return EOK;
}

/* Public API */

/* Detect the partition-table format on a disk and populate a partition_table_t.
 * A protective MBR entry (type 0xEE at LBA 1) selects GPT; otherwise the
 * classic MBR path is taken. GPT headers are validated by CRC before use,
 * falling back to the backup header when the primary is damaged. */
int partition_scan(const blockdev_device_t *device, partition_table_t *table)
{
    gpt_location_t primary;
    gpt_location_t backup;
    uint8_t       *sector;
    uint8_t       *scratch;
    bool           protective   = false;
    bool           legacy_entry = false;
    int            primary_status;
    int            backup_status;
    int            status;

    if (!device || !table || device->sector_count < 1 || device->sector_size < 512 || !is_power_of_two(device->sector_size)) return -EINVAL;
    if (device->sector_count > UINT64_MAX / device->sector_size) return -EOVERFLOW;
    memset(table, 0, sizeof(*table));
    table->partitions = calloc(PARTITION_MAX_COUNT, sizeof(partition_info_t));
    if (!table->partitions) return -ENOMEM;
    sector  = malloc(device->sector_size);
    scratch = malloc(device->sector_size);
    if (!sector || !scratch) {
        free(sector);
        free(scratch);
        partition_table_destroy(table);
        return -ENOMEM;
    }

    status = read_sector(device, 0, sector);
    if (status != EOK) goto fail;
    if (sector[MBR_SIGNATURE_OFFSET] != 0x55 || sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA) {
        status = -ENOENT;
        goto fail;
    }

    for (unsigned int slot = 0; slot < MBR_PARTITION_COUNT; slot++) {
        const uint8_t *entry = sector + MBR_PARTITION_OFFSET + (size_t)slot * MBR_PARTITION_SIZE;
        if (entry[4] == MBR_PROTECTIVE_TYPE && load_le32(entry + 8) == GPT_PRIMARY_LBA) protective = true;
        if (entry[4] != 0 && entry[4] != MBR_PROTECTIVE_TYPE) legacy_entry = true;
    }
    table->hybrid = protective && legacy_entry;

    if (!protective) {
        status = parse_mbr(device, table, sector, scratch);
        free(sector);
        free(scratch);
        if (status != EOK) partition_table_destroy(table);
        return status;
    }

    if (device->sector_count < 3) {
        status = -EBADMSG;
        goto fail;
    }
    primary_status = validate_gpt_header(device, GPT_PRIMARY_LBA, &primary, sector, scratch);
    backup_status  = validate_gpt_header(device, device->sector_count - 1, &backup, sector, scratch);
    if (primary_status != EOK && backup_status != EOK) {
        status = primary_status == -EIO || backup_status == -EIO ? -EIO : -EBADMSG;
        goto fail;
    }

    table->type = PARTITION_TABLE_GPT;
    if (primary_status == EOK) {
        table->degraded = backup_status != EOK || !gpt_locations_match(&primary, &backup);
        memcpy(table->disk_guid, primary.disk_guid, 16);
        status = parse_gpt_entries(device, table, &primary, scratch);
        if (status == EOK) goto success;
        if (backup_status != EOK) goto fail;

        table->count    = 0;
        table->degraded = true;
        memset(table->partitions, 0, PARTITION_MAX_COUNT * sizeof(*table->partitions));
        memcpy(table->disk_guid, backup.disk_guid, 16);
        status = parse_gpt_entries(device, table, &backup, scratch);
        if (status != EOK) goto fail;
    } else {
        table->degraded = true;
        memcpy(table->disk_guid, backup.disk_guid, 16);
        status = parse_gpt_entries(device, table, &backup, scratch);
        if (status != EOK) goto fail;
    }
success:
    free(sector);
    free(scratch);
    return EOK;
fail:
    free(sector);
    free(scratch);
    partition_table_destroy(table);
    return status;
}

/* Release the dynamic partition array owned by a partition table */
void partition_table_destroy(partition_table_t *table)
{
    if (!table) return;
    free(table->partitions);
    memset(table, 0, sizeof(*table));
}

/* Look up a partition by its 1-based number (or GPT entry index) */
const partition_info_t *partition_find(const partition_table_t *table, uint32_t number)
{
    if (!table) return NULL;
    for (size_t i = 0; i < table->count; i++)
        if (table->partitions[i].number == number) return &table->partitions[i];
    return NULL;
}

/* Format a partition UUID: canonical GUID for GPT, "disk-signature-N" for MBR */
int partition_format_uuid(const partition_table_t *table, const partition_info_t *partition, char *buffer, size_t size)
{
    if (!table || !partition || !buffer) return -EINVAL;
    if (table->type == PARTITION_TABLE_GPT) return format_guid(partition->unique_guid, buffer, size);
    if (table->type != PARTITION_TABLE_MBR) return -EINVAL;
    if (size < 12) return -ENOSPC;
    (void)snprintf(buffer, size, "%08x-%02x", table->mbr_disk_signature, partition->number);
    return EOK;
}
