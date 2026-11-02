/*
 *
 *      bochs.c
 *      bochs dispi vga DRM driver
 *
 *      2026/11/1 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <drivers/bus/pci.h>
#include <drivers/gpu/drm/bochs/bochs_drv.h>
#include <drivers/gpu/drm/drm_edid.h>
#include <drivers/gpu/drm/drm_init.h>
#include <libs/util/byteorder.h>
#include <mem/alloc.h>
#include <mem/page.h>

#if CONFIG_BOCHS && CONFIG_DRM

/* Offsets inside the register BAR (BAR2). */
#    define BOCHS_MMIO_VGA_OFFSET   0x400 // legacy VGA ports 0x3c0..0x3df
#    define BOCHS_MMIO_DISPI_OFFSET 0x500 // VBE dispi index/data pairs
#    define BOCHS_MMIO_QEXT_SIZE    0x600
#    define BOCHS_MMIO_QEXT_ENDIAN  0x604
#    define BOCHS_MMIO_EDID_LIMIT   0x400 // EDID blobs live below the VGA window

/* The register window has to cover both the VGA and the DISPI blocks. */
#    define BOCHS_MMIO_MIN_SPAN (BOCHS_MMIO_DISPI_OFFSET + 0x20)

/* Values the qext endianness register expects. */
#    define BOCHS_QEXT_LITTLE_ENDIAN 0x1e1e1e1e
#    define BOCHS_QEXT_BIG_ENDIAN    0xbebebebe

/* Legacy VGA registers used to blank the display. */
#    define BOCHS_VGA_MIS_W     0x3c2
#    define BOCHS_VGA_IS1_RC    0x3da
#    define BOCHS_VGA_ATT_W     0x3c0
#    define BOCHS_VGA_MIS_COLOR 0x01

/* Device whose framebuffer the kernel console draws into. */
static struct bochs_device *bochs_console_dev;

/* Embedded-state accessor used by both driver translation units. */
struct bochs_device *to_bochs_device(const struct drm_device *dev)
{
    return (struct bochs_device *)dev->dev_private;
}

/* The register window is preferred; the legacy dispi I/O ports are the fallback. */
static bool bochs_uses_mmio(const struct bochs_device *bdev)
{
    return bdev->mmio != NULL;
}

/* Write a legacy VGA register through MMIO or the I/O port. */
static void bochs_vga_writeb(struct bochs_device *bdev, uint16_t ioport, uint8_t val)
{
    if (ioport < 0x3c0 || ioport > 0x3df) return;
    if (bochs_uses_mmio(bdev)) {
        mmio_write8(bdev->mmio + (ioport - 0x3c0 + BOCHS_MMIO_VGA_OFFSET), val);
    } else {
        outb(ioport, val);
    }
}

/* Read a legacy VGA register through MMIO or the I/O port. */
static uint8_t bochs_vga_readb(struct bochs_device *bdev, uint16_t ioport)
{
    if (ioport < 0x3c0 || ioport > 0x3df) return 0xff;
    if (bochs_uses_mmio(bdev)) return mmio_read8(bdev->mmio + (ioport - 0x3c0 + BOCHS_MMIO_VGA_OFFSET));
    return inb(ioport);
}

/* Read a bochs dispi register through MMIO or the I/O port pair. */
static uint16_t bochs_dispi_read(struct bochs_device *bdev, uint16_t reg)
{
    if (bochs_uses_mmio(bdev)) return mmio_read16(bdev->mmio + BOCHS_MMIO_DISPI_OFFSET + ((uint32_t)reg << 1));
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    return inw(VBE_DISPI_IOPORT_DATA);
}

