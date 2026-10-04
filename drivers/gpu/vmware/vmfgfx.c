/*
 *
 *      vmfgfx.c
 *      VMware SVGA II display driver (vmfgfx) - register file, FIFO and probe
 *
 *      2026/10/04 By Yinyuan34513
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <boot/limine.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_print.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/gpu/vmware/vmfgfx.h>
#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

#if CONFIG_VMFGFX && CONFIG_DRM

/*
 * The index and value ports of BAR 0 sit SVGA_IO_MUL bytes apart, and the
 * multiplier differs between the legacy and the PCI flavour of the device
 * (1 versus 4).  Probe the layout by writing an identification value and
 * reading it back: only the real value port can round-trip it, everything
 * else returns what the device was reset with.
 */
static int vmfgfx_probe_ports(struct vmfgfx_device *vdev)
{
    static const uint8_t value_offsets[] = {4, 1};

    for (size_t i = 0; i < sizeof(value_offsets) / sizeof(value_offsets[0]); i++) {
        vdev->io_value = value_offsets[i];
        vmfgfx_reg_write(vdev, SVGA_REG_ID, SVGA_ID_1);
        if (vmfgfx_reg_read(vdev, SVGA_REG_ID) == SVGA_ID_1) {
            vmfgfx_reg_write(vdev, SVGA_REG_ID, SVGA_ID_2);
            return 0;
        }
    }
    return -ENODEV;
}

/*
 * Select a register and read it back.  The read is not wasted: BAR 0 is a
 * flush-coalesced I/O window, so only a following access guarantees the
 * write has actually reached the device.
 */
uint32_t vmfgfx_reg_read(struct vmfgfx_device *vdev, uint32_t reg)
{
    if (!vdev || !vdev->io_value) return 0;

    outl((uint16_t)(vdev->io_base + VMFGFX_IO_INDEX), reg);
    return inl((uint16_t)(vdev->io_base + vdev->io_value));
}

/* Select and write a register, then read it back to flush the I/O window. */
void vmfgfx_reg_write(struct vmfgfx_device *vdev, uint32_t reg, uint32_t value)
{
    if (!vdev || !vdev->io_value) return;

    outl((uint16_t)(vdev->io_base + VMFGFX_IO_INDEX), reg);
    outl((uint16_t)(vdev->io_base + vdev->io_value), value);
    (void)inl((uint16_t)(vdev->io_base + vdev->io_value));
}

/* Can the device hold a mode of this size in its video RAM? */
bool vmfgfx_mode_supported(struct vmfgfx_device *vdev, uint32_t width, uint32_t height)
{
    if (!vdev || !width || !height) return false;
    if (width < VMFGFX_MIN_WIDTH || height < VMFGFX_MIN_HEIGHT) return false;
    if (width > vdev->max_width || height > vdev->max_height) return false;
    return (uint64_t)width * VMFGFX_BYTES_PP * height <= vdev->vram_size;
}

/*
 * Pick the mode to start with: keep the geometry of the boot framebuffer so
 * the console switches over without a resolution change, and fall back to
 * the configured default whenever that geometry does not fit the device.
 */
static void vmfgfx_pick_mode(struct vmfgfx_device *vdev, uint32_t *width, uint32_t *height)
{
    struct limine_framebuffer *framebuffer = get_framebuffer();
    uint32_t                   w           = CONFIG_DRM_DEFAULT_WIDTH;
    uint32_t                   h           = CONFIG_DRM_DEFAULT_HEIGHT;

    if (framebuffer && framebuffer->bpp == 32 && vmfgfx_mode_supported(vdev, (uint32_t)framebuffer->width, (uint32_t)framebuffer->height)) {
        w = (uint32_t)framebuffer->width;
        h = (uint32_t)framebuffer->height;
    } else if (!vmfgfx_mode_supported(vdev, w, h)) {
        w = VMFGFX_MIN_WIDTH;
        h = VMFGFX_MIN_HEIGHT;
    }

    *width  = w;
    *height = h;
}

