/*
 *
 *      kmsg.c
 *      /dev/kmsg character device
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/char/chrdev.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#define KMSG_MAJOR 1
#define KMSG_MINOR 11

static int64_t kmsg_read(void *ctx, void *private_data, uint64_t flags, void *buffer, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)buffer;
    (void)offset;
    (void)size;
    /*
     * The kernel currently exposes printk output through its consoles.  A
     * nonblocking kmsg reader observes no queued record rather than EOF.
     */
    return -EAGAIN;
}

static int64_t kmsg_write(void *ctx, void *private_data, uint64_t flags, const void *buffer, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)offset;
    if (!buffer && size) return -EINVAL;
    if (!size) return 0;

    char *message = malloc(size + 1);
    if (!message) return -ENOMEM;
    memcpy(message, buffer, size);
    message[size] = '\0';
    /*
     * /dev/kmsg writers may prefix a syslog priority as "<n>".  The console
     * backend has no severity lanes yet, but should not print that framing.
     */
    char *text = message;
    if (text[0] == '<') {
        char *end = strchr(text, '>');
        if (end && (size_t)(end - text) <= 4) text = end + 1;
    }
    printk("%s", text);
    free(message);
    return (int64_t)size;
}

void kmsgdev_init(void)
{
    static const tmpfs_device_ops_t ops = {
        .file_read  = kmsg_read,
        .file_write = kmsg_write,
    };
    (void)cdev_add("", "kmsg", KMSG_MAJOR, KMSG_MINOR, 1, file_stream, 0600, &ops);
}
