/*
 *
 *      virtgpu_vq.c
 *      VirtIO-GPU virtqueue operations
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/idt.h>
#include <drivers/bus/virtpci.h>
#include <drivers/firmware/apic.h>
#include <drivers/gpu/drm/virtio/virtgpu_drv.h>
#include <drivers/gpu/drm/virtio/virtgpu_vq.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <process/sched.h>

/* One virtio-gpu device is supported by the DRM probe path today. */
static struct virtio_gpu_device *virtgpu_irq_device;

/* Wake command waiters; used-ring reclamation stays in process context. */
INTERRUPT_BEGIN static void virtgpu_irq_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    struct virtio_gpu_device *vgdev = __atomic_load_n(&virtgpu_irq_device, __ATOMIC_ACQUIRE);
    if (vgdev) {
        /* ISR is read-to-clear for INTx/MSI; MSI-X may report zero here. */
        if (vgdev->vp_dev && vgdev->vp_dev->isr) (void)*vgdev->vp_dev->isr;
        (void)wait_queue_wake_all(&vgdev->ctrlq_complete_wait);
        (void)wait_queue_wake_all(&vgdev->cursorq_complete_wait);
    }
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Install one shared completion vector for both virtqueues. */
static int virtgpu_irq_init(struct virtio_gpu_device *vgdev)
{
    struct vp_device *vp = vgdev->vp_dev;
    int               vector;

    if (!vp || !vp->pci_dev || !vp->common) return -ENODEV;
    pci_msi_init(vp->pci_dev);
    vector = pci_enable_msi(vp->pci_dev);
    if (vector < 0) {
        if (pci_enable_msix(vp->pci_dev, 1) != 1) return -ENODEV;
        vector               = pci_irq_vector(vp->pci_dev, 0);
        vgdev->msix_enabled  = true;

        /* queue_msix_vector contains an MSI-X table index, not an IDT vector. */
        vp->common->queue_select      = VIRTGPU_CTRLQ;
        vp->common->queue_msix_vector = 0;
        if (vp->common->queue_msix_vector == UINT16_MAX) goto err_msix;
        vp->common->queue_select      = VIRTGPU_CURSORQ;
        vp->common->queue_msix_vector = 0;
        if (vp->common->queue_msix_vector == UINT16_MAX) goto err_msix;
        vp->common->msix_config = 0;
        if (vp->common->msix_config == UINT16_MAX) goto err_msix;
    }
    if (vector < 0) goto err_irq;

    vgdev->irq_vector = vector;
    __atomic_store_n(&virtgpu_irq_device, vgdev, __ATOMIC_RELEASE);
    register_interrupt_handler((uint16_t)vector, (void *)virtgpu_irq_handler, 0, 0x8e);
    vgdev->irq_enabled = true;
    return 0;

err_msix:
    pci_disable_msix(vp->pci_dev);
    vgdev->msix_enabled = false;
    return -ENODEV;
err_irq:
    if (vgdev->msix_enabled)
        pci_disable_msix(vp->pci_dev);
    else
        pci_disable_msi(vp->pci_dev);
    vgdev->msix_enabled = false;
    return -ENODEV;
}

static void virtgpu_irq_fini(struct virtio_gpu_device *vgdev)
{
    if (!vgdev || !vgdev->irq_enabled) return;
    __atomic_store_n(&virtgpu_irq_device, NULL, __ATOMIC_RELEASE);
    if (vgdev->msix_enabled)
        pci_disable_msix(vgdev->vp_dev->pci_dev);
    else
        pci_disable_msi(vgdev->vp_dev->pci_dev);
    vgdev->irq_enabled  = false;
    vgdev->msix_enabled = false;
    vgdev->irq_vector   = -1;
}

/*
 * Serialize synchronous queue users without keeping interrupts disabled while
 * the host processes a command.  A regular spin_lock() is irq-saving in this
 * kernel; holding it across the used-ring wait delayed the timer and PS/2 IRQs
 * by the full host round-trip and was directly visible as libinput lag.
 */