/* Write a bochs dispi register through MMIO or the I/O port pair. */
static void bochs_dispi_write(struct bochs_device *bdev, uint16_t reg, uint16_t val)
{
    if (bochs_uses_mmio(bdev)) {
        mmio_write16(bdev->mmio + BOCHS_MMIO_DISPI_OFFSET + ((uint32_t)reg << 1), val);
        return;
    }
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

/* Byte-swap scanout words through qext; only BGRX8888 needs the swap. */
static void bochs_hw_set_endian(struct bochs_device *bdev, bool big_endian)
{
    if (bdev->qext_size < BOCHS_QEXT_MIN_SIZE) return;
    mmio_write32(bdev->mmio + BOCHS_MMIO_QEXT_ENDIAN, big_endian ? BOCHS_QEXT_BIG_ENDIAN : BOCHS_QEXT_LITTLE_ENDIAN);
}

/* Program the scanout geometry.  The mode always follows the framebuffer. */
void bochs_hw_setmode(struct bochs_device *bdev, const struct drm_framebuffer *fb)
{
    uint32_t yres_virtual;

    if (!bdev || !fb || !fb->width || !fb->height || !fb->pitches[0]) return;
    bdev->xres   = (uint16_t)fb->width;
    bdev->yres   = (uint16_t)fb->height;
    bdev->bpp    = BOCHS_BPP;
    bdev->stride = fb->pitches[0];
    yres_virtual = (uint32_t)(bdev->fb_size / bdev->stride);

    if (yres_virtual > UINT16_MAX) yres_virtual = UINT16_MAX;
    bdev->yres_virtual = (uint16_t)yres_virtual;

    /* The adapter only accepts geometry changes with the display disabled. */
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_BPP, bdev->bpp);
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_XRES, bdev->xres);
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_YRES, bdev->yres);
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_BANK, 0);
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_VIRT_HEIGHT, bdev->yres_virtual);

    /* Keep VRAM across enable so a modeset or console takeover does not flash black. */
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);

    /* Enabling resets the virtual line width, so program it afterwards. */
    bochs_hw_setbase(bdev, bdev->stride);
}

/* Select the byte order the adapter expects for @format. */
void bochs_hw_setformat(struct bochs_device *bdev, uint32_t format)
{
    if (!bdev) return;

    switch (format) {
        case DRM_FORMAT_XRGB8888 :
        case DRM_FORMAT_ARGB8888 : // the adapter ignores the alpha byte
            bochs_hw_set_endian(bdev, false);
            break;
        case DRM_FORMAT_BGRX8888 :
            bochs_hw_set_endian(bdev, true);
            break;
        default :
            plogk("bochs: unsupported scanout format 0x%x\n", (unsigned)format);
            return;
    }

    bdev->format = format;
}

/* Point the scanout at VRAM offset 0 with the pitch as virtual width. */
void bochs_hw_setbase(struct bochs_device *bdev, uint32_t stride)
{
    uint32_t virtual_width;
    if (!bdev || !stride) return;

    virtual_width = (stride * 8) / BOCHS_BPP;
    if (!virtual_width || virtual_width > UINT16_MAX) return;

    bdev->stride = stride;
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)virtual_width);
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_X_OFFSET, 0);
    bochs_dispi_write(bdev, VBE_DISPI_INDEX_Y_OFFSET, 0);
}

/* Blank or unblank the output through the VGA attribute controller. */
void bochs_hw_blank(struct bochs_device *bdev, bool blank)
{
    if (!bdev) return;

    /* Enable the colour bit so VGA_IS1_RC is readable, then drop the flip-flop. */
    bochs_vga_writeb(bdev, BOCHS_VGA_MIS_W, BOCHS_VGA_MIS_COLOR);
    (void)bochs_vga_readb(bdev, BOCHS_VGA_IS1_RC);

    /* Only the index and the video-enable bit matter here. */
    bochs_vga_writeb(bdev, BOCHS_VGA_ATT_W, blank ? 0 : 0x20);
}

/* Read @len bytes of EDID data starting at @start inside the register BAR. */
static int bochs_edid_block(struct bochs_device *bdev, uint8_t *buf, size_t start, size_t len)
{
    size_t i;

    if (!bdev->mmio) return -ENODEV;
    if (start + len > BOCHS_MMIO_EDID_LIMIT || start + len > bdev->mmio_size) return -EINVAL;
    for (i = 0; i < len; i++) buf[i] = mmio_read8(bdev->mmio + start + i);

    return 0;
}