/* Lay out the command ring inside BAR 2.  Every value is a byte offset. */
static int vmfgfx_fifo_init(struct vmfgfx_device *vdev)
{
    uint32_t size = (uint32_t)vdev->fifo_size;

    size &= ~(uint32_t)(sizeof(uint32_t) - 1);
    if (size <= VMFGFX_FIFO_CTRL_BYTES + VMFGFX_FIFO_MIN_COMMANDS) return -ENODEV;

    vdev->fifo[SVGA_FIFO_MIN]  = VMFGFX_FIFO_CTRL_BYTES;
    vdev->fifo[SVGA_FIFO_MAX]  = size;
    vdev->fifo[SVGA_FIFO_NEXT] = VMFGFX_FIFO_CTRL_BYTES;
    vdev->fifo[SVGA_FIFO_STOP] = VMFGFX_FIFO_CTRL_BYTES;
    return 0;
}

/*
 * Push one command into the FIFO, ask the host to run the queue and wait
 * for it to drain.  Running the queue synchronously keeps the ring usable
 * without an interrupt: the next command always starts from an empty ring.
 */
static int vmfgfx_fifo_command(struct vmfgfx_device *vdev, const uint32_t *words, size_t count)
{
    uint32_t min = VMFGFX_FIFO_CTRL_BYTES;
    uint32_t max = (uint32_t)vdev->fifo_size;
    uint32_t next, stop, used, free, off;
    size_t   bytes = count * sizeof(uint32_t);

    if (!vdev || !vdev->fifo || !words || !count || max <= min) return -EINVAL;
    if (bytes > (size_t)(max - min)) return -EINVAL;

    next = vdev->fifo[SVGA_FIFO_NEXT];
    stop = vdev->fifo[SVGA_FIFO_STOP];
    if (next < min || next >= max || stop < min || stop >= max) return -EIO;

    used = (next >= stop) ? (next - stop) : ((max - stop) + (next - min));
    free = (max - min) - used;
    if (free <= bytes) return -EBUSY;

    off = next;
    for (size_t i = 0; i < count; i++) {
        if (off + sizeof(uint32_t) > max) off = min;
        vdev->fifo[off / sizeof(uint32_t)] = words[i];
        off += sizeof(uint32_t);
    }
    if (off >= max) off = min;
    vdev->fifo[SVGA_FIFO_NEXT] = off;

    /* SVGA_REG_SYNC runs the queue inside the write itself. */
    vmfgfx_reg_write(vdev, SVGA_REG_SYNC, 1);

    for (int tries = 0; tries < 100000; tries++) {
        if (vdev->fifo[SVGA_FIFO_STOP] == vdev->fifo[SVGA_FIFO_NEXT]) return 0;
    }
    return -EBUSY;
}

/* Tell the host that a rectangle of the scanout surface has changed. */
static int vmfgfx_update_rect(struct vmfgfx_device *vdev, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint32_t command[5] = {SVGA_CMD_UPDATE, x, y, width, height};

    return vmfgfx_fifo_command(vdev, command, sizeof(command) / sizeof(command[0]));
}

/* Program a new scanout mode; the device keeps scanning out VRAM offset 0. */
int vmfgfx_set_mode(struct vmfgfx_device *vdev, uint32_t width, uint32_t height)
{
    if (!vdev) return -EINVAL;
    if (vdev->mode_width == width && vdev->mode_height == height) return 0;
    if (!vmfgfx_mode_supported(vdev, width, height)) return -EINVAL;

    vmfgfx_reg_write(vdev, SVGA_REG_WIDTH, width);
    vmfgfx_reg_write(vdev, SVGA_REG_HEIGHT, height);
    vmfgfx_reg_write(vdev, SVGA_REG_BITS_PER_PIXEL, VMFGFX_BPP);

    vdev->mode_width  = width;
    vdev->mode_height = height;
    vdev->mode_pitch  = width * VMFGFX_BYTES_PP;
    return 0;
}

/*
 * Copy one framebuffer rectangle into the scanout window at the start of
 * video RAM and let the host know what moved.  Both the source frame and
 * the programmed mode clip the copy.
 */