static void virtgpu_cmd_gate_lock(volatile int *busy, wait_queue_t *wait)
{
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(busy, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;

        /* Driver probing can submit before the first schedulable task exists. */
        if (!__atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
            continue;
        }

        wait_queue_prepare(wait);
        expected = 0;
        if (__atomic_compare_exchange_n(busy, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            wait_queue_cancel(wait);
            return;
        }
        wait_queue_sleep();
    }
}

/* Release the command gate and wake one waiting submitter. */
static void virtgpu_cmd_gate_unlock(volatile int *busy, wait_queue_t *wait)
{
    __atomic_store_n(busy, 0, __ATOMIC_RELEASE);
    (void)wait_queue_wake_one_sync(wait);
}

/* Virtqueue initialisation / teardown */
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

    /* Pre-allocate control-queue staging pages for the hot display path. */
    for (uint32_t i = 0; i < VIRTGPU_CTRLQ_MAX_BATCH; i++) {
        uint64_t cmd_phys  = alloc_frames(1);
        uint64_t resp_phys = alloc_frames(1);
        if (cmd_phys && resp_phys) {
            void *cmd  = phys_to_virt(cmd_phys);
            void *resp = phys_to_virt(resp_phys);
            if (cmd && resp) {
                vgdev->ctrlq_dma_cmd_phys[i]  = cmd_phys;
                vgdev->ctrlq_dma_resp_phys[i] = resp_phys;
                vgdev->ctrlq_dma_cmd[i]       = cmd;
                vgdev->ctrlq_dma_resp[i]      = resp;
            } else {
                free_frames(cmd_phys, 1);
                free_frames(resp_phys, 1);
                vgdev->ctrlq_dma_cmd_phys[i]  = 0;
                vgdev->ctrlq_dma_resp_phys[i] = 0;
                vgdev->ctrlq_dma_cmd[i]       = NULL;
                vgdev->ctrlq_dma_resp[i]      = NULL;
            }
        } else {
            if (cmd_phys) free_frames(cmd_phys, 1);
            if (resp_phys) free_frames(resp_phys, 1);
            vgdev->ctrlq_dma_cmd_phys[i]  = 0;
            vgdev->ctrlq_dma_resp_phys[i] = 0;
            vgdev->ctrlq_dma_cmd[i]       = NULL;
            vgdev->ctrlq_dma_resp[i]      = NULL;
        }
    }

    /* Cursor moves are input-hot; never allocate a frame for each motion. */
    vgdev->cursorq_dma_cmd_phys = alloc_frames(1);
    vgdev->cursorq_dma_cmd      = vgdev->cursorq_dma_cmd_phys ? phys_to_virt(vgdev->cursorq_dma_cmd_phys) : NULL;
    if (!vgdev->cursorq_dma_cmd) {
        if (vgdev->cursorq_dma_cmd_phys) free_frames(vgdev->cursorq_dma_cmd_phys, 1);
        vgdev->cursorq_dma_cmd_phys = 0;
    }

    /* Probe-time commands can poll; runtime commands sleep on this IRQ. */
    if (virtgpu_irq_init(vgdev)) plogk("virtgpu: MSI/MSI-X unavailable; falling back to bounded queue polling.\n");

    plogk("virtgpu: Virtqueues initialised (ctrlq=%d, cursorq=%d, irq=%d)\n", vgdev->ctrlq.num_max, vgdev->cursorq.num_max, vgdev->irq_enabled ? vgdev->irq_vector : -1);
    return 0;
}

/* Tear down the control and cursor virtqueues. */
void virtgpu_vq_fini(struct virtio_gpu_device *vgdev)
{
    if (!vgdev) return;

    /* Stop device DMA before returning queue pages to the frame allocator. */
    if (vgdev->vp_dev) vp_reset_device(vgdev->vp_dev);
    virtgpu_irq_fini(vgdev);
    vp_del_vq(&vgdev->cursorq);
    vp_del_vq(&vgdev->ctrlq);

    for (uint32_t i = 0; i < VIRTGPU_CTRLQ_MAX_BATCH; i++) {
        if (vgdev->ctrlq_dma_cmd_phys[i]) {
            free_frames(vgdev->ctrlq_dma_cmd_phys[i], 1);
            vgdev->ctrlq_dma_cmd_phys[i] = 0;
        }
        if (vgdev->ctrlq_dma_resp_phys[i]) {
            free_frames(vgdev->ctrlq_dma_resp_phys[i], 1);
            vgdev->ctrlq_dma_resp_phys[i] = 0;
        }
        vgdev->ctrlq_dma_cmd[i]  = NULL;
        vgdev->ctrlq_dma_resp[i] = NULL;
    }
    if (vgdev->cursorq_dma_cmd_phys) {
        free_frames(vgdev->cursorq_dma_cmd_phys, 1);
        vgdev->cursorq_dma_cmd_phys = 0;
    }
    vgdev->cursorq_dma_cmd = NULL;
}

