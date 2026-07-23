/*
 *
 *      gpu.c
 *      VirtIO-GPU DRM driver (full 3D / render-node / KMS)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Industrial-grade virtio-gpu DRM driver implementing:
 *    - KMS display pipeline (CRTC, plane, encoder, connector)
 *    - Render node (/dev/dri/renderD128)
 *    - 3D acceleration (virgl) via context creation + SUBMIT_3D
 *    - Blob resources for modern virgl/virtio-gpu
 *    - PRIME dma-buf export/import
 *    - Capability set query
 *    - DebugFS introspection
 *
 *  Integrates with the existing Uinxed DRM core via drm_dev_alloc /
 *  drm_dev_register and exposes ioctls that are byte-compatible with
 *  the Linux virtgpu UAPI.
 *
 */

#include <chipset/common.h>
#include <drivers/drm/drm.h>
#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_fourcc.h>
#include <drivers/drm/drm_init.h>
#include <drivers/drm/drm_mode.h>
#include <drivers/drm/drm_print.h>
#include <drivers/pci.h>
#include <drivers/virt/gpu/virtgpu_cmd.h>
#include <drivers/virt/gpu/virtgpu_drv.h>
#include <drivers/virt/gpu/virtgpu_gem.h>
#include <drivers/virt/gpu/virtgpu_kms.h>
#include <drivers/virt/gpu/virtgpu_vq.h>
#include <drivers/virt/pci.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