/* Read the adapter EDID; returns bytes read, or a negative errno. */
int bochs_edid_read(struct bochs_device *bdev, void *buf, size_t bufsize)
{
    struct edid *edid = (struct edid *)buf;
    uint8_t      header[8];
    size_t       total;

    if (!bdev || !buf || bufsize < EDID_LENGTH) return -EINVAL;

    /* The EDID region is blank unless the adapter provides one. */
    if (bochs_edid_block(bdev, header, 0, sizeof(header))) return -ENODEV;
    if (drm_edid_header_is_valid(header) != (int)sizeof(header)) return -ENODEV;
    if (bochs_edid_block(bdev, (uint8_t *)buf, 0, EDID_LENGTH)) return -ENODEV;
    total = ((size_t)edid->extensions + 1) * EDID_LENGTH;

    /* Reject an over-long blob instead of publishing one that readers would overrun. */
    if (total > BOCHS_EDID_MAX || total > bufsize) return -EINVAL;
    if (total > EDID_LENGTH && bochs_edid_block(bdev, (uint8_t *)buf + EDID_LENGTH, EDID_LENGTH, total - EDID_LENGTH)) return -ENODEV;

    return (int)total;
}

/* True when @fb uses a layout this adapter can show and it fits in VRAM. */
bool bochs_scanout_supported(struct bochs_device *bdev, struct drm_framebuffer *fb)
{
    if (!bdev || !bdev->fb_map) return false;
    if (!fb || !fb->obj[0] || !fb->obj[0]->backing) return false;
    if (!fb->width || !fb->height || fb->width > BOCHS_MAX_DIMENSION || fb->height > BOCHS_MAX_DIMENSION) return false;
    if (fb->format != DRM_FORMAT_XRGB8888 && fb->format != DRM_FORMAT_ARGB8888 && fb->format != DRM_FORMAT_BGRX8888) return false;
    if (fb->pitches[0] < (uint64_t)fb->width * sizeof(uint32_t)) return false;

    /* Byte order correction exists only when the qext block is present. */
    if (fb->format == DRM_FORMAT_BGRX8888 && bdev->qext_size < BOCHS_QEXT_MIN_SIZE) return false;

    /* VBE rounds XRES and VIRT_WIDTH down to 8-pixel steps. */
    if (fb->width % 8) return false;
    if (fb->pitches[0] % (8 * sizeof(uint32_t))) return false;
    if ((uint64_t)fb->pitches[0] / sizeof(uint32_t) > BOCHS_MAX_VIRT_WIDTH) return false;

    /* The whole buffer has to fit both the GEM object and the card's VRAM. */
    if ((uint64_t)fb->offsets[0] + (uint64_t)fb->pitches[0] * fb->height > fb->obj[0]->size) return false;
    if ((uint64_t)fb->pitches[0] * fb->height > bdev->fb_size) return false;

    return true;
}

/* Upload one rectangle of @fb into VRAM; clips are clamped to the buffer. */
int bochs_scanout_rect(struct bochs_device *bdev, struct drm_framebuffer *fb, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    const uint8_t *src;
    uint8_t       *dst;
    size_t         stride;
    uint32_t       y;

    if (!bochs_scanout_supported(bdev, fb)) return -EINVAL;
    if (x2 > fb->width) x2 = fb->width;
    if (y2 > fb->height) y2 = fb->height;
    if (x1 >= x2 || y1 >= y2) return 0;

    stride = fb->pitches[0];
    src    = (const uint8_t *)fb->obj[0]->backing + fb->offsets[0];
    dst    = bdev->fb_map;

    /* The virtual width equals the pitch, so VRAM and source rows line up. */
    for (y = y1; y < y2; y++) {
        size_t offset = (size_t)y * stride + (size_t)x1 * sizeof(uint32_t);
        memcpy(dst + offset, src + offset, (size_t)(x2 - x1) * sizeof(uint32_t));
    }

    return 0;
}

/* Upload @fb and make it the scanout; callers repaint the cursor afterwards. */
int bochs_scanout_fb(struct bochs_device *bdev, struct drm_framebuffer *fb)
{
    int ret;

    if (!bdev) return -EINVAL;
    if (!fb) {
        bdev->current_fb = NULL;
        return 0;
    }

    ret = bochs_scanout_rect(bdev, fb, 0, 0, fb->width, fb->height);
    if (ret) return ret;

    bdev->current_fb = fb;
    return 0;
}

/* True when @fb is the committed primary framebuffer of an active CRTC. */
static bool bochs_fb_is_scanout(struct bochs_device *bdev, struct drm_framebuffer *fb)
{
    struct drm_mode_config *config = &bdev->drm_dev->mode_config;
    ilist_node_t           *node;

    for (node = config->crtc_list.next; node != &config->crtc_list; node = node->next) {
        struct drm_crtc *crtc = container_of(node, struct drm_crtc, head);
        if (crtc->primary && crtc->primary->state && crtc->primary->state->fb == fb && crtc->state && crtc->state->active) return true;
    }
    return false;
}

