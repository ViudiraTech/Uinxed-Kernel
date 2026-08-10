/*
 *
 *      virtgpu_vq.c
 *      VirtIO-GPU virtqueue operations
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Manages the control and cursor virtqueues.  Provides a synchronous
 *  command submission helper (virtgpu_ctrl_cmd) used by all command-
 *  encoding functions.
 *
 */

#include <arch/common.h>
#include <drivers/bus/virtpci.h>
#include <drivers/gpu/drm/virtio/virtgpu_drv.h>
#include <drivers/gpu/drm/virtio/virtgpu_vq.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>

/* ------------------------------------------------------------------ */
/* Virtqueue initialisation / teardown                                 */
/* ------------------------------------------------------------------ */

int virtgpu_vq_init(struct virtio_gpu_device *vgdev)
{
    struct vp_device *vp = vgdev->vp_dev;
    int               ret;

    ret = vp_setup_vq(vp, VIRTGPU_CTRLQ, VIRTGPU_VQ_NUM, &vgdev->ctrlq);
    if (ret) {
        plogk("virtgpu: Failed to set up control virtqueue: %d\n", ret);
        return ret;
    }

    ret = vp_setup_vq(vp, VIRTGPU_CURSORQ, VIRTGPU_VQ_NUM, &vgdev->cursorq);
    if (ret) {
        plogk("virtgpu: Failed to set up cursor virtqueue: %d\n", ret);
        vp_del_vq(&vgdev->ctrlq);
        return ret;
    }

    plogk("virtgpu: Virtqueues initialised (ctrlq=%d, cursorq=%d)\n", vgdev->ctrlq.num_max, vgdev->cursorq.num_max);
    return 0;
}

void virtgpu_vq_fini(struct virtio_gpu_device *vgdev)
{
    /* Stop device DMA before returning queue pages to the frame allocator. */
    if (vgdev && vgdev->vp_dev) vp_reset_device(vgdev->vp_dev);
    vp_del_vq(&vgdev->cursorq);
    vp_del_vq(&vgdev->ctrlq);
}

/* ------------------------------------------------------------------ */
/* Memory barrier helpers                                              */
/* ------------------------------------------------------------------ */

