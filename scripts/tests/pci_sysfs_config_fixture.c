/* Host-side PCI mock; the runner inserts the actual sysfs config reader. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct kobject {
        int unused;
};
struct attribute {
        const char *name;
        uint16_t    mode;
};
struct bin_attribute {
        struct attribute attr;
        size_t           size;
        ssize_t (*write)(struct kobject *, struct bin_attribute *, char *, int64_t, size_t);
        ssize_t (*read)(struct kobject *, struct bin_attribute *, char *, int64_t, size_t);
};
struct attribute_group {
        struct bin_attribute **bin_attrs;
};
struct device {
        struct kobject kobj;
        void          *driver_data;
};
typedef struct {
        uint8_t bus, slot, func;
} pci_device_t;
typedef struct {
        pci_device_t *device;
        void         *ecam_ptr;
        int           pcie;
        uint8_t       bytes[4096];
} pci_device_cache_t;
typedef struct {
        pci_device_cache_t *parent;
        uint32_t            offset;
} pci_device_reg_t;
/* PRODUCTION PRIVATE TYPE */
static unsigned reads;
static int      pci_find_capability(pci_device_cache_t *cache, int id)
{
    assert(id == 0x10);
    return cache->pcie ? 0x40 : 0;
}
static uint32_t read_pci(pci_device_reg_t reg)
{
    assert((reg.offset & 3) == 0);
    assert(reg.offset + 4 <= sizeof(reg.parent->bytes));
    reads++;
    const uint8_t *p = reg.parent->bytes + reg.offset;
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define EOK              0
#define PCI_COMMAND_PORT 0xcf8
#define PCI_DATA_PORT    0xcfc
static int                 pci_legacy_lock;
static int                 locked;
static uint32_t            selected;
static pci_device_cache_t *legacy_target, *mmio_target;
static unsigned            writes;
static uint64_t            spin_lock_irqsave(int *lock)
{
    (void)lock;
    assert(!locked);
    locked = 1;
    return 0x1234;
}
static void spin_unlock_irqrestore(int *lock, uint64_t flags)
{
    (void)lock;
    assert(locked);
    assert(flags == 0x1234);
    locked = 0;
}
static void hardware_write(pci_device_cache_t *cache, size_t offset, uint32_t value, unsigned size)
{
    assert(offset % size == 0);
    assert(offset + size <= sizeof(cache->bytes));
    writes++;
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = (uint8_t)(value >> (8 * i));
        if (offset + i == 6 || offset + i == 7)
            cache->bytes[offset + i] &= (uint8_t)~byte; /* PCI status is W1C. */
        else
            cache->bytes[offset + i] = byte;
    }
}
static void mmio_write8(volatile void *p, uint8_t v)
{
    hardware_write(mmio_target, (volatile uint8_t *)p - mmio_target->bytes, v, 1);
}
static void mmio_write16(volatile void *p, uint16_t v)
{
    hardware_write(mmio_target, (volatile uint8_t *)p - mmio_target->bytes, v, 2);
}
static void mmio_write32(volatile void *p, uint32_t v)
{
    hardware_write(mmio_target, (volatile uint8_t *)p - mmio_target->bytes, v, 4);
}
static void legacy_write(uint16_t port, uint32_t value, unsigned size)
{
    assert(locked);
    assert((selected & 0xffffff00U) == 0x80021900U);
    assert(port >= PCI_DATA_PORT && port < PCI_DATA_PORT + 4);
    hardware_write(legacy_target, (selected & 0xfc) + (port - PCI_DATA_PORT), value, size);
}
static void outb(uint16_t port, uint8_t value)
{
    legacy_write(port, value, 1);
}
static void outw(uint16_t port, uint16_t value)
{
    legacy_write(port, value, 2);
}
static void outl(uint16_t port, uint32_t value)
{
    if (port == PCI_COMMAND_PORT) {
        assert(locked);
        selected = value;
    } else
        legacy_write(port, value, 4);
}