/* Flush userspace damage from the active scanout buffer into VRAM. */
static int bochs_framebuffer_dirty(struct drm_framebuffer *fb, struct drm_file *file_priv, unsigned int flags, unsigned int color, struct drm_clip_rect *clips, unsigned int num_clips)
{
    struct bochs_device *bdev;
    unsigned int         first;
    unsigned int         step;
    unsigned int         i;

    (void)file_priv;
    (void)color;

    if (!fb || !fb->obj[0] || !fb->base.dev) return -EINVAL;
    bdev = to_bochs_device(fb->base.dev);
    if (!bdev) return -EINVAL;

    /* An off-screen buffer is uploaded in full when it is flipped onto the scanout. */
    if (bdev->current_fb != fb) {
        int ret;

        /* A committed primary may have lost the scanout to the console; damage takes it back. */
        if (!bochs_fb_is_scanout(bdev, fb)) return 0;
        if (!bochs_scanout_supported(bdev, fb)) return -EINVAL;
        bochs_hw_setmode(bdev, fb);
        bochs_hw_setformat(bdev, fb->format);
        ret = bochs_scanout_fb(bdev, fb);
        if (ret) return ret;

        /* A previous plane disable blanked the output. */
        bochs_hw_blank(bdev, false);

        /* The full upload covers any pointer that was on screen. */
        bochs_cursor_repaint(bdev, true);
        return 0;
    }

    if (!num_clips) {
        int ret = bochs_scanout_fb(bdev, fb);
        if (ret) return ret;
    } else {
        first = (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) ? 1U : 0U;
        step  = (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) ? 2U : 1U;
        for (i = first; i < num_clips; i += step) {
            int ret = bochs_scanout_rect(bdev, fb, clips[i].x1, clips[i].y1, clips[i].x2, clips[i].y2);
            if (ret) return ret;
        }
    }

    /* The upload may have covered the pointer. */
    bochs_cursor_repaint(bdev, true);
    return 0;
}

const struct drm_framebuffer_funcs bochs_fb_funcs = {
    .dirty = bochs_framebuffer_dirty,
};

/* Rectangle the pointer covers, clipped to @fb; false when nothing is visible. */
static bool bochs_cursor_rect(struct bochs_device *bdev, struct drm_framebuffer *fb, int32_t *left, int32_t *top, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2)
{
    int64_t l = (int64_t)bdev->cursor_x - bdev->cursor_hot_x;
    int64_t t = (int64_t)bdev->cursor_y - bdev->cursor_hot_y;
    int64_t r = l + (int64_t)bdev->cursor_width;
    int64_t b = t + (int64_t)bdev->cursor_height;

    *left = (int32_t)l;
    *top  = (int32_t)t;

    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (int64_t)fb->width) r = fb->width;
    if (b > (int64_t)fb->height) b = fb->height;
    if (l >= r || t >= b) return false;

    *x1 = (int32_t)l;
    *y1 = (int32_t)t;
    *x2 = (int32_t)r;
    *y2 = (int32_t)b;

    return true;
}

/* Blend a premultiplied ARGB cursor pixel over an opaque scanout pixel. */
static uint32_t bochs_cursor_blend(uint32_t under, uint32_t pixel, bool opaque)
{
    uint32_t alpha = opaque ? 0xffU : (pixel >> 24);
    uint32_t inverse;
    uint32_t r, g, b;

    if (!alpha) return under;
    if (alpha == 0xffU) return pixel;

    inverse = 0xffU - alpha;
    r       = ((pixel >> 16) & 0xffU) + (((under >> 16) & 0xffU) * inverse) / 0xffU;
    g       = ((pixel >> 8) & 0xffU) + (((under >> 8) & 0xffU) * inverse) / 0xffU;
    b       = (pixel & 0xffU) + ((under & 0xffU) * inverse) / 0xffU;

    if (r > 0xffU) r = 0xffU;
    if (g > 0xffU) g = 0xffU;
    if (b > 0xffU) b = 0xffU;

    /* The scanout ignores the top byte; keep it opaque. */
    return 0xff000000U | (r << 16) | (g << 8) | b;
}