/* CPU hint for spin-wait loops - improves performance and memory ordering */
static inline void cpu_relax(void)
{
    __asm__ volatile("pause");
}

/*
 * Queue completions normally arrive through MSI/MSI-X, but a lost or
 * misrouted edge must not stall the desktop until the fatal device timeout.
 * Poll briefly for the common QEMU fast path, then arm a one-tick timed wait
 * so the used ring is rechecked even when no interrupt is delivered.
 */
#define VIRTGPU_FAST_POLL_COUNT     256U
#define VIRTGPU_QUEUE_TIMEOUT_TICKS (5ULL * TIMER_HZ)

static uint64_t virtgpu_next_recheck_deadline(uint64_t overall_deadline)
{
    uint64_t now      = sched_ticks();
    uint64_t deadline = now == UINT64_MAX ? UINT64_MAX : now + 1;
    return deadline < overall_deadline ? deadline : overall_deadline;
}

/* DMA-safe staging buffers for one control-queue command. */
struct virtgpu_dma_command {
        uint64_t cmd_phys;
        uint64_t resp_phys;
        size_t   cmd_pages;
        size_t   resp_pages;
        void    *cmd;
        void    *resp;
};

/* Free the DMA staging buffers allocated for a command batch. */
static void virtgpu_dma_commands_release(struct virtgpu_dma_command *dma, uint32_t count)
{
    if (!dma) return;
    for (uint32_t i = 0; i < count; i++) {
        /* cmd_pages == 0 marks a pooled staging buffer that must not be freed. */
        if (dma[i].cmd_phys && dma[i].cmd_pages) free_frames(dma[i].cmd_phys, dma[i].cmd_pages);
        if (dma[i].resp_phys && dma[i].resp_pages) free_frames(dma[i].resp_phys, dma[i].resp_pages);
    }
}

/* Mark both queues dead and reset the device after a fatal error. */
static void virtgpu_mark_queues_broken(struct virtio_gpu_device *vgdev)
{
    vp_reset_device(vgdev->vp_dev);
    vgdev->ctrlq.broken   = true;
    vgdev->cursorq.broken = true;
}

/* Synchronous control-queue commands */

/*
 * Publish a group of independent descriptor chains, ring the MMIO
 * doorbell once, then reap every response.  The device consumes chains
 * in available-ring order, so transfer/set-scanout/flush sequences keep
 * their protocol ordering without paying one notification and wait per
 * command.
 */