int vmfgfx_scanout_rect(struct vmfgfx_device *vdev, struct drm_framebuffer *fb, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    const uint8_t *src;
    uint8_t       *dst;
    size_t         row_bytes;
    uint32_t       row;

    if (!vdev || !fb || !fb->obj[0] || !fb->obj[0]->backing || !vdev->vram) return -EINVAL;
    if (fb->format != DRM_FORMAT_XRGB8888 && fb->format != DRM_FORMAT_ARGB8888) return -EINVAL;
    if (fb->pitches[0] < fb->width * VMFGFX_BYTES_PP) return -EINVAL;
    if (x >= fb->width || y >= fb->height || x >= vdev->mode_width || y >= vdev->mode_height) return 0;

    if (width > fb->width - x) width = fb->width - x;
    if (height > fb->height - y) height = fb->height - y;
    if (width > vdev->mode_width - x) width = vdev->mode_width - x;
    if (height > vdev->mode_height - y) height = vdev->mode_height - y;
    if (!width || !height) return 0;

    row_bytes = (size_t)width * VMFGFX_BYTES_PP;
    if ((uint64_t)fb->offsets[0] + (uint64_t)(y + height - 1) * fb->pitches[0] + (uint64_t)(x + width) * VMFGFX_BYTES_PP > fb->obj[0]->size) return -EINVAL;
    if ((uint64_t)(y + height - 1) * vdev->mode_pitch + (uint64_t)(x + width) * VMFGFX_BYTES_PP > vdev->vram_size) return -EINVAL;

    src = (const uint8_t *)fb->obj[0]->backing + fb->offsets[0];
    dst = (uint8_t *)vdev->vram;

    for (row = 0; row < height; row++)
        memcpy(dst + (size_t)(y + row) * vdev->mode_pitch + (size_t)x * VMFGFX_BYTES_PP, src + (size_t)(y + row) * fb->pitches[0] + (size_t)x * VMFGFX_BYTES_PP, row_bytes);

    return vmfgfx_update_rect(vdev, x, y, width, height);
}

/*
 * Define the hardware cursor.  The payload is an SVGA bitmap mask (one bit
 * per pixel, MSB first, one set bit meaning "opaque") followed by a 32-bit
 * image holding the RGB values of the opaque pixels.
 */
int vmfgfx_cursor_set(struct vmfgfx_device *vdev, struct drm_gem_object *gem, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y)
{
    uint32_t       *payload, *mask, *image;
    const uint32_t *pixels;
    uint32_t        words_per_row, mask_words, image_words;
    int             ret;

    if (!vdev) return -EINVAL;

    if (!gem) {
        vmfgfx_reg_write(vdev, SVGA_REG_CURSOR_ON, SVGA_CURSOR_ON_HIDE);
        return 0;
    }
    if (!(vdev->caps & SVGA_CAP_CURSOR)) return -EINVAL;
    if (!gem->backing || !width || !height || width > VMFGFX_CURSOR_MAX || height > VMFGFX_CURSOR_MAX) return -EINVAL;

    image_words = width * height;
    if (gem->size < (size_t)image_words * VMFGFX_BYTES_PP) return -EINVAL;

    words_per_row = (width + 31) / 32;
    mask_words    = words_per_row * height;

    payload = malloc((8 + mask_words + image_words) * sizeof(uint32_t));
    if (!payload) return -ENOMEM;
    memset(payload, 0, (8 + mask_words + image_words) * sizeof(uint32_t));

    mask   = payload + 8;
    image  = mask + mask_words;
    pixels = (const uint32_t *)gem->backing;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = pixels[(size_t)y * width + x]; // dumb buffers pack tightly

            if (!(pixel >> 24)) continue; // fully transparent: no mask bit, no image
            mask[(size_t)y * words_per_row + (x / 32)] |= 1u << (31 - (x % 32));
            image[(size_t)y * width + x] = pixel & 0x00FFFFFFu;
        }
    }

    payload[0] = SVGA_CMD_DEFINE_CURSOR;
    payload[1] = 0; // cursor id
    payload[2] = (uint32_t)hot_x;
    payload[3] = (uint32_t)hot_y;
    payload[4] = width;
    payload[5] = height;
    payload[6] = 0; // reserved
    payload[7] = VMFGFX_BPP;

    ret = vmfgfx_fifo_command(vdev, payload, 8 + mask_words + image_words);
    free(payload);
    if (ret) return ret;

    vmfgfx_reg_write(vdev, SVGA_REG_CURSOR_ON, SVGA_CURSOR_ON_SHOW);
    return 0;
}