/* A BGRX8888 scanout is read big-endian; byte-swap it to the canonical word. */
static uint32_t bochs_cursor_swap_word(uint32_t pixel)
{
    return bswap32(pixel);
}

/* Copy the scanout content under the pointer back into VRAM. */
static void bochs_cursor_erase(struct bochs_device *bdev)
{
    struct drm_framebuffer *fb = bdev->current_fb;
    int32_t                 left, top, x1, y1, x2, y2;

    if (!bdev->cursor_shown || !fb) return;
    if (!bochs_cursor_rect(bdev, fb, &left, &top, &x1, &y1, &x2, &y2)) return;

    (void)bochs_scanout_rect(bdev, fb, (uint32_t)x1, (uint32_t)y1, (uint32_t)x2, (uint32_t)y2);
}

/* Blend the pointer into VRAM on top of the active scanout. */
static void bochs_cursor_paint(struct bochs_device *bdev)
{
    struct drm_framebuffer *fb = bdev->current_fb;
    const uint8_t          *src;
    const uint8_t          *image;
    uint8_t                *dst;
    uint32_t                pitch;
    uint32_t                image_pitch;
    int32_t                 left, top, x1, y1, x2, y2;
    int32_t                 x, y;
    bool                    swap;

    if (!bdev->cursor_shown || !bdev->cursor_obj || !bdev->cursor_obj->backing) return;
    if (!fb || !bochs_scanout_supported(bdev, fb)) return;
    if (!bochs_cursor_rect(bdev, fb, &left, &top, &x1, &y1, &x2, &y2)) return;

    pitch       = fb->pitches[0];
    image_pitch = bdev->cursor_width * sizeof(uint32_t);
    src         = (const uint8_t *)fb->obj[0]->backing + fb->offsets[0];
    image       = (const uint8_t *)bdev->cursor_obj->backing;
    dst         = bdev->fb_map;
    swap        = fb->format == DRM_FORMAT_BGRX8888;

    for (y = y1; y < y2; y++) {
        size_t row     = (size_t)y * pitch;
        size_t img_row = (size_t)(y - top) * image_pitch;

        for (x = x1; x < x2; x++) {
            size_t   offset = row + (size_t)x * sizeof(uint32_t);
            uint32_t under, pixel, blended;

            memcpy(&under, src + offset, sizeof(under));
            memcpy(&pixel, image + img_row + (size_t)(x - left) * sizeof(uint32_t), sizeof(pixel));

            /* The cursor image is ARGB; a BGRX scanout stores the word byte-swapped. */
            blended = bochs_cursor_blend(swap ? bochs_cursor_swap_word(under) : under, pixel, bdev->cursor_opaque);
            if (swap) blended = bochs_cursor_swap_word(blended);

            /* Transparent pixels leave VRAM untouched. */
            if (blended != under) memcpy(dst + offset, &blended, sizeof(blended));
        }
    }
}

/* An image whose alpha bytes are all zero is an opaque XRGB cursor. */
static bool bochs_cursor_is_opaque(const struct drm_gem_object *gem, uint32_t width, uint32_t height)
{
    const uint8_t *image = (const uint8_t *)gem->backing;
    uint32_t       x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            const uint8_t *pixel = image + ((size_t)y * width + x) * sizeof(uint32_t);
            if (pixel[3]) return false;
        }
    }

    return true;
}

/* Install or clear (gem == NULL) the pointer. */
int bochs_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *gem, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y)
{
    struct bochs_device *bdev;
    bool                 enable = gem != NULL;

    if (!crtc) return -ENXIO;
    bdev = to_bochs_device(crtc->dev);

    if (!bdev) return -ENXIO;
    if (enable) {
        if (!width || !height || width > BOCHS_CURSOR_MAX || height > BOCHS_CURSOR_MAX) return -EINVAL;

        /* The image is composited from the object's system-memory backing. */
        if (!gem->backing || gem->size < (uint64_t)width * height * sizeof(uint32_t)) return -EINVAL;
        if (hot_x < 0 || hot_y < 0 || (uint32_t)hot_x >= width || (uint32_t)hot_y >= height) return -EINVAL;
    }

    spin_lock(&bdev->cursor_lock);

    /* Restore whatever the old pointer covered before it is replaced. */
    bochs_cursor_erase(bdev);

    bdev->cursor_shown  = enable;
    bdev->cursor_obj    = enable ? gem : NULL;
    bdev->cursor_width  = enable ? width : 0;
    bdev->cursor_height = enable ? height : 0;
    bdev->cursor_hot_x  = enable ? hot_x : 0;
    bdev->cursor_hot_y  = enable ? hot_y : 0;
    bdev->cursor_opaque = enable ? bochs_cursor_is_opaque(gem, width, height) : false;

    if (enable) bochs_cursor_paint(bdev);

    spin_unlock(&bdev->cursor_lock);
    return 0;
}