int virtgpu_ctrl_cmd_batch(struct virtio_gpu_device *vgdev, struct virtgpu_vq_command *commands, uint32_t count)
{
    enum {
        CTRL_LOG_NONE,
        CTRL_LOG_NO_DESCRIPTORS,
        CTRL_LOG_QUEUE_ADD,
        CTRL_LOG_TIMEOUT,
        CTRL_LOG_BAD_RESPONSE,
        CTRL_LOG_BAD_FENCE,
    } log_reason
        = CTRL_LOG_NONE;

    struct vp_virtqueue        *vq;
    uint32_t                    submitted = 0;
    uint32_t                    completed = 0;
    uint32_t                    timeout   = 0;
    uint32_t                    fast_polls = 0;
    uint32_t                    len;
    uint32_t                    log_index    = 0;
    uint32_t                    log_type     = 0;
    uint32_t                    log_reply    = 0;
    uint32_t                    log_expected = 0;
    int                         log_error    = 0;
    int                         ret          = 0;
    struct virtgpu_dma_command  dma_stack[VIRTGPU_CTRLQ_MAX_BATCH];
    struct virtgpu_dma_command *dma         = dma_stack;
    bool                        dma_dynamic = false;

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

    memset(dma_stack, 0, sizeof(dma_stack));
    if (count > VIRTGPU_CTRLQ_MAX_BATCH) {
        dma = calloc(count, sizeof(*dma));
        if (!dma) {
            plogk("virtgpu: Ctrl_cmd_batch: dma descriptor allocation failed (count=%u)\n", (unsigned)count);
            return -ENOMEM;
        }
        dma_dynamic = true;
    }
    for (uint32_t i = 0; i < count; i++) {
        /* Small commands reuse the pooled staging pages; oversized ones still allocate transiently. */
        if ((size_t)commands[i].cmd_size <= PAGE_4K_SIZE && (size_t)commands[i].resp_size <= PAGE_4K_SIZE && i < VIRTGPU_CTRLQ_MAX_BATCH && vgdev->ctrlq_dma_cmd[i]) {
            dma[i].cmd        = vgdev->ctrlq_dma_cmd[i];
            dma[i].resp       = vgdev->ctrlq_dma_resp[i];
            dma[i].cmd_phys   = vgdev->ctrlq_dma_cmd_phys[i];
            dma[i].resp_phys  = vgdev->ctrlq_dma_resp_phys[i];
            dma[i].cmd_pages  = 0; /* pooled: never freed */
            dma[i].resp_pages = 0;
            continue;
        }

        dma[i].cmd_pages  = (ALIGN_UP((size_t)commands[i].cmd_size, PAGE_4K_SIZE)) / PAGE_4K_SIZE;
        dma[i].resp_pages = (ALIGN_UP((size_t)commands[i].resp_size, PAGE_4K_SIZE)) / PAGE_4K_SIZE;
        dma[i].cmd_phys   = alloc_frames(dma[i].cmd_pages);
        dma[i].resp_phys  = alloc_frames(dma[i].resp_pages);
        if (!dma[i].cmd_phys || !dma[i].resp_phys) {
            plogk("virtgpu: Ctrl_cmd_batch: frame allocation failed (index=%u)\n", (unsigned)i);
            virtgpu_dma_commands_release(dma, count);
            if (dma_dynamic) free(dma);
            return -ENOMEM;
        }
        dma[i].cmd  = phys_to_virt(dma[i].cmd_phys);
        dma[i].resp = phys_to_virt(dma[i].resp_phys);
    }

    /* The sleepable gate owns the shared staging pages until all replies land. */
    virtgpu_cmd_gate_lock(&vgdev->ctrlq_cmd_busy, &vgdev->ctrlq_cmd_wait);

    if (count > (uint32_t)vq->num_free / 2) {
        log_reason = CTRL_LOG_NO_DESCRIPTORS;
        ret        = -ENOSPC;
        goto out_unlock;
    }

    /*
     * A used descriptor only says that the device returned the transport
     * buffer.  VirtIO-GPU may otherwise complete control commands
     * asynchronously, so fence the tail of a synchronous batch.  Queue order
     * then makes this one fence cover transfer/scanout/flush without adding a
     * round trip to every command in the batch.  Preserve caller-owned fences
     * (notably SUBMIT_3D) and their IDs.
     */
    struct virtio_gpu_ctrl_hdr *tail = (struct virtio_gpu_ctrl_hdr *)commands[count - 1].cmd;
    if (!(tail->flags & VIRTIO_GPU_FLAG_FENCE)) {
        uint64_t rflags = spin_lock_irqsave(&vgdev->fence_lock);
        tail->fence_id  = vgdev->next_fence_id++;
        if (!vgdev->next_fence_id) vgdev->next_fence_id = 1;
        spin_unlock_irqrestore(&vgdev->fence_lock, rflags);
        tail->flags |= VIRTIO_GPU_FLAG_FENCE;
    }

    for (uint32_t i = 0; i < count; i++) {
        memcpy(dma[i].cmd, commands[i].cmd, (size_t)commands[i].cmd_size);
        memset(dma[i].resp, 0, (size_t)commands[i].resp_size);
        ret = virtqueue_add_out_in(vq, dma[i].cmd, commands[i].cmd_size, dma[i].resp, commands[i].resp_size);
        if (ret) {
            log_reason = CTRL_LOG_QUEUE_ADD;
            log_index  = i;
            log_error  = ret;
            break;
        }
        submitted++;
    }

    if (submitted == 0) goto out_unlock;

    /* One doorbell covers every avail entry published above. */
    virtqueue_kick(vq);
    uint64_t overall_deadline = __atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE) ? sched_ticks() + VIRTGPU_QUEUE_TIMEOUT_TICKS : 0;
    while (completed < submitted) {
        if (virtqueue_get_buf(vq, &len)) {
            completed++;
            timeout = 0;
            continue;
        }

        if (vgdev->irq_enabled && __atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE)) {
            if (fast_polls++ < VIRTGPU_FAST_POLL_COUNT) {
                cpu_relax();
                compiler_barrier();
                continue;
            }

            uint64_t now = sched_ticks();
            if (now >= overall_deadline) {
                /* Reap once more before declaring the queue dead. */
                if (virtqueue_get_buf(vq, &len)) {
                    completed++;
                    continue;
                }
                log_reason = CTRL_LOG_TIMEOUT;
                virtgpu_mark_queues_broken(vgdev);
                ret = -EIO;
                goto out_unlock;
            }

            wait_queue_prepare(&vgdev->ctrlq_complete_wait);

            /* Close the used-ring/prepare race before committing the sleep. */
            if (virtqueue_get_buf(vq, &len)) {
                wait_queue_cancel(&vgdev->ctrlq_complete_wait);
                completed++;
                continue;
            }
            (void)wait_queue_wait_timed(&vgdev->ctrlq_complete_wait, virtgpu_next_recheck_deadline(overall_deadline));
            continue;
        }

        if (++timeout > 10000000) {
            /*
             * A completion can race the first empty observation. Reap once
             * more before declaring the device dead so a valid response does
             * not strand its descriptor chain.
             */
            if (virtqueue_get_buf(vq, &len)) {
                completed++;
                timeout = 0;
                continue;
            }
            log_reason = CTRL_LOG_TIMEOUT;
            virtgpu_mark_queues_broken(vgdev);
            ret = -EIO;
            goto out_unlock;
        }
        if ((timeout & 0x3fffU) == 0 && __atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE))
            sched_yield();
        else
            cpu_relax();
        compiler_barrier();
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
            log_reason   = CTRL_LOG_BAD_RESPONSE;
            log_type     = request->type;
            log_reply    = reply->type;
            log_expected = expected;
            ret          = -EIO;
            break;
        }
        if ((request->flags & VIRTIO_GPU_FLAG_FENCE) && (!(reply->flags & VIRTIO_GPU_FLAG_FENCE) || reply->fence_id != request->fence_id)) {
            log_reason = CTRL_LOG_BAD_FENCE;
            log_type   = request->type;
            ret        = -EIO;
            break;
        }
    }