/* Move the hardware cursor to a new position. */
int vmfgfx_cursor_move(struct vmfgfx_device *vdev, int32_t x, int32_t y)
{
    if (!vdev) return -EINVAL;

    vmfgfx_reg_write(vdev, SVGA_REG_CURSOR_X, (uint32_t)x);
    vmfgfx_reg_write(vdev, SVGA_REG_CURSOR_Y, (uint32_t)y);
    return 0;
}

/* Open callback: nothing to initialize per client yet. */
static int vmfgfx_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

/* Post-close callback: no per-client state to release. */
static void vmfgfx_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

/* Lastclose callback: the console keeps its scanout until release. */
static void vmfgfx_lastclose(struct drm_device *dev)
{
    (void)dev;
}

/* Tear down the device and release all driver resources. */
static void vmfgfx_release(struct drm_device *dev)
{
    struct vmfgfx_device *vdev = (struct vmfgfx_device *)dev->dev_private;

    if (!vdev) return;

    /* drm_dev_put() already removed the device from the core list. */
    drm_vblank_cleanup(dev);
    drm_mode_config_cleanup(dev);
    vmfgfx_kms_cleanup(vdev);
    free(vdev);
    dev->dev_private = NULL;
}

/* DRM driver descriptor */
static struct drm_driver vmfgfx_drm_driver = {
    .name            = "vmfgfx",
    .desc            = "DRM driver for the VMware SVGA II display adapter",
    .date            = "20261004",
    .major           = 1,
    .minor           = 0,
    .patchlevel      = 0,
    .driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER | DRIVER_SYNCHRONOUS_FLIP,

    .open      = vmfgfx_open,
    .postclose = vmfgfx_postclose,
    .lastclose = vmfgfx_lastclose,
    .release   = vmfgfx_release,

    .gem_prime_import = vmfgfx_gem_prime_import,
    .fb_funcs         = &vmfgfx_fb_funcs,

    /*
     * Objects live in ordinary system memory and are copied into video RAM
     * on every flush, so the generic dumb-buffer helpers are all that is
     * needed to expose a complete GEM/PRIME interface.
     */
    .dumb_create     = drm_gem_dumb_create,
    .dumb_map_offset = drm_gem_dumb_map_offset,
    .dumb_destroy    = drm_gem_dumb_destroy,
};