/* Place the pointer hotspot at (@x, @y). */
int bochs_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y)
{
    struct bochs_device *bdev;

    if (!crtc) return -ENXIO;
    bdev = to_bochs_device(crtc->dev);
    if (!bdev) return -ENXIO;

    spin_lock(&bdev->cursor_lock);
    bochs_cursor_erase(bdev);
    bdev->cursor_x = x;
    bdev->cursor_y = y;
    bochs_cursor_paint(bdev);
    spin_unlock(&bdev->cursor_lock);

    return 0;
}

/* Restore the covered rect, then repaint; @wait = false trylocks. */
void bochs_cursor_repaint(struct bochs_device *bdev, bool wait)
{
    if (!bdev || !bdev->cursor_shown) return;
    if (wait) {
        spin_lock(&bdev->cursor_lock);
    } else if (!spin_trylock(&bdev->cursor_lock)) {
        return;
    }

    bochs_cursor_erase(bdev);
    bochs_cursor_paint(bdev);
    spin_unlock(&bdev->cursor_lock);
}

/* Remove the pointer from the scanout (last close, device teardown). */
void bochs_cursor_cleanup(struct bochs_device *bdev)
{
    if (!bdev) return;

    spin_lock(&bdev->cursor_lock);
    bochs_cursor_erase(bdev);
    bdev->cursor_shown  = false;
    bdev->cursor_obj    = NULL;
    bdev->cursor_width  = 0;
    bdev->cursor_height = 0;
    spin_unlock(&bdev->cursor_lock);
}

/* Upload the damaged console rectangle, taking the scanout back if a client owns it. */
static void bochs_console_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    struct bochs_device    *bdev = bochs_console_dev;
    struct drm_framebuffer *fb;
    if (!bdev) return;

    fb = bdev->console_fb;
    if (!fb || !fb->obj[0] || !fb->obj[0]->backing) return;
    if (x >= fb->width || y >= fb->height || !width || !height) return;
    if (width > fb->width - x) width = fb->width - x;
    if (height > fb->height - y) height = fb->height - y;

    /* A client may have taken the scanout; put the console mode back. */
    if (bdev->current_fb != fb) {
        if (!bochs_scanout_supported(bdev, fb)) return;

        bochs_hw_setmode(bdev, fb);
        bochs_hw_setformat(bdev, fb->format);
        (void)bochs_scanout_fb(bdev, fb);

        /* Enable alone does not lift a blank left by a CRTC disable. */
        bochs_hw_blank(bdev, false);
        bochs_cursor_repaint(bdev, false);
        return;
    }

    (void)bochs_scanout_rect(bdev, fb, x, y, x + width, y + height);

    /* Console output may have painted over the pointer. */
    bochs_cursor_repaint(bdev, false);
}

/* Hand @fb to the kernel console and keep it alive until the device is torn down. */
void bochs_console_attach(struct bochs_device *bdev, struct drm_framebuffer *fb)
{
    struct drm_device *dev;

    if (!bdev || !fb) return;
    dev = bdev->drm_dev;
    if (!dev) return;

    if (bdev->console_fb != fb) {
        if (bdev->console_fb) drm_framebuffer_put(bdev->console_fb);
        drm_framebuffer_get(fb);
        bdev->console_fb = fb;
    }

    bochs_console_dev           = bdev;
    dev->fb_console_flush       = bochs_console_flush;
    dev->fb_console_flush_guard = NULL;
}