/* PRODUCTION FUNCTIONS */
/* pciutils lib/sysfs.c requires pread() to return precisely the requested length. */
static int lspci_read_block(struct device *dev, struct bin_attribute *attr, int offset, uint8_t *buffer, int length)
{
    return attr->read(&dev->kobj, attr, (char *)buffer, offset, length) == length;
}
static void write_snapshot(struct device *dev, struct bin_attribute *attr, const char *path)
{
    uint8_t bytes[4096];
    assert(lspci_read_block(dev, attr, 0, bytes, 64));
    assert(lspci_read_block(dev, attr, 64, bytes + 64, 192));
    if (attr->size == 4096)
        assert(lspci_read_block(dev, attr, 256, bytes + 256, 3840));
    else
        assert(!lspci_read_block(dev, attr, 256, bytes + 256, 4));
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(bytes, 1, attr->size, f) == attr->size);
    assert(fclose(f) == 0);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    pci_device_cache_t cache  = {0};
    pci_sysfs_dev_t private   = {.cache = &cache};
    struct device        dev  = {.driver_data = &private};
    struct bin_attribute attr = {.read = pci_config_read, .write = pci_config_write};
    uint8_t              buffer[4096];
    for (size_t i = 0; i < sizeof(cache.bytes); i++) cache.bytes[i] = (uint8_t)(i * 37 + 11);
    assert(pci_config_size(&cache) == 256);
    cache.pcie = 1;
    assert(pci_config_size(&cache) == 256);
    cache.ecam_ptr = &cache;
    cache.pcie     = 0;
    assert(pci_config_size(&cache) == 256);
    cache.pcie = 1;
    assert(pci_config_size(&cache) == 4096);
    for (size_t size = 256; size <= 4096; size *= 16) {
        attr.size = size;
        reads     = 0;
        assert(lspci_read_block(&dev, &attr, 0, buffer, 64));
        assert(reads == 16);
        assert(memcmp(buffer, cache.bytes, 64) == 0);
        for (size_t offset = 0; offset < 8; offset++) {
            memset(buffer, 0, sizeof(buffer));
            assert(attr.read(&dev.kobj, &attr, (char *)buffer, offset, 17) == 17);
            assert(memcmp(buffer, cache.bytes + offset, 17) == 0);
            assert(buffer[17] == 0);
        }
        assert(attr.read(&dev.kobj, &attr, (char *)buffer, 0, SIZE_MAX) == (ssize_t)size);
        assert(memcmp(buffer, cache.bytes, size) == 0);
        assert(attr.read(&dev.kobj, &attr, (char *)buffer, -1, 1) == -EINVAL);
        assert(attr.read(&dev.kobj, &attr, (char *)buffer, INT64_MAX, 1) == 0);
        assert(attr.read(&dev.kobj, &attr, (char *)buffer, size, 1) == 0);
        buffer[1] = 0x5a;
        assert(attr.read(&dev.kobj, &attr, (char *)buffer, size - 1, 8) == 1);
        assert(buffer[0] == cache.bytes[size - 1]);
        assert(buffer[1] == 0x5a);
        reads = 0;
        assert(attr.read(&dev.kobj, &attr, (char *)buffer, 0, 0) == 0);
        assert(reads == 0);
    }
private
    .cache = NULL;
    assert(attr.read(&dev.kobj, &attr, (char *)buffer, 0, 1) == -ENODEV);
private
    .cache = &cache;
    memset(cache.bytes, 0, sizeof(cache.bytes));
    cache.bytes[0]  = 0x34;
    cache.bytes[1]  = 0x12;
    cache.bytes[2]  = 0x11;
    cache.bytes[3]  = 0x11;
    cache.bytes[8]  = 1;
    cache.bytes[11] = 3;
    attr.size       = 256;
    write_snapshot(&dev, &attr, argv[1]);
    cache.bytes[6]    = 0x10;
    cache.bytes[0x34] = 0x40;
    cache.bytes[0x40] = 0x10;
    attr.size         = 4096;
    write_snapshot(&dev, &attr, argv[2]);
    puts("PCI config reads/writes, ECAM/CF8 widths, W1C neighbors and lspci blocks: PASS");
    return 0;
}