out_unlock:
    virtgpu_cmd_gate_unlock(&vgdev->ctrlq_cmd_busy, &vgdev->ctrlq_cmd_wait);
    virtgpu_dma_commands_release(dma, count);
    if (dma_dynamic) free(dma);

    /*
     * printk ultimately damages and flushes the DRM-backed console.  Never
     * print while owning the command gate: doing so recursively submits another
     * control command and deadlocks the task that must release this gate.
     */
    switch (log_reason) {
        case CTRL_LOG_NO_DESCRIPTORS :
            plogk("virtgpu: Ctrl_cmd_batch: not enough free descriptors (count=%u, num_free=%u)\n", (unsigned)count, (unsigned)vq->num_free);
            break;
        case CTRL_LOG_QUEUE_ADD :
            plogk("virtgpu: Ctrl_cmd_batch: queue add failed (index=%u, err=%d)\n", (unsigned)log_index, log_error);
            break;
        case CTRL_LOG_TIMEOUT :
            plogk("virtgpu: Timed out waiting for GPU command batch (%u/%u complete)\n", completed, submitted);
            break;
        case CTRL_LOG_BAD_RESPONSE :
            plogk("virtgpu: GPU command 0x%04x returned 0x%04x, expected 0x%04x\n", log_type, log_reply, log_expected);
            break;
        case CTRL_LOG_BAD_FENCE :
            plogk("virtgpu: GPU command 0x%04x returned an invalid fence response.\n", log_type);
            break;
        default :
            break;
    }
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

