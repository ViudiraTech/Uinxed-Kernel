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

    DRM_INFO("Virtqueues initialised (ctrlq=%d, cursorq=%d)\n",
             vgdev->ctrlq.num_max, vgdev->cursorq.num_max);
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
/* Synchronous control-queue command                                   */
/* ------------------------------------------------------------------ */

/*
 * Send @cmd of @cmd_size bytes to the control queue, wait for a response
 * of @resp_size bytes into @resp.  If @fence_id is non-NULL, the fence
 * ID from the response header is written back.
 *
 * The caller must ensure that the command header is embedded in @cmd and
 * that @resp begins with a struct virtio_gpu_ctrl_hdr.
 */
int virtgpu_ctrl_cmd(struct virtio_gpu_device *vgdev, void *cmd, int cmd_size,
                     void *resp, int resp_size, uint32_t *fence_id)
{
    struct vp_virtqueue            *vq = &vgdev->ctrlq;
    int                             ret;
    uint32_t                        len;
    struct virtio_gpu_ctrl_hdr     *hdr;

    if (cmd_size <= 0 || resp_size <= 0) { return -EINVAL; }

    /* Submit cmd + resp as a single descriptor chain:
     * - out descriptor: driver writes cmd, device reads it
     * - in descriptor:  device writes resp, driver reads it
     * The device processes both atomically. */
    ret = virtqueue_add_out_in(vq, cmd, cmd_size, resp, resp_size);
    if (ret) {
        DRM_ERROR("virtqueue_add_out_in failed: %d\n", ret);
        return ret;
    }
    virtqueue_kick(vq);

    /* Spin waiting for the device to complete the chain.
     * Getting back either buffer pointer means completion.
     * Use cpu_relax() to improve performance and memory ordering.
     *
     * The counter provides a safety timeout: ~10M iterations (~1-2 s on QEMU)
     * after which we bail out instead of hanging the kernel forever. */
    {
        uint32_t timeout = 0;

        for (;;) {
            void *buf = virtqueue_get_buf(vq, &len);
            if (buf == cmd || buf == resp) { break; }
            cpu_relax();
            compiler_barrier();
            if (++timeout > 10000000) {
                DRM_ERROR("Timed out waiting for GPU response (cmd 0x%04x)\n",
                          ((struct virtio_gpu_ctrl_hdr *)cmd)->type);
                return -EIO;
            }
        }
    }

    hdr = (struct virtio_gpu_ctrl_hdr *)resp;
    if (hdr->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        DRM_ERROR("GPU command 0x%04x failed with 0x%04x\n",
                  ((struct virtio_gpu_ctrl_hdr *)cmd)->type, hdr->type);
        return -EIO;
    }

    if (fence_id) { *fence_id = hdr->fence_id; }
    return 0;
}
