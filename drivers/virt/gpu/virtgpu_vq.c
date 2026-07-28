/*
 *
 *      virtgpu_vq.c
 *      VirtIO-GPU virtqueue operations
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Manages the control and cursor virtqueues.  Provides a synchronous
 *  command submission helper (virtgpu_ctrl_cmd) used by all command-
 *  encoding functions.
 *
 */

#include <chipset/common.h>
#include <drivers/virt/gpu/virtgpu_drv.h>
#include <drivers/virt/gpu/virtgpu_vq.h>
#include <drivers/virt/pci.h>
#include <kernel/errno.h>
#include <mem/alloc.h>

/* ------------------------------------------------------------------ */
/* Virtqueue initialisation / teardown                                 */
/* ------------------------------------------------------------------ */

int virtgpu_vq_init(struct virtio_gpu_device *vgdev)
{
    struct vp_device *vp = vgdev->vp_dev;
    int               ret;

    ret = vp_setup_vq(vp, VIRTGPU_CTRLQ, VIRTGPU_VQ_NUM, &vgdev->ctrlq);
    if (ret) {
        DRM_ERROR("Failed to set up control virtqueue: %d\n", ret);
        return ret;
    }

    ret = vp_setup_vq(vp, VIRTGPU_CURSORQ, VIRTGPU_VQ_NUM, &vgdev->cursorq);
    if (ret) {
        DRM_ERROR("Failed to set up cursor virtqueue: %d\n", ret);
        vp_del_vq(&vgdev->ctrlq);
        return ret;
    }

    DRM_INFO("Virtqueues initialised (ctrlq=%d, cursorq=%d)\n", vgdev->ctrlq.num_max, vgdev->cursorq.num_max);
    return 0;
}

void virtgpu_vq_fini(struct virtio_gpu_device *vgdev)
{
    vp_del_vq(&vgdev->cursorq);
    vp_del_vq(&vgdev->ctrlq);
}

/* ------------------------------------------------------------------ */
/* Memory barrier helpers                                              */
/* ------------------------------------------------------------------ */

/* Write memory barrier — ensures all preceding stores are globally visible */
static inline void wmb(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

/* Full memory barrier */
static inline void mb(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

/* CPU hint for spin-wait loops — improves performance and memory ordering */
static inline void cpu_relax(void)
{
    __asm__ volatile("pause");
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
    struct vp_virtqueue *vq;
    uint32_t             submitted = 0;
    uint32_t             completed = 0;
    uint32_t             timeout   = 0;
    uint32_t             len;
    int                  ret = 0;

    if (!vgdev || !commands || count == 0) return -EINVAL;
    vq = &vgdev->ctrlq;

    for (uint32_t i = 0; i < count; i++) {
        if (!commands[i].cmd || !commands[i].resp || commands[i].cmd_size <= 0 || commands[i].resp_size <= 0) return -EINVAL;
    }

    /* Stack-backed command buffers must remain owned by this caller until
     * every response has arrived.  This lock also prevents one CPU from
     * reaping another CPU's completion. */
    spin_lock(&vgdev->ctrlq_cmd_lock);

    if (count > (uint32_t)vq->num_free / 2) {
        ret = -ENOSPC;
        goto out_unlock;
    }

    for (uint32_t i = 0; i < count; i++) {
        struct virtio_gpu_ctrl_hdr *request = (struct virtio_gpu_ctrl_hdr *)commands[i].cmd;

        /* A used descriptor only acknowledges queue consumption.  Fence every
         * synchronous command so its response also proves host processing. */
        request->flags |= VIRTIO_GPU_FLAG_FENCE;
        spin_lock(&vgdev->fence_lock);
        request->fence_id = vgdev->next_fence_id++;
        if (request->fence_id == 0) request->fence_id = vgdev->next_fence_id++;
        spin_unlock(&vgdev->fence_lock);
        ret = virtqueue_add_out_in(vq, commands[i].cmd, commands[i].cmd_size, commands[i].resp, commands[i].resp_size);
        if (ret) break;
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
            DRM_ERROR("Timed out waiting for GPU command batch (%u/%u complete)\n", completed, submitted);
            ret = -EIO;
            goto out_unlock;
        }
    }

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
            DRM_ERROR("GPU command 0x%04x returned 0x%04x, expected 0x%04x\n", request->type, reply->type, expected);
            ret = -EIO;
            break;
        }
        if (!(reply->flags & VIRTIO_GPU_FLAG_FENCE) || reply->fence_id != request->fence_id) {
            DRM_ERROR("GPU command 0x%04x returned an invalid fence response\n", request->type);
            ret = -EIO;
            break;
        }
    }

out_unlock:
    spin_unlock(&vgdev->ctrlq_cmd_lock);
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

    if (cmd_size <= 0 || resp_size <= 0) { return -EINVAL; }

    command.cmd       = cmd;
    command.cmd_size  = cmd_size;
    command.resp      = resp;
    command.resp_size = resp_size;

    ret = virtgpu_ctrl_cmd_batch(vgdev, &command, 1);
    if (ret) return ret;

    hdr = (struct virtio_gpu_ctrl_hdr *)resp;
    if (fence_id) { *fence_id = hdr->fence_id; }
    return 0;
}

int virtgpu_cursor_cmd(struct virtio_gpu_device *vgdev, void *cmd, int cmd_size)
{
    uint32_t len, timeout = 0;
    int      ret;

    if (!vgdev || !cmd || cmd_size < (int)sizeof(struct virtio_gpu_ctrl_hdr)) return -EINVAL;
    spin_lock(&vgdev->cursorq_cmd_lock);
    /* Cursorq requests are output-only and have no protocol response.  The
     * preceding resource upload is fenced on controlq; here we only wait for
     * the device to consume the cursor descriptor before returning. */
    ret = virtqueue_add(&vgdev->cursorq, cmd, cmd_size, 0);
    if (ret) goto out;
    virtqueue_kick(&vgdev->cursorq);
    while (!virtqueue_get_buf(&vgdev->cursorq, &len)) {
        cpu_relax();
        compiler_barrier();
        if (++timeout > 10000000) {
            ret = -EIO;
            goto out;
        }
    }
out:
    spin_unlock(&vgdev->cursorq_cmd_lock);
    return ret;
}