/* Stop the console flush before the framebuffer behind it is released. */
void bochs_console_detach(struct bochs_device *bdev)
{
    struct drm_device *dev;
    if (!bdev) return;

    dev = bdev->drm_dev;
    if (bochs_console_dev == bdev) bochs_console_dev = NULL;
    if (dev) {
        dev->fb_console_flush       = NULL;
        dev->fb_console_flush_guard = NULL;
    }
}

/* True for a Bochs dispi VGA adapter. */
static bool bochs_pci_match(const pci_device_cache_t *entry)
{
    if (!entry) return false;
    if (entry->device_id != BOCHS_PCI_DEVICE_STDVGA) return false;
    return entry->vendor_id == BOCHS_PCI_VENDOR_QEMU || entry->vendor_id == BOCHS_PCI_VENDOR_SIMICS;
}

/* Map both BARs and read the adapter identification. */
static int bochs_hw_init(struct bochs_device *bdev, pci_device_cache_t *pci)
{
    pci_device_reg_t reg = {pci, PCI_CONF_REVISION};
    pci_bar_t        bar;
    uint32_t         revision;
    uint16_t         id;
    uint64_t         vram;

    bdev->pci = pci;

    /* The cursor compositing lock is used from the commit, ioctl and console paths. */
    bdev->cursor_lock.lock = 0;

    /* BAR2 carries the VGA and bochs dispi registers when it is memory mapped. */
    if (pci_map_bar(pci, BOCHS_PCI_BAR_REGISTERS, &bar) == 0 && bar.size >= BOCHS_MMIO_MIN_SPAN) {
        bdev->mmio      = bar.virt;
        bdev->mmio_size = bar.size;
    }

    id = bochs_dispi_read(bdev, VBE_DISPI_INDEX_ID);
    if ((id & 0xfff0) != VBE_DISPI_ID0) {
        plogk("bochs: dispi id mismatch (0x%x), adapter not supported.\n", (unsigned)id);
        return -ENODEV;
    }

    vram = (uint64_t)bochs_dispi_read(bdev, VBE_DISPI_INDEX_VIDEO_MEMORY_64K) * 64 * 1024;
    if (pci_map_bar(pci, BOCHS_PCI_BAR_FRAMEBUFFER, &bar)) {
        plogk("bochs: no usable framebuffer BAR.\n");
        return -ENODEV;
    }
    if (!vram || vram > bar.size) {
        if (vram) plogk("bochs: VRAM size mismatch (adapter %llu kB, BAR %llu kB), using the smaller.\n", (unsigned long long)(vram / 1024), (unsigned long long)(bar.size / 1024));
        vram = bar.size;
    }

    bdev->fb_map  = bar.virt;
    bdev->fb_phys = bar.phys;
    bdev->fb_size = vram;

    if (!bdev->fb_size) {
        plogk("bochs: the framebuffer BAR is empty.\n");
        return -ENODEV;
    }

    /* Revision 2+ exposes the qext block that byte-swaps scanout words. */
    revision = read_pci(reg) & 0xff;
    if (bochs_uses_mmio(bdev) && revision >= 2 && bdev->mmio_size >= BOCHS_MMIO_QEXT_ENDIAN + sizeof(uint32_t)) {
        uint32_t qext = mmio_read32(bdev->mmio + BOCHS_MMIO_QEXT_SIZE);
        if (qext >= BOCHS_QEXT_MIN_SIZE && qext <= bdev->mmio_size) {
            bdev->qext_size = qext;
            bochs_hw_set_endian(bdev, false);
        }
    }

    plogk("bochs: dispi vga id 0x%x, %llu kB VRAM @ 0x%llx, %s registers, qext %u\n", (unsigned)id, (unsigned long long)(bdev->fb_size / 1024), (unsigned long long)bdev->fb_phys,
          bochs_uses_mmio(bdev) ? "mmio" : "ioport", (unsigned)bdev->qext_size);
    return 0;
}

/* Per-open driver state: the adapter needs none. */
static int bochs_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

/* Per-client state is owned by the DRM core. */
static void bochs_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

/* Last close drops the client cursor and hands the display back to the console. */
static void bochs_lastclose(struct drm_device *dev)
{
    struct bochs_device *bdev = to_bochs_device(dev);
    bochs_cursor_cleanup(bdev);

    /* Take the scanout back before the core removes the client's framebuffers. */
    if (bdev && bdev->console_fb && bdev->current_fb != bdev->console_fb) bochs_console_flush(0, 0, bdev->console_fb->width, bdev->console_fb->height);
}

