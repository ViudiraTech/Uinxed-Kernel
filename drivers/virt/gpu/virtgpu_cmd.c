/*
 *
 *      virtgpu_cmd.c
 *      VirtIO-GPU command encoding
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Encodes and submits individual virtio-gpu protocol commands.  Each
 *  function allocates a command buffer, fills in the protocol struct,
 *  sends it via virtgpu_ctrl_cmd, and unpacks the response.
 *
 */

#include <drivers/virt/gpu/virtgpu_drv.h>
#include <drivers/virt/gpu/virtgpu_vq.h>
#include <mem/alloc.h>

/* ------------------------------------------------------------------ */
/* Display information                                                 */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_get_display_info(struct virtio_gpu_device *vgdev)
{
    struct virtio_gpu_ctrl_hdr      cmd;
    struct virtio_gpu_resp_display_info resp;
    int                             i, ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    if (ret) { return ret; }

    vgdev->num_scanouts = 0;
    for (i = 0; i < 16; i++) {
        struct virtio_gpu_display_one *d = &resp.pmodes[i];
        if (!d->enabled) { continue; }
        vgdev->scanouts[vgdev->num_scanouts].width   = d->r.width;
        vgdev->scanouts[vgdev->num_scanouts].height  = d->r.height;
        vgdev->scanouts[vgdev->num_scanouts].enabled = true;
        vgdev->scanouts[vgdev->num_scanouts].rect    = d->r;
        vgdev->num_scanouts++;
    }

    DRM_INFO("Display info: %d scanout(s)\n", vgdev->num_scanouts);
    for (i = 0; i < vgdev->num_scanouts && i < 16; i++) {
        DRM_INFO("  scanout[%d]: %ux%u\n", i,
                 vgdev->scanouts[i].width, vgdev->scanouts[i].height);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* EDID                                                                */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_get_edid(struct virtio_gpu_device *vgdev, int scanout_id,
                         void *edid, int *edid_size)
{
    struct virtio_gpu_get_edid cmd;
    struct virtio_gpu_resp_edid resp;
    int ret;

    if (!vgdev->has_edid) { return -ENOSYS; }

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_GET_EDID;
    cmd.scanout_id = scanout_id;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    if (ret) { return ret; }

    if (resp.size > 0 && resp.size <= 1024) {
        memcpy(edid, resp.edid, resp.size);
        *edid_size = resp.size;
    } else {
        *edid_size = 0;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Resource creation — 2D                                              */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_create_resource_2d(struct virtio_gpu_device *vgdev,
                                   struct virtio_gpu_object *obj)
{
    struct virtio_gpu_create_resource_2d cmd;
    struct virtio_gpu_ctrl_hdr          resp;
    int                                 ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.resource_id = obj->hw_res_handle;
    cmd.format      = virtgpu_drm_format_to_virtio(obj->format);
    cmd.width       = obj->width;
    cmd.height      = obj->height;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Resource creation — 3D                                              */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_create_resource_3d(struct virtio_gpu_device *vgdev,
                                   struct virtio_gpu_object *obj,
                                   const struct drm_virtgpu_resource_create *rc)
{
    struct virtio_gpu_resource_create_3d cmd;
    struct virtio_gpu_ctrl_hdr           resp;
    int                                  ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd.resource_id = obj->hw_res_handle;
    cmd.target      = rc->target;
    cmd.format      = virtgpu_drm_format_to_virtio(rc->format);
    cmd.bind        = rc->bind;
    cmd.width       = rc->width;
    cmd.height      = rc->height;
    cmd.depth       = rc->depth;
    cmd.array_size  = rc->array_size;
    cmd.last_level  = rc->last_level;
    cmd.nr_samples  = rc->nr_samples;
    cmd.flags       = rc->flags;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Resource creation — Blob                                           */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_create_blob(struct virtio_gpu_device *vgdev,
                            struct virtio_gpu_object *obj,
                            const struct drm_virtgpu_resource_create_blob *blob)
{
    struct virtio_gpu_resource_create_blob cmd;
    struct virtio_gpu_ctrl_hdr             resp;
    int                                    ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    cmd.resource_id = obj->hw_res_handle;
    cmd.blob_mem    = blob->blob_mem;
    cmd.blob_flags  = blob->blob_flags;
    cmd.nr_entries  = 1;
    cmd.blob_id     = blob->blob_id;
    cmd.size        = blob->size;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Resource destruction                                                */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_unref_resource(struct virtio_gpu_device *vgdev, uint32_t res_id)
{
    struct virtio_gpu_unref    cmd;
    struct virtio_gpu_ctrl_hdr resp;
    int                        ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.resource_id = res_id;
    cmd.padding     = 0;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Backing management (guest memory attach/detach)                     */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_attach_backing(struct virtio_gpu_device *vgdev,
                               struct virtio_gpu_object *obj)
{
    struct virtio_gpu_resource_attach_backing *cmd;
    struct virtio_gpu_ctrl_hdr                 resp;
    struct virtio_gpu_mem_entry               *entries;
    int                                        ret, cmd_size;

    if (!obj->num_entries || !obj->entries) { return -EINVAL; }

    cmd_size = sizeof(*cmd) + obj->num_entries * sizeof(struct virtio_gpu_mem_entry);
    cmd = malloc(cmd_size);
    if (!cmd) { return -ENOMEM; }

    memset(cmd, 0, cmd_size);
    cmd->hdr.type       = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id    = obj->hw_res_handle;
    cmd->nr_entries     = obj->num_entries;

    entries = (struct virtio_gpu_mem_entry *)(cmd + 1);
    memcpy(entries, obj->entries, obj->num_entries * sizeof(struct virtio_gpu_mem_entry));

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, cmd, cmd_size, &resp, sizeof(resp), NULL);

    free(cmd);
    return ret;
}

int virtgpu_cmd_detach_backing(struct virtio_gpu_device *vgdev, uint32_t res_id)
{
    struct virtio_gpu_resource_attach_backing cmd;
    struct virtio_gpu_ctrl_hdr                resp;
    int                                       ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd.resource_id = res_id;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Data transfer (2D)                                                  */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_transfer_to_host_2d(struct virtio_gpu_device *vgdev,
                                    struct virtio_gpu_object *obj,
                                    uint64_t offset)
{
    struct virtio_gpu_transfer_to_host_2d cmd;
    struct virtio_gpu_ctrl_hdr           resp;
    int                                  ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.resource_id = obj->hw_res_handle;
    cmd.offset      = offset;
    cmd.r.x         = 0;
    cmd.r.y         = 0;
    cmd.r.width     = obj->width;
    cmd.r.height    = obj->height;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Data transfer (3D)                                                  */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_transfer_3d(struct virtio_gpu_device *vgdev,
                            struct virtio_gpu_object *obj,
                            const struct drm_virtgpu_3d_transfer *xf,
                            bool to_host)
{
    struct virtio_gpu_transfer_3d cmd;
    struct virtio_gpu_ctrl_hdr   resp;
    int                          ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type     = to_host ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D
                               : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    cmd.resource_id  = obj->hw_res_handle;
    cmd.offset       = xf->offset;
    cmd.level        = xf->level;
    cmd.stride       = xf->stride;
    cmd.layer_stride = xf->layer_stride;
    cmd.box.x        = xf->box.x;
    cmd.box.y        = xf->box.y;
    cmd.box.z        = 0;
    cmd.box.w        = xf->box.w;
    cmd.box.h        = xf->box.h;
    cmd.box.d        = xf->box.d;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Resource flush                                                      */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_resource_flush(struct virtio_gpu_device *vgdev,
                               struct virtio_gpu_object *obj,
                               struct virtio_gpu_rect *rect)
{
    struct virtio_gpu_resource_flush cmd;
    struct virtio_gpu_ctrl_hdr       resp;
    int                              ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.resource_id = obj->hw_res_handle;
    if (rect) {
        cmd.r = *rect;
    } else {
        cmd.r.x      = 0;
        cmd.r.y      = 0;
        cmd.r.width  = obj->width;
        cmd.r.height = obj->height;
    }

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Scanout                                                             */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_set_scanout(struct virtio_gpu_device *vgdev, int scanout_id,
                            struct virtio_gpu_object *obj)
{
    struct virtio_gpu_set_scanout cmd;
    struct virtio_gpu_ctrl_hdr    resp;
    int                           ret;
    struct virtio_gpu_rect        r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.scanout_id = scanout_id;
    cmd.resource_id = obj ? obj->hw_res_handle : 0;

    if (obj) {
        r.x      = 0;
        r.y      = 0;
        r.width  = obj->width;
        r.height = obj->height;
    } else {
        memset(&r, 0, sizeof(r));
    }
    cmd.r = r;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

int virtgpu_cmd_set_scanout_blob(struct virtio_gpu_device *vgdev, int scanout_id,
                                 struct virtio_gpu_object *obj)
{
    /* For blob scanout, use SET_SCANOUT_BLOB if supported.
     * Fall back to SET_SCANOUT if not. */
    return virtgpu_cmd_set_scanout(vgdev, scanout_id, obj);
}

/* ------------------------------------------------------------------ */
/* Context management (3D)                                             */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_ctx_create(struct virtio_gpu_device *vgdev, uint32_t ctx_id,
                           uint32_t context_init)
{
    struct virtio_gpu_ctx_create cmd;
    struct virtio_gpu_ctrl_hdr   resp;
    int                          ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id  = ctx_id;
    cmd.nlen        = 0;
    cmd.context_init = context_init;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

int virtgpu_cmd_ctx_destroy(struct virtio_gpu_device *vgdev, uint32_t ctx_id)
{
    struct virtio_gpu_ctx_destroy cmd;
    struct virtio_gpu_ctrl_hdr    resp;
    int                           ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.hdr.ctx_id = ctx_id;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* 3D command submission                                               */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_submit_3d(struct virtio_gpu_device *vgdev, uint32_t ctx_id,
                          const void *cmd_data, uint32_t size,
                          struct virtio_gpu_fence *fence)
{
    struct virtio_gpu_submit_3d *cmd;
    struct virtio_gpu_ctrl_hdr   resp;
    int                          ret;

    cmd = malloc(sizeof(*cmd) + size);
    if (!cmd) { return -ENOMEM; }

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = ctx_id;
    cmd->hdr.fence_id = fence ? fence->id : 0;
    cmd->size       = size;
    cmd->padding    = 0;

    memcpy((uint8_t *)(cmd + 1), cmd_data, size);

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, cmd, sizeof(*cmd) + size, &resp, sizeof(resp), NULL);

    free(cmd);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Capset query                                                        */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_get_capset_info(struct virtio_gpu_device *vgdev, uint32_t idx,
                                uint32_t *capset_id, uint32_t *max_version,
                                uint32_t *max_size)
{
    struct virtio_gpu_get_capset_info  cmd;
    struct virtio_gpu_resp_capset_info resp;
    int                                ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    cmd.capset_index = idx;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    if (ret) { return ret; }

    *capset_id   = resp.capset_id;
    *max_version = resp.capset_max_version;
    *max_size    = resp.capset_max_size;
    return 0;
}

int virtgpu_cmd_get_capset(struct virtio_gpu_device *vgdev, uint32_t capset_id,
                           uint32_t version, void *data, uint32_t max_size)
{
    struct virtio_gpu_get_capset  cmd;
    struct virtio_gpu_resp_capset *resp;
    int                            ret, resp_size;

    resp_size = sizeof(struct virtio_gpu_resp_capset) + max_size;
    resp = malloc(resp_size);
    if (!resp) { return -ENOMEM; }

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd.capset_id  = capset_id;
    cmd.capset_version = version;

    memset(resp, 0, resp_size);
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), resp, resp_size, NULL);
    if (ret) {
        free(resp);
        return ret;
    }

    memcpy(data, resp->capset_data, max_size);
    free(resp);
    return 0;
}
