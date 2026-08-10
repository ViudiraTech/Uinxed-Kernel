/*
 *
 *      tpm_vfs.c
 *      TPM character devices (/dev/tpm0, /dev/tpmrm0)
 *
 *      2026/8/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/char/tpm/tpm.h>
#include <drivers/core/device.h>
#include <fs/virtual/devtmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#define TPM_DEV_MAJOR 10
#define TPM0_MINOR    224 // /dev/tpm0
#define TPMRM0_MINOR  225 // /dev/tpmrm0

typedef struct {
        uint8_t command[TPM_BUFSIZE];
        uint8_t response[TPM_BUFSIZE];
        size_t  response_len;
        size_t  read_offset;
        bool    has_response;
} tpm_chardev_state_t;

static int tpm_chardev_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    (void)node;
    (void)flags;
    tpm_chardev_state_t *state = calloc(1, sizeof(*state));
    if (!state) return -ENOMEM;
    *private_data = state;
    return 0;
}

static void tpm_chardev_release(vfs_node_t node, void *private_data)
{
    (void)node;
    free(private_data);
}

static int64_t tpm_chardev_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    tpm_chardev_state_t *state = private_data;
    tpm_device_t        *tpm   = tpm_get_device();
    int                  rc;

    (void)ctx;
    (void)flags;
    (void)offset;
    if (!state || !tpm || !addr || !size || size > TPM_BUFSIZE) return -EINVAL;

    memcpy(state->command, addr, size);
    /*
     * tpm_transmit() sends the command buffer and returns the response
     * length in the same buffer (in-place).
     */
    rc = tpm_transmit(tpm, state->command, TPM_BUFSIZE, size);
    if (rc < 0) return (int64_t)rc;

    state->response_len = (size_t)rc;
    memcpy(state->response, state->command, state->response_len);
    state->has_response = true;
    state->read_offset  = 0;
    return (int64_t)size;
}

static int64_t tpm_chardev_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    tpm_chardev_state_t *state = private_data;
    size_t               actual;

    (void)ctx;
    (void)flags;
    (void)offset;
    if (!state || !addr) return -EINVAL;
    if (!state->has_response || state->read_offset >= state->response_len) return 0;
    actual = size < state->response_len - state->read_offset ? size : state->response_len - state->read_offset;
    memcpy(addr, state->response + state->read_offset, actual);
    state->read_offset += actual;
    return (int64_t)actual;
}

static int tpm_chardev_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *argument)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)request;
    (void)argument;
    return -ENOTTY;
}

void tpm_vfs_init(void)
{
    static const tmpfs_device_ops_t tpm_ops = {
        .open       = tpm_chardev_open,
        .release    = tpm_chardev_release,
        .file_read  = tpm_chardev_read,
        .file_write = tpm_chardev_write,
        .file_ioctl = tpm_chardev_ioctl,
    };

    if (!tpm_get_device()) return;

    if (devtmpfs_register_char_device("/dev/tpm0", MKDEV(TPM_DEV_MAJOR, TPM0_MINOR), MKDEV(TPM_DEV_MAJOR, TPM0_MINOR), file_stream, &tpm_ops)
        == 0) {
        vfs_node_t node = vfs_open("/dev/tpm0");
        if (node) {
            node->mode = 0600;
            vfs_close(node);
        }
    }
    if (devtmpfs_register_char_device("/dev/tpmrm0", MKDEV(TPM_DEV_MAJOR, TPMRM0_MINOR), MKDEV(TPM_DEV_MAJOR, TPMRM0_MINOR), file_stream,
                                      &tpm_ops)
        == 0) {
        vfs_node_t node = vfs_open("/dev/tpmrm0");
        if (node) {
            node->mode = 0600;
            vfs_close(node);
        }
    }
    plogk("tpm: Character devices /dev/tpm0 and /dev/tpmrm0 registered.\n");
}