/* Accept a dma-buf that was exported earlier; the pixels are copied on upload. */
static struct drm_gem_object *bochs_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;
    (void)dev;

    if (!obj) return NULL;
    drm_gem_object_get(obj);
    return obj;
}

/* Tear down the device and release every driver resource. */
static void bochs_release(struct drm_device *dev)
{
    struct bochs_device *bdev = to_bochs_device(dev);
    if (!bdev) return;

    /* Detach the console before the KMS objects it reaches through go away. */
    bochs_kms_fini(bdev);
    drm_vblank_cleanup(dev);

    /* drm_mode_config_cleanup() unlinks every KMS object and frees its state. */
    drm_mode_config_cleanup(dev);

    /* Our reference keeps the console scanout alive across the fb_list walk. */
    if (bdev->console_fb) {
        drm_framebuffer_put(bdev->console_fb);
        bdev->console_fb = NULL;
    }

    if (bdev->connector) free(bdev->connector);
    if (bdev->encoder) free(bdev->encoder);
    if (bdev->crtc) free(bdev->crtc);
    if (bdev->primary) free(bdev->primary);

    /* The BAR mappings are part of the kernel page tables for the boot lifetime. */
    free(bdev);
    dev->dev_private = NULL;
}

/* DRM driver descriptor */
static struct drm_driver bochs_drm_driver = {
    .name            = "bochs-drm",
    .desc            = "bochs dispi vga interface",
    .date            = "20261101",
    .major           = 1,
    .minor           = 0,
    .patchlevel      = 0,
    .driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM | DRIVER_PRIME | DRIVER_SYNCHRONOUS_FLIP,

    .open      = bochs_open,
    .postclose = bochs_postclose,
    .lastclose = bochs_lastclose,
    .release   = bochs_release,

    .gem_prime_import = bochs_gem_prime_import,
    .fb_funcs         = &bochs_fb_funcs,

    .dumb_create     = drm_gem_dumb_create,
    .dumb_map_offset = drm_gem_dumb_map_offset,
    .dumb_destroy    = drm_gem_dumb_destroy,
};

/* Machine init / probe */
int bochs_probe(bool *display_owned)
{
    pci_devices_cache_t *cache;
    pci_device_cache_t  *pci = NULL;
    struct bochs_device *bdev;
    struct drm_device   *dev;
    int                  ret;

    cache = pci_get_devices_cache();
    if (!cache) return -ENODEV;

    for (pci_device_cache_t *entry = cache->head; entry; entry = entry->next) {
        if (!bochs_pci_match(entry)) continue;
        pci = entry;
        break;
    }
    if (!pci) return -ENODEV;

    bdev = malloc(sizeof(*bdev));
    if (!bdev) {
        plogk("bochs: out of memory allocating device state.\n");
        return -ENOMEM;
    }
    memset(bdev, 0, sizeof(*bdev));

    /* Decode the framebuffer and both the memory-mapped and I/O register windows. */
    pci_enable_device(pci, PCI_CMD_MEM | PCI_CMD_IO);

    ret = bochs_hw_init(bdev, pci);
    if (ret) {
        free(bdev);
        return ret;
    }

    dev = drm_dev_alloc(&bochs_drm_driver);
    if (!dev) {
        DRM_ERROR("failed to allocate the DRM device.\n");
        free(bdev);
        return -ENOMEM;
    }

    bdev->drm_dev    = dev;
    dev->dev_private = bdev;

    /* Bring up KMS before registration so the node already exposes modes. */
    ret = bochs_kms_init(bdev);
    if (ret) {
        DRM_ERROR("KMS init failed: %d\n", ret);
        drm_dev_unregister(dev); // .release() tears down whatever was built
        return ret;
    }

    ret = drm_dev_register(dev, 0);
    if (ret) {
        DRM_ERROR("failed to register the DRM device: %d\n", ret);
        drm_dev_unregister(dev);
        return ret;
    }

    if (display_owned) *display_owned = true;
    return 0;
}

/* Module exit hook (device lifetime is owned by the DRM release callback). */
void bochs_module_exit(void)
{
    /* No-op: the device is released through bochs_release(). */
}

#endif