/*
 * Submit one output-only cursor command.  The shared DMA page must not be
 * overwritten until the device has returned its descriptor, so serialize and
 * sleep on the cursor queue IRQ instead of reusing sub-ranges speculatively.
 */
int virtgpu_cursor_cmd(struct virtio_gpu_device *vgdev, void *cmd, int cmd_size)
{
    uint32_t command_type;
    uint32_t timeout = 0;
    uint32_t fast_polls = 0;
    int      ret     = 0;
    size_t   dma_pages;
    uint64_t dma_phys;
    void    *dma_cmd;
    bool     pooled;

    if (!vgdev || !cmd || cmd_size < (int)sizeof(struct virtio_gpu_ctrl_hdr)) {
        plogk("virtgpu: Cursor_cmd: invalid argument (cmd_size=%d)\n", cmd_size);
        return -EINVAL;
    }
    command_type = ((struct virtio_gpu_ctrl_hdr *)cmd)->type;

    pooled = (size_t)cmd_size <= PAGE_4K_SIZE && vgdev->cursorq_dma_cmd;
    if (pooled) {
        dma_pages = 0;
        dma_phys  = vgdev->cursorq_dma_cmd_phys;
        dma_cmd   = vgdev->cursorq_dma_cmd;
    } else {
        dma_pages = ALIGN_UP((size_t)cmd_size, PAGE_4K_SIZE) / PAGE_4K_SIZE;
        dma_phys  = alloc_frames(dma_pages);
        if (!dma_phys) return -ENOMEM;
        dma_cmd = phys_to_virt(dma_phys);
    }

    virtgpu_cmd_gate_lock(&vgdev->cursorq_cmd_busy, &vgdev->cursorq_cmd_wait);
    memcpy(dma_cmd, cmd, (size_t)cmd_size);
    ret = virtqueue_add(&vgdev->cursorq, dma_cmd, cmd_size, 0);
    if (!ret) {
        uint32_t len;
        virtqueue_kick(&vgdev->cursorq);
        uint64_t overall_deadline = __atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE) ? sched_ticks() + VIRTGPU_QUEUE_TIMEOUT_TICKS : 0;
        for (;;) {
            void *completed = virtqueue_get_buf(&vgdev->cursorq, &len);
            if (completed == dma_cmd) break;

            if (vgdev->irq_enabled && __atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE)) {
                if (fast_polls++ < VIRTGPU_FAST_POLL_COUNT) {
                    cpu_relax();
                    compiler_barrier();
                    continue;
                }

                uint64_t now = sched_ticks();
                if (now >= overall_deadline) {
                    if (virtqueue_get_buf(&vgdev->cursorq, &len) == dma_cmd) break;
                    virtgpu_mark_queues_broken(vgdev);
                    ret = -EIO;
                    break;
                }

                wait_queue_prepare(&vgdev->cursorq_complete_wait);

                /* Close the used-ring/prepare race before going to sleep. */
                completed = virtqueue_get_buf(&vgdev->cursorq, &len);
                if (completed == dma_cmd) {
                    wait_queue_cancel(&vgdev->cursorq_complete_wait);
                    break;
                }
                (void)wait_queue_wait_timed(&vgdev->cursorq_complete_wait, virtgpu_next_recheck_deadline(overall_deadline));
                continue;
            }

            if (++timeout > 10000000) {
                if (virtqueue_get_buf(&vgdev->cursorq, &len) == dma_cmd) break;
                virtgpu_mark_queues_broken(vgdev);
                ret = -EIO;
                break;
            }
            if ((timeout & 0x3fffU) == 0 && __atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE))
                sched_yield();
            else
                cpu_relax();
        }
    }
    virtgpu_cmd_gate_unlock(&vgdev->cursorq_cmd_busy, &vgdev->cursorq_cmd_wait);
    if (!pooled) free_frames(dma_phys, dma_pages);

    if (ret == -EIO) plogk("virtgpu: Timed out waiting for cursor command 0x%04x.\n", command_type);
    if (ret && ret != -EIO) plogk("virtgpu: Cursor_cmd: queue add failed (err=%d)\n", ret);
    return ret;
}