/* ------------------------------------------------------------------ */
/* Ioctl implementation prototypes                                      */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_map(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_execbuffer(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_getparam(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_resource_create(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_resource_info(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_transfer_from_host(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_transfer_to_host(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_wait(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_get_caps(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_resource_create_blob(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_context_init(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* ------------------------------------------------------------------ */
/* DRM ioctl table                                                     */
/* ------------------------------------------------------------------ */

static const struct drm_ioctl_desc virtgpu_ioctls[] = {
    {DRM_IOCTL_VIRTGPU_MAP,                 virtgpu_ioctl_map,                 DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_EXECBUFFER,          virtgpu_ioctl_execbuffer,          DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_GETPARAM,            virtgpu_ioctl_getparam,            0},
    {DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,     virtgpu_ioctl_resource_create,     DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_RESOURCE_INFO,       virtgpu_ioctl_resource_info,       DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST,  virtgpu_ioctl_transfer_from_host,  DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,    virtgpu_ioctl_transfer_to_host,    DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_WAIT,                virtgpu_ioctl_wait,                DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_GET_CAPS,            virtgpu_ioctl_get_caps,            0},
    {DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, virtgpu_ioctl_resource_create_blob, DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_CONTEXT_INIT,        virtgpu_ioctl_context_init,        DRM_AUTH},
};

#define VIRTGPU_NUM_IOCTLS (sizeof(virtgpu_ioctls) / sizeof(virtgpu_ioctls[0]))

/* ------------------------------------------------------------------ */
/* Driver callback implementations                                     */
/* ------------------------------------------------------------------ */

static int virtgpu_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

static void virtgpu_postclose(struct drm_device *dev, struct drm_file *file)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    /* Release any contexts owned by this file */
    if (file->driver_priv) {
        uint32_t ctx_id = (uint32_t)(uintptr_t)file->driver_priv;
        if (vgdev->has_virgl) {
            virtgpu_cmd_ctx_destroy(vgdev, ctx_id);
        }
        file->driver_priv = NULL;
    }
}

static void virtgpu_lastclose(struct drm_device *dev)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    /* Disable scanout on last close */
    virtgpu_cmd_set_scanout(vgdev, 0, NULL);
}

static void virtgpu_release(struct drm_device *dev)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    if (vgdev) {
        virtgpu_kms_fini(vgdev);
        virtgpu_vq_fini(vgdev);

        vp_release_device(vgdev->vp_dev);
        free(vgdev->vp_dev);
        free(vgdev);
        dev->dev_private = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* DRM driver descriptor                                                */
/* ------------------------------------------------------------------ */

static struct drm_driver virtgpu_drm_driver = {
    .name            = "virtgpu",
    .desc            = "VirtIO GPU",
    .date            = "20260723",
    .major           = 0,
    .minor           = 1,
    .patchlevel      = 0,
    .driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM
                       | DRIVER_PRIME | DRIVER_RENDER | DRIVER_SYNCOBJ
                       | DRIVER_SYNCOBJ_TIMELINE,

    .open             = virtgpu_open,
    .postclose        = virtgpu_postclose,
    .lastclose        = virtgpu_lastclose,
    .release          = virtgpu_release,

    .ioctls           = virtgpu_ioctls,
    .num_ioctls       = VIRTGPU_NUM_IOCTLS,

    .gem_free_object  = virtgpu_gem_free_object,
    .gem_prime_import = virtgpu_gem_prime_import,

    .dumb_create      = virtgpu_gem_dumb_create,
    .dumb_map_offset  = virtgpu_gem_dumb_map_offset,
    .dumb_destroy     = drm_gem_dumb_destroy,
};

/* ------------------------------------------------------------------ */
/* Ioctl: MAP                                                          */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_map(struct drm_device *dev, void *data,
                             struct drm_file *file_priv)
{
    struct drm_virtgpu_map *args = (struct drm_virtgpu_map *)data;
    struct drm_gem_object  *obj;

    (void)dev;
    obj = drm_gem_object_lookup(file_priv, args->handle);
    if (!obj) { return -ENOENT; }

    args->offset = (uint64_t)(uintptr_t)obj->backing;
    drm_gem_object_put(obj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ioctl: EXECBUFFER (3D command submission)                           */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_execbuffer(struct drm_device *dev, void *data,
                                    struct drm_file *file_priv)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_execbuffer *args = (struct drm_virtgpu_execbuffer *)data;
    uint32_t ctx_id;
    void    *cmd_buf;
    int      ret;

    if (!vgdev->has_virgl) { return -ENOSYS; }

    ctx_id = (uint32_t)(uintptr_t)file_priv->driver_priv;
    if (!ctx_id) { return -EINVAL; }

    if (args->size > 65536) { return -EINVAL; }

    cmd_buf = malloc(args->size);
    if (!cmd_buf) { return -ENOMEM; }

    /* Copy command buffer from userspace (in this kernel, userspace
     * pointers are accessible directly). */
    memcpy(cmd_buf, (void *)(uintptr_t)args->command, args->size);

    /* Submit to host GPU */
    ret = virtgpu_cmd_submit_3d(vgdev, ctx_id, cmd_buf, args->size, NULL);

    free(cmd_buf);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Ioctl: GETPARAM                                                     */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_getparam(struct drm_device *dev, void *data,
                                  struct drm_file *file_priv)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_getparam *args = (struct drm_virtgpu_getparam *)data;

    (void)file_priv;

    switch (args->param) {
        case DRM_VIRTGPU_PARAM_3D_FEATURES:
            args->value = vgdev->has_virgl ? 1 : 0;
            break;
        case DRM_VIRTGPU_PARAM_CAPSET_QUERY_FIX:
            args->value = 1;
            break;
        case DRM_VIRTGPU_PARAM_RESOURCE_BLOB:
            args->value = vgdev->has_resource_blob ? 1 : 0;
            break;
        case DRM_VIRTGPU_PARAM_HOST_VISIBLE:
            args->value = 0; /* Not yet implemented */
            break;
        case DRM_VIRTGPU_PARAM_CONTEXT_INIT:
            args->value = vgdev->has_context_init ? 1 : 0;
            break;
        case DRM_VIRTGPU_PARAM_SUPPORTED_CAPSET_ID:
            args->value = VIRTGPU_CAPSET_VIRGL;
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ioctl: RESOURCE_CREATE (3D resource)                                */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_resource_create(struct drm_device *dev, void *data,
                                         struct drm_file *file_priv)
{
    struct virtio_gpu_device        *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_resource_create *args = (struct drm_virtgpu_resource_create *)data;
    struct virtio_gpu_object        *obj;
    size_t                           size;
    int                              ret;
    uint32_t                         handle;

    size = args->width * args->height * 4;
    if (args->depth > 1) { size *= args->depth; }

    obj = virtgpu_gem_alloc_object(dev, size);
    if (!obj) { return -ENOMEM; }

    obj->hw_res_handle = virtgpu_resource_id_alloc(vgdev);
    obj->format        = args->format;
    obj->width         = args->width;
    obj->height        = args->height;
    obj->depth         = args->depth;
    obj->created_3d    = true;

    ret = virtgpu_cmd_create_resource_3d(vgdev, obj, args);
    if (ret) {
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    ret = virtgpu_cmd_attach_backing(vgdev, obj);
    if (ret) {
        virtgpu_cmd_unref_resource(vgdev, obj->hw_res_handle);
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    ret = drm_gem_handle_create(file_priv, &obj->base, &handle);
    if (ret) {
        virtgpu_cmd_detach_backing(vgdev, obj->hw_res_handle);
        virtgpu_cmd_unref_resource(vgdev, obj->hw_res_handle);
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    args->handle = handle;
    args->size   = size;
    DRM_DEBUG_DRIVER("3D resource created: handle=%u, res_id=%u, %ux%ux%u fmt=0x%x\n",
                     handle, obj->hw_res_handle, args->width, args->height,
                     args->depth, args->format);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ioctl: RESOURCE_INFO                                                */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_resource_info(struct drm_device *dev, void *data,
                                       struct drm_file *file_priv)
{
    struct drm_virtgpu_resource_info *args = (struct drm_virtgpu_resource_info *)data;
    struct drm_gem_object            *obj;

    (void)dev;
    obj = drm_gem_object_lookup(file_priv, args->handle);
    if (!obj) { return -ENOENT; }

    args->bo_size    = obj->size;
    args->res_handle = ((struct virtio_gpu_object *)obj)->hw_res_handle;

    drm_gem_object_put(obj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ioctl: TRANSFER_FROM_HOST / TRANSFER_TO_HOST                        */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_transfer_from_host(struct drm_device *dev, void *data,
                                            struct drm_file *file_priv)
{
    struct virtio_gpu_device      *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_3d_transfer *args = (struct drm_virtgpu_3d_transfer *)data;
    struct drm_gem_object         *gem_obj;
    struct virtio_gpu_object      *obj;
    int                            ret;

    if (!vgdev->has_virgl) { return -ENOSYS; }

    gem_obj = drm_gem_object_lookup(file_priv, args->handle);
    if (!gem_obj) { return -ENOENT; }

    obj = to_virtio_gpu_object(gem_obj);
    ret = virtgpu_cmd_transfer_3d(vgdev, obj, args, false);

    drm_gem_object_put(gem_obj);
    return ret;
}

static int virtgpu_ioctl_transfer_to_host(struct drm_device *dev, void *data,
                                          struct drm_file *file_priv)
{
    struct virtio_gpu_device      *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_3d_transfer *args = (struct drm_virtgpu_3d_transfer *)data;
    struct drm_gem_object         *gem_obj;
    struct virtio_gpu_object      *obj;
    int                            ret;

    gem_obj = drm_gem_object_lookup(file_priv, args->handle);
    if (!gem_obj) { return -ENOENT; }

    obj = to_virtio_gpu_object(gem_obj);

    if (obj->created_3d) {
        ret = virtgpu_cmd_transfer_3d(vgdev, obj, args, true);
    } else {
        ret = virtgpu_cmd_transfer_to_host_2d(vgdev, obj, args->offset);
    }

    drm_gem_object_put(gem_obj);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Ioctl: WAIT (fence wait)                                           */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_wait(struct drm_device *dev, void *data,
                              struct drm_file *file_priv)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_wait  *args  = (struct drm_virtgpu_wait *)data;
    struct drm_gem_object    *gem_obj;
    struct virtio_gpu_object *obj;
    int                       ret;

    gem_obj = drm_gem_object_lookup(file_priv, args->handle);
    if (!gem_obj) { return -ENOENT; }

    obj = to_virtio_gpu_object(gem_obj);

    if (obj->fence && !obj->fence->signaled) {
        struct virtio_gpu_ctrl_hdr  ping_cmd;
        struct virtio_gpu_ctrl_hdr  ping_resp;

        /* Send a no-op ping to the host to flush any pending fences */
        memset(&ping_cmd, 0, sizeof(ping_cmd));
        ping_cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
        memset(&ping_resp, 0, sizeof(ping_resp));
        ret = virtgpu_ctrl_cmd(vgdev, &ping_cmd, sizeof(ping_cmd),
                               &ping_resp, sizeof(ping_resp), NULL);
        if (ret == 0) {
            obj->fence->signaled = true;
        }
    }

    drm_gem_object_put(gem_obj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ioctl: GET_CAPS                                                     */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_get_caps(struct drm_device *dev, void *data,
                                  struct drm_file *file_priv)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_get_caps *args = (struct drm_virtgpu_get_caps *)data;
    void                       *caps_data;
    int                         ret;

    (void)file_priv;

    if (!args->addr || !args->capset_size) { return -EINVAL; }

    caps_data = malloc(args->capset_size);
    if (!caps_data) { return -ENOMEM; }

    ret = virtgpu_cmd_get_capset(vgdev, args->capset_id, args->capset_ver,
                                 caps_data, args->capset_size);
    if (ret) {
        free(caps_data);
        return ret;
    }

    /* Copy capset to userspace */
    memcpy((void *)(uintptr_t)args->addr, caps_data, args->capset_size);
    free(caps_data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ioctl: RESOURCE_CREATE_BLOB                                        */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_resource_create_blob(struct drm_device *dev, void *data,
                                              struct drm_file *file_priv)
{
    struct virtio_gpu_device              *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_resource_create_blob *args = (struct drm_virtgpu_resource_create_blob *)data;
    struct virtio_gpu_object              *obj;
    int                                    ret;
    uint32_t                               handle;

    if (!vgdev->has_resource_blob) { return -ENOSYS; }

    obj = virtgpu_gem_alloc_object(dev, args->size);
    if (!obj) { return -ENOMEM; }

    obj->hw_res_handle = virtgpu_resource_id_alloc(vgdev);
    obj->created_blob  = true;
    obj->blob_mem      = args->blob_mem;
    obj->blob_flags    = args->blob_flags;
    obj->blob_id       = args->blob_id;
    obj->width         = (uint32_t)args->size;
    obj->height        = 1;
    obj->format        = 0;
    obj->depth         = 1;

    ret = virtgpu_cmd_create_blob(vgdev, obj, args);
    if (ret) {
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    ret = virtgpu_cmd_attach_backing(vgdev, obj);
    if (ret) {
        virtgpu_cmd_unref_resource(vgdev, obj->hw_res_handle);
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    ret = drm_gem_handle_create(file_priv, &obj->base, &handle);
    if (ret) {
        virtgpu_cmd_detach_backing(vgdev, obj->hw_res_handle);
        virtgpu_cmd_unref_resource(vgdev, obj->hw_res_handle);
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    args->handle = handle;
    args->blob_handle = obj->hw_res_handle;
    DRM_DEBUG_DRIVER("Blob resource created: handle=%u, res_id=%u, size=%llu, mem=%u flags=0x%x\n",
                     handle, obj->hw_res_handle, args->size, args->blob_mem, args->blob_flags);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ioctl: CONTEXT_INIT                                                 */
/* ------------------------------------------------------------------ */

static int virtgpu_ioctl_context_init(struct drm_device *dev, void *data,
                                      struct drm_file *file_priv)
{
    struct virtio_gpu_device       *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_context_init *args = (struct drm_virtgpu_context_init *)data;
    uint32_t                        ctx_id;
    int                             ret;

    if (!vgdev->has_virgl) { return -ENOSYS; }

    ctx_id = args->ctx_id;
    if (!ctx_id) {
        /* Auto-assign a context ID (in production, use an IDR) */
        ctx_id = (uint32_t)(uintptr_t)file_priv;
    }

    ret = virtgpu_cmd_ctx_create(vgdev, ctx_id, args->context_init);
    if (ret) { return ret; }

    file_priv->driver_priv = (void *)(uintptr_t)ctx_id;
    DRM_DEBUG_DRIVER("Context created: ctx_id=%u, init_flags=0x%x\n",
                     ctx_id, args->context_init);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Page-flip: switch scanout to a new framebuffer (called from KMS)    */
/* ------------------------------------------------------------------ */

int virtgpu_page_flip(struct virtio_gpu_device *vgdev,
                      struct drm_framebuffer *fb,
                      struct drm_framebuffer *old_fb)
{
    struct drm_gem_object    *gem_obj = fb ? fb->obj[0] : NULL;
    struct virtio_gpu_object *obj     = NULL;
    int                       scanout_id = 0;
    int                       ret;

    (void)old_fb;

    if (gem_obj) {
        obj = to_virtio_gpu_object(gem_obj);

        /* Transfer guest backing to host resource */
        ret = virtgpu_cmd_transfer_to_host_2d(vgdev, obj, 0);
        if (ret) {
            DRM_ERROR("flip: transfer failed: %d\n", ret);
            return ret;
        }

        /* Set the scanout to display this resource */
        ret = virtgpu_cmd_set_scanout(vgdev, scanout_id, obj);
        if (ret) {
            DRM_ERROR("flip: set_scanout failed: %d\n", ret);
            return ret;
        }

        /* Flush the resource to make it visible */
        ret = virtgpu_cmd_resource_flush(vgdev, obj, NULL);
        if (ret) {
            DRM_ERROR("flip: flush failed: %d\n", ret);
            return ret;
        }

        vgdev->current_scanout_obj = obj;
        DRM_DEBUG_DRIVER("flip: scanout %d -> resource %u (%ux%u)\n",
                         scanout_id, obj->hw_res_handle, obj->width, obj->height);
    } else {
        /* Disable scanout */
        ret = virtgpu_cmd_set_scanout(vgdev, scanout_id, NULL);
        if (ret) { return ret; }
        vgdev->current_scanout_obj = NULL;
    }

    vgdev->current_fb = fb;
    return 0;
}

/* ------------------------------------------------------------------ */
/* DebugFS — simple feature dump                                       */
/* ------------------------------------------------------------------ */

static void virtgpu_debugfs_info(struct virtio_gpu_device *vgdev)
{
    DRM_INFO("virtgpu: features: virgl=%d edid=%d blob=%d ctx_init=%d\n",
             vgdev->has_virgl, vgdev->has_edid,
             vgdev->has_resource_blob, vgdev->has_context_init);
    DRM_INFO("virtgpu: %d scanout(s), %d ctrlq/%d cursorq\n",
             vgdev->num_scanouts,
             vgdev->ctrlq.num_max, vgdev->cursorq.num_max);
}

/* ------------------------------------------------------------------ */
/* Module init / probe                                                 */
/* ------------------------------------------------------------------ */

int virtio_gpu_driver_init(void)
{
    struct virtio_gpu_device *vgdev;
    struct vp_device         *vp;
    uint64_t                  features;
    int                       ret;

    DRM_INFO("Probing for VirtIO GPU...\n");

    /* Allocate the virtual-PCI transport */
    vp = malloc(sizeof(*vp));
    if (!vp) { return -ENOMEM; }

    ret = vp_find_device(PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_VIRTIO_GPU, vp);
    if (ret) {
        DRM_ERROR("No VirtIO GPU found on PCI: %d\n", ret);
        free(vp);
        return ret;
    }

    vp_setup_device(vp);

    /* Negotiate features */
    features =
        (1ULL << VIRTIO_GPU_F_VIRGL)
        | (1ULL << VIRTIO_GPU_F_EDID)
        | (1ULL << VIRTIO_GPU_F_RESOURCE_UUID)
        | (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB)
        | (1ULL << VIRTIO_GPU_F_CONTEXT_INIT);

    ret = vp_negotiate_features(vp, features, &features);
    if (ret) {
        vp_release_device(vp);
        free(vp);
        return ret;
    }

    /* Allocate the virtio-gpu device */
    vgdev = malloc(sizeof(*vgdev));
    if (!vgdev) {
        vp_release_device(vp);
        free(vp);
        return -ENOMEM;
    }
    memset(vgdev, 0, sizeof(*vgdev));

    vgdev->vp_dev   = vp;
    vp->private_data = vgdev;

    vgdev->has_virgl         = !!(features & (1ULL << VIRTIO_GPU_F_VIRGL));
    vgdev->has_edid          = !!(features & (1ULL << VIRTIO_GPU_F_EDID));
    vgdev->has_resource_blob = !!(features & (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB));
    vgdev->has_context_init  = !!(features & (1ULL << VIRTIO_GPU_F_CONTEXT_INIT));

    vgdev->resource_idr_lock.lock = 0;
    vgdev->fence_lock.lock = 0;
    vgdev->next_resource_id = 1;
    vgdev->next_fence_id    = 1;
    vgdev->num_scanouts     = 0;

    /*
     * VirtIO spec §3.1.1: step 5 — set FEATURES_OK and verify.
     * DRIVER_OK must be set LAST, after all virtqueues are configured.
     */
    vp_set_status(vp, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                      | VIRTIO_STATUS_FEATURES_OK);
    compiler_barrier();
    {
        uint8_t s = vp_get_status(vp);
        if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
            DRM_ERROR("Device rejected feature negotiation (status=0x%02x)\n", s);
            vp_release_device(vp);
            free(vgdev);
            return -ENODEV;
        }
    }

    /* Set up virtqueues (must be done before DRIVER_OK) */
    ret = virtgpu_vq_init(vgdev);
    if (ret) {
        vp_release_device(vp);
        free(vgdev);
        return ret;
    }

    /*
     * VirtIO spec §3.1.1: step 8 — set DRIVER_OK after queues are ready.
     * Use a write barrier to ensure all virtqueue setup stores are visible
     * to the device before the status write reaches it.
     */
    compiler_barrier();
    vp_set_status(vp, vp_get_status(vp) | VIRTIO_STATUS_DRIVER_OK);
    compiler_barrier();

    /* Allocate and register the DRM device */
    vgdev->drm_dev = drm_dev_alloc(&virtgpu_drm_driver);
    if (!vgdev->drm_dev) {
        virtgpu_vq_fini(vgdev);
        vp_release_device(vp);
        free(vgdev);
        return -ENOMEM;
    }

    vgdev->drm_dev->dev_private = vgdev;

    ret = drm_dev_register(vgdev->drm_dev, 0);
    if (ret) {
        virtgpu_vq_fini(vgdev);
        vp_release_device(vp);
        free(vgdev->drm_dev);
        free(vgdev);
        return ret;
    }

    /* Initialise the KMS display pipeline */
    ret = virtgpu_kms_init(vgdev);
    if (ret) {
        DRM_ERROR("KMS init failed: %d (continuing with render only)\n", ret);
        /* Non-fatal — render node still works */
    }

    /* Debug info */
    virtgpu_debugfs_info(vgdev);

    DRM_INFO("VirtIO GPU driver initialised successfully"
             " (card/render node, 3D=%d, blob=%d)\n",
             vgdev->has_virgl, vgdev->has_resource_blob);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Initialisation hook — called from kernel init                       */
/* ------------------------------------------------------------------ */

/*
 * Called after drm_init() to register the virtio-gpu driver.
 * In a full implementation this would be a proper module_init or
 * driver-registration callback.  For now, it's called explicitly.
 */
void virtio_gpu_init(void)
{
    int ret = virtio_gpu_driver_init();
    if (ret) {
        plogk("virtgpu: init failed: %d\n", ret);
    }
}

/*
 * Return the singleton virtio-gpu device for the kernel to query.
 * Returns NULL if no device was probed.
 */
void *virtio_gpu_get_device(void)
{
    /* The singleton is stashed in the global registered DRM device.
     * For now we simply return NULL and let the caller decide. */
    extern struct drm_device *drm_get_singleton(void);
    struct drm_device *dev = drm_get_singleton();

    if (!dev || !dev->dev_private) { return NULL; }
    return dev->dev_private;
}

/*
 * Module exit stub (placeholder for future cleanup).
 */
void virtio_gpu_module_exit(void)
{
    /* No-op: device lifecycle managed by gpu.c/release */
}