/* Write memory barrier –ensures all preceding stores are globally visible */
static inline void wmb(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

/* Full memory barrier */
static inline void mb(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

/* CPU hint for spin-wait loops –improves performance and memory ordering */
static inline void cpu_relax(void)
{
    __asm__ volatile("pause");
}

struct virtgpu_dma_command {
        uint64_t cmd_phys;
        uint64_t resp_phys;
        size_t   cmd_pages;
        size_t   resp_pages;
        void    *cmd;
        void    *resp;
};

static void virtgpu_dma_commands_free(struct virtgpu_dma_command *dma, uint32_t count)
{
    if (!dma) return;
    for (uint32_t i = 0; i < count; i++) {
        if (dma[i].cmd_phys) free_frames(dma[i].cmd_phys, dma[i].cmd_pages);
        if (dma[i].resp_phys) free_frames(dma[i].resp_phys, dma[i].resp_pages);
    }
    free(dma);
}

static void virtgpu_mark_queues_broken(struct virtio_gpu_device *vgdev)
{
    vp_reset_device(vgdev->vp_dev);
    vgdev->ctrlq.broken   = true;
    vgdev->cursorq.broken = true;
}

/* ------------------------------------------------------------------ */
/* Synchronous control-queue commands                                  */
/* ------------------------------------------------------------------ */

/*
 * Publish a group of independent descriptor chains, ring the MMIO
 * doorbell once, then reap every response.  The device consumes chains
 * in available-ring order, so transfer/set-scanout/flush sequences keep
 * their protocol ordering without paying one notification and wait per
 * command.
 */
int virtgpu_ctrl_cmd_batch(struct virtio_gpu_device *vgdev, struct virtgpu_vq_command *commands, uint32_t count)
{
    struct vp_virtqueue        *vq;
    uint32_t                    submitted = 0;
    uint32_t                    completed = 0;
    uint32_t                    timeout   = 0;
    uint32_t                    len;
    int                         ret = 0;
    struct virtgpu_dma_command *dma;

    if (!vgdev || !commands || count == 0) {
        plogk("virtgpu: Ctrl_cmd_batch: invalid argument (count=%u)\n", (unsigned)count);
        return -EINVAL;
    }
    vq = &vgdev->ctrlq;
    if (count > (uint32_t)vq->num_max / 2U) {
        plogk("virtgpu: Ctrl_cmd_batch: command count exceeds ring capacity (count=%u, num_max=%u)\n", (unsigned)count, (unsigned)vq->num_max);
        return -ENOSPC;
    }

    for (uint32_t i = 0; i < count; i++)
        if (!commands[i].cmd || !commands[i].resp || commands[i].cmd_size <= 0 || commands[i].resp_size <= 0) {
            plogk("virtgpu: Ctrl_cmd_batch: invalid command slot (index=%u)\n", (unsigned)i);
            return -EINVAL;
        }

    dma = calloc(count, sizeof(*dma));
    if (!dma) {
        plogk("virtgpu: Ctrl_cmd_batch: dma descriptor allocation failed (count=%u)\n", (unsigned)count);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        dma[i].cmd_pages  = (ALIGN_UP((size_t)commands[i].cmd_size, PAGE_4K_SIZE)) / PAGE_4K_SIZE;
        dma[i].resp_pages = (ALIGN_UP((size_t)commands[i].resp_size, PAGE_4K_SIZE)) / PAGE_4K_SIZE;
        dma[i].cmd_phys   = alloc_frames(dma[i].cmd_pages);
        dma[i].resp_phys  = alloc_frames(dma[i].resp_pages);
        if (!dma[i].cmd_phys || !dma[i].resp_phys) {
            plogk("virtgpu: Ctrl_cmd_batch: frame allocation failed (index=%u)\n", (unsigned)i);
            virtgpu_dma_commands_free(dma, count);
            return -ENOMEM;
        }
        dma[i].cmd  = phys_to_virt(dma[i].cmd_phys);
        dma[i].resp = phys_to_virt(dma[i].resp_phys);
    }

    /*
     * Stack-backed command buffers must remain owned by this caller until
     * every response has arrived.  This lock also prevents one CPU from
     * reaping another CPU's completion.
     */
    spin_lock(&vgdev->ctrlq_cmd_lock);

    if (count > (uint32_t)vq->num_free / 2) {
        plogk("virtgpu: Ctrl_cmd_batch: not enough free descriptors (count=%u, num_free=%u)\n", (unsigned)count, (unsigned)vq->num_free);
        ret = -ENOSPC;
        goto out_unlock;
    }

    for (uint32_t i = 0; i < count; i++) {
        struct virtio_gpu_ctrl_hdr *request = (struct virtio_gpu_ctrl_hdr *)commands[i].cmd;

        /*
         * A used descriptor only acknowledges queue consumption.  Fence every
         * synchronous command so its response also proves host processing.
         */
        request->flags |= VIRTIO_GPU_FLAG_FENCE;
        spin_lock(&vgdev->fence_lock);
        request->fence_id = vgdev->next_fence_id++;
        if (request->fence_id == 0) request->fence_id = vgdev->next_fence_id++;
        spin_unlock(&vgdev->fence_lock);
        memcpy(dma[i].cmd, commands[i].cmd, (size_t)commands[i].cmd_size);
        memset(dma[i].resp, 0, (size_t)commands[i].resp_size);
        ret = virtqueue_add_out_in(vq, dma[i].cmd, commands[i].cmd_size, dma[i].resp, commands[i].resp_size);
        if (ret) {
            plogk("virtgpu: Ctrl_cmd_batch: queue add failed (index=%u, err=%d)\n", (unsigned)i, ret);
            break;
        }
        submitted++;
    }

    if (submitted == 0) goto out_unlock;

    /* One doorbell covers every avail entry published above. */
    virtqueue_kick(vq);

    while (completed < submitted) {
        if (virtqueue_get_buf(vq, &len)) {
            completed++;
            timeout = 0;
            continue;
        }

        cpu_relax();
        compiler_barrier();
        if (++timeout > 10000000) {
            plogk("virtgpu: Timed out waiting for GPU command batch (%u/%u complete)\n", completed, submitted);
            virtgpu_mark_queues_broken(vgdev);
            ret = -EIO;
            goto out_unlock;
        }
    }

    for (uint32_t i = 0; i < submitted; i++) memcpy(commands[i].resp, dma[i].resp, (size_t)commands[i].resp_size);
    if (submitted != count) goto out_unlock;

    for (uint32_t i = 0; i < count; i++) {
        struct virtio_gpu_ctrl_hdr *request = (struct virtio_gpu_ctrl_hdr *)commands[i].cmd;
        struct virtio_gpu_ctrl_hdr *reply   = (struct virtio_gpu_ctrl_hdr *)commands[i].resp;
        uint32_t                    expected;

        switch (request->type) {
            case VIRTIO_GPU_CMD_GET_DISPLAY_INFO :
                expected = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
                break;
            case VIRTIO_GPU_CMD_GET_CAPSET_INFO :
                expected = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
                break;
            case VIRTIO_GPU_CMD_GET_CAPSET :
                expected = VIRTIO_GPU_RESP_OK_CAPSET;
                break;
            case VIRTIO_GPU_CMD_GET_EDID :
                expected = VIRTIO_GPU_RESP_OK_EDID;
                break;
            case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID :
                expected = VIRTIO_GPU_RESP_OK_RESOURCE_UUID;
                break;
            case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB :
                expected = VIRTIO_GPU_RESP_OK_MAP_INFO;
                break;
            default :
                expected = VIRTIO_GPU_RESP_OK_NODATA;
                break;
        }

        if (reply->type != expected) {
            plogk("virtgpu: GPU command 0x%04x returned 0x%04x, expected 0x%04x\n", request->type, reply->type, expected);
            ret = -EIO;
            break;
        }
        if (!(reply->flags & VIRTIO_GPU_FLAG_FENCE) || reply->fence_id != request->fence_id) {
            plogk("virtgpu: GPU command 0x%04x returned an invalid fence response.\n", request->type);
            ret = -EIO;
            break;
        }
    }

out_unlock:
    spin_unlock(&vgdev->ctrlq_cmd_lock);
    virtgpu_dma_commands_free(dma, count);
    return ret;
}

/*
 * Send @cmd of @cmd_size bytes to the control queue, wait for a response
 * of @resp_size bytes into @resp.  If @fence_id is non-NULL, the fence
 * ID from the response header is written back.
 *
 * The caller must ensure that the command header is embedded in @cmd and
 * that @resp begins with a struct virtio_gpu_ctrl_hdr.
 */
int virtgpu_ctrl_cmd(struct virtio_gpu_device *vgdev, void *cmd, int cmd_size, void *resp, int resp_size, uint64_t *fence_id)
{
    struct virtgpu_vq_command   command;
    struct virtio_gpu_ctrl_hdr *hdr;
    int                         ret;

    if (cmd_size <= 0 || resp_size <= 0) {
        plogk("virtgpu: Ctrl_cmd: invalid sizes (cmd_size=%d, resp_size=%d)\n", cmd_size, resp_size);
        return -EINVAL;
    }

    command.cmd       = cmd;
    command.cmd_size  = cmd_size;
    command.resp      = resp;
    command.resp_size = resp_size;

    ret = virtgpu_ctrl_cmd_batch(vgdev, &command, 1);
    if (ret) return ret;

    hdr = (struct virtio_gpu_ctrl_hdr *)resp;
    if (fence_id) *fence_id = hdr->fence_id;
    return 0;
}

int virtgpu_cursor_cmd(struct virtio_gpu_device *vgdev, void *cmd, int cmd_size)
{
    uint32_t len, timeout = 0;
    int      ret;
    uint64_t dma_phys;
    size_t   dma_pages;
    void    *dma_cmd;

    if (!vgdev || !cmd || cmd_size < (int)sizeof(struct virtio_gpu_ctrl_hdr)) {
        plogk("virtgpu: Cursor_cmd: invalid argument (cmd_size=%d)\n", cmd_size);
        return -EINVAL;
    }
    dma_pages = ALIGN_UP((size_t)cmd_size, PAGE_4K_SIZE) / PAGE_4K_SIZE;
    dma_phys  = alloc_frames(dma_pages);
    if (!dma_phys) {
        plogk("virtgpu: Cursor_cmd: frame allocation failed (pages=%lu)\n", (unsigned long)dma_pages);
        return -ENOMEM;
    }
    dma_cmd = phys_to_virt(dma_phys);
    memcpy(dma_cmd, cmd, (size_t)cmd_size);
    spin_lock(&vgdev->cursorq_cmd_lock);
    /*
     * Cursorq requests are output-only and have no protocol response.  The
     * preceding resource upload is fenced on controlq; here we only wait for
     * the device to consume the cursor descriptor before returning.
     */
    ret = virtqueue_add(&vgdev->cursorq, dma_cmd, cmd_size, 0);
    if (ret) {
        plogk("virtgpu: Cursor_cmd: queue add failed (err=%d)\n", ret);
        goto out;
    }
    virtqueue_kick(&vgdev->cursorq);
    while (!virtqueue_get_buf(&vgdev->cursorq, &len)) {
        cpu_relax();
        compiler_barrier();
        if (++timeout > 10000000) {
            struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)cmd;
            plogk("virtgpu: Timed out waiting for cursor command 0x%04x consumption.\n", hdr ? hdr->type : 0);
            virtgpu_mark_queues_broken(vgdev);
            ret = -EIO;
            goto out;
        }
    }
out:
    spin_unlock(&vgdev->cursorq_cmd_lock);
    free_frames(dma_phys, dma_pages);
    return ret;
}