/* Probe the VMware SVGA II controller and attach a KMS DRM device. */
int vmfgfx_probe(void)
{
    /* pci_device_find() requires its request to outlive the call. */
    static pci_finding_request_t          request;
    volatile pci_finding_response_iter_t *response;
    pci_device_cache_t                   *pci;
    pci_bar_t                             vram_bar, fifo_bar;
    struct vmfgfx_device                 *vdev;
    struct drm_device                    *drm;
    uint32_t                              width, height;
    int                                   ret;

    memset(&request, 0, sizeof(request));
    request.type                     = PCI_FOUND_DEVICE;
    request.req.device_req.vendor_id = VMFGFX_PCI_VENDOR;
    request.req.device_req.device_id = VMFGFX_PCI_DEVICE;

    pci_device_find(&request);
    response = request.response;
    if (!response || response->error != PCI_FINDING_SUCCESS || !response->device) {
        if (response) free((void *)response);
        return -ENODEV; // no VMware SVGA II on this machine
    }
    pci = response->device;
    free((void *)response);

    pci_enable_device(pci, PCI_CMD_IO | PCI_CMD_MEM);

    vdev = malloc(sizeof(*vdev));
    if (!vdev) return -ENOMEM;
    memset(vdev, 0, sizeof(*vdev));
    vdev->pci             = pci;
    vdev->flush_lock.lock = 0;

    vdev->io_base = (uint16_t)pci_get_port_base(pci);
    if (!vdev->io_base) {
        plogk("vmfgfx: device has no I/O BAR, probe skipped.\n");
        free(vdev);
        return -ENODEV;
    }

    ret = vmfgfx_probe_ports(vdev);
    if (ret) {
        plogk("vmfgfx: index/value ports not found at 0x%04x, probe skipped.\n", vdev->io_base);
        free(vdev);
        return ret;
    }

    if (pci_map_bar(pci, 1, &vram_bar) < 0) {
        plogk("vmfgfx: video RAM BAR is not a memory BAR, probe skipped.\n");
        free(vdev);
        return -ENODEV;
    }
    if (pci_map_bar(pci, 2, &fifo_bar) < 0) {
        plogk("vmfgfx: command FIFO BAR is not a memory BAR, probe skipped.\n");
        free(vdev);
        return -ENODEV;
    }

    vdev->vram      = vram_bar.virt;
    vdev->vram_size = vram_bar.size;
    vdev->fifo      = (volatile uint32_t *)fifo_bar.virt;
    vdev->fifo_size = fifo_bar.size;

    vdev->caps       = vmfgfx_reg_read(vdev, SVGA_REG_CAPABILITIES);
    vdev->max_width  = vmfgfx_reg_read(vdev, SVGA_REG_MAX_WIDTH);
    vdev->max_height = vmfgfx_reg_read(vdev, SVGA_REG_MAX_HEIGHT);
    if (!vdev->max_width || !vdev->max_height) {
        vdev->max_width  = VMFGFX_MAX_WIDTH;
        vdev->max_height = VMFGFX_MAX_HEIGHT;
    }

    vmfgfx_pick_mode(vdev, &width, &height);
    if (!vmfgfx_mode_supported(vdev, width, height)) {
        plogk("vmfgfx: no usable mode (video RAM %llu bytes, max %ux%u), probe skipped.\n", vdev->vram_size, vdev->max_width, vdev->max_height);
        free(vdev);
        return -ENODEV;
    }

    /*
     * Bring the device up in one go: stop the current scanout, program the
     * mode, arm the FIFO and only then enable the device - the host runs
     * the FIFO only once SVGA_REG_ENABLE and SVGA_REG_CONFIG_DONE are both
     * set.
     */
    vmfgfx_reg_write(vdev, SVGA_REG_ENABLE, 0);
    vmfgfx_reg_write(vdev, SVGA_REG_BITS_PER_PIXEL, VMFGFX_BPP);
    vmfgfx_reg_write(vdev, SVGA_REG_WIDTH, width);
    vmfgfx_reg_write(vdev, SVGA_REG_HEIGHT, height);

    ret = vmfgfx_fifo_init(vdev);
    if (ret) {
        plogk("vmfgfx: command FIFO too small (%llu bytes), probe skipped.\n", vdev->fifo_size);
        free(vdev);
        return ret;
    }
    vmfgfx_reg_write(vdev, SVGA_REG_CONFIG_DONE, 1);
    vmfgfx_reg_write(vdev, SVGA_REG_ENABLE, 1);

    vdev->mode_width  = width;
    vdev->mode_height = height;
    vdev->mode_pitch  = width * VMFGFX_BYTES_PP;

    plogk("vmfgfx: SVGA ID 0x%08x, caps 0x%x, %ux%u at %llu bytes video RAM\n", vmfgfx_reg_read(vdev, SVGA_REG_ID), vdev->caps, width, height, vdev->vram_size);

    drm = drm_dev_alloc(&vmfgfx_drm_driver);
    if (!drm) {
        DRM_ERROR("failed to allocate DRM device.\n");
        free(vdev);
        return -ENOMEM;
    }
    drm->dev_private = vdev;
    vdev->drm        = drm;

    /*
     * Build the KMS pipeline first, then publish the device: a registered
     * /dev/dri node must already expose its modes and be fully usable.
     */
    ret = vmfgfx_kms_init(vdev);
    if (ret) {
        DRM_ERROR("KMS setup failed: %d\n", ret);
        drm_dev_unregister(drm); // .release() tears down what was built
        return ret;
    }

    ret = drm_dev_register(drm, 0);
    if (ret) {
        DRM_ERROR("failed to register DRM device: %d\n", ret);
        drm_dev_unregister(drm);
        return ret;
    }

    /* Give the console its own framebuffer now that the device is live. */
    ret = vmfgfx_kms_initial_modeset(vdev);
    if (ret) DRM_ERROR("initial modeset skipped: %d\n", ret);

    return 0;
}

#endif
