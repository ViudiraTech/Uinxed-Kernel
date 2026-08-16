/*
 *
 *      mem.c
 *      Memory character devices (/dev/null, zero, full, random, urandom)
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/char/chrdev.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>

#define MEM_MAJOR 1

typedef enum mem_memory_kind {
    MEM_MEM_NULL,
    MEM_MEM_ZERO,
    MEM_MEM_FULL,
    MEM_MEM_RANDOM,
} mem_memory_kind_t;

typedef struct {
        mem_memory_kind_t kind;
} mem_memory_device_t;

static spinlock_t mem_random_lock;
static uint64_t   mem_random_state[4];

/* Return one xoshiro256** word, seeding lazily from rdtsc. */
static uint64_t mem_random_word(void)
{
    spin_lock(&mem_random_lock);
    if (!(mem_random_state[0] | mem_random_state[1] | mem_random_state[2] | mem_random_state[3])) {
        uint64_t seed = rdtsc() ^ (uintptr_t)&mem_random_state ^ 0x9e3779b97f4a7c15ULL;
        for (size_t i = 0; i < 4; i++) {
            seed += 0x9e3779b97f4a7c15ULL;
            uint64_t z          = seed;
            z                   = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z                   = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            mem_random_state[i] = z ^ (z >> 31);
        }
    }

    /* xoshiro256**. */
    uint64_t result = ((mem_random_state[1] * 5) << 7 | (mem_random_state[1] * 5) >> 57) * 9;
    uint64_t t      = mem_random_state[1] << 17;
    mem_random_state[2] ^= mem_random_state[0];
    mem_random_state[3] ^= mem_random_state[1];
    mem_random_state[1] ^= mem_random_state[2];
    mem_random_state[0] ^= mem_random_state[3];
    mem_random_state[2] ^= t;
    mem_random_state[3] = (mem_random_state[3] << 45) | (mem_random_state[3] >> 19);
    spin_unlock(&mem_random_lock);
    return result;
}

/* Read handler for /dev/null, zero, full and random. */
static int64_t mem_read(void *ctx, void *private_data, uint64_t flags, void *buffer, size_t offset, size_t size)
{
    mem_memory_device_t *device = ctx;
    (void)private_data;
    (void)flags;
    (void)offset;
    if (!device || (!buffer && size)) return -EINVAL;
    if (device->kind == MEM_MEM_NULL) return 0;
    if (device->kind == MEM_MEM_ZERO || device->kind == MEM_MEM_FULL) {
        memset(buffer, 0, size);
        return (int64_t)size;
    }

    uint8_t *out = buffer;
    while (size) {
        uint64_t word  = mem_random_word();
        size_t   chunk = size < sizeof(word) ? size : sizeof(word);
        memcpy(out, &word, chunk);
        out += chunk;
        size -= chunk;
    }
    return (int64_t)(out - (uint8_t *)buffer);
}

/* Write handler; /dev/full rejects writes with -ENOSPC. */
static int64_t mem_write(void *ctx, void *private_data, uint64_t flags, const void *buffer, size_t offset, size_t size)
{
    mem_memory_device_t *device = ctx;
    (void)private_data;
    (void)flags;
    (void)buffer;
    (void)offset;
    if (!device) return -EINVAL;
    return device->kind == MEM_MEM_FULL ? -ENOSPC : (int64_t)size;
}

/* User-mode read path for the zero/null devices. */
static int64_t mem_read_user(void *ctx, void *private_data, uint64_t flags, void *buffer, size_t offset, size_t size, struct process *proc)
{
    mem_memory_device_t *device = ctx;
    (void)private_data;
    (void)flags;
    (void)offset;
    if (!device) return -EINVAL;
    if (device->kind == MEM_MEM_NULL) return 0;
    if (device->kind != MEM_MEM_ZERO && device->kind != MEM_MEM_FULL) return -ENOSYS;
    if (clear_user_process(proc, buffer, size)) return -EFAULT;
    return (int64_t)size;
}

/* User-mode write path, delegated to mem_write(). */
static int64_t mem_write_user(void *ctx, void *private_data, uint64_t flags, const void *buffer, size_t offset, size_t size, struct process *proc)
{
    (void)proc;
    return mem_write(ctx, private_data, flags, buffer, offset, size);
}

static mem_memory_device_t mem_devices[] = {
    {.kind = MEM_MEM_NULL}, {.kind = MEM_MEM_ZERO}, {.kind = MEM_MEM_FULL}, {.kind = MEM_MEM_RANDOM}, {.kind = MEM_MEM_RANDOM},
};

static const struct {
        const char *name;
        uint8_t     minor;
        uint16_t    mode;
} mem_nodes[] = {
    {.name = "null",    .minor = 3, .mode = 0666},
    {.name = "zero",    .minor = 5, .mode = 0666},
    {.name = "full",    .minor = 7, .mode = 0666},
    {.name = "random",  .minor = 8, .mode = 0666},
    {.name = "urandom", .minor = 9, .mode = 0666},
};

/* Register the standard memory character devices. */
void memdev_init(void)
{
    for (size_t i = 0; i < sizeof(mem_nodes) / sizeof(mem_nodes[0]); i++) {
        tmpfs_device_ops_t ops = {
            .file_read       = mem_read,
            .file_write      = mem_write,
            .file_write_user = mem_write_user,
            .ctx             = &mem_devices[i],
        };
        if (mem_devices[i].kind != MEM_MEM_RANDOM) ops.file_read_user = mem_read_user;
        (void)cdev_add("", mem_nodes[i].name, MEM_MAJOR, mem_nodes[i].minor, 1, file_stream, mem_nodes[i].mode, &ops);
    }
}
