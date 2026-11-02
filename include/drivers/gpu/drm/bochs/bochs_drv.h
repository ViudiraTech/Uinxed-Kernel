/*
 *
 *      bochs_drv.h
 *      bochs dispi vga DRM driver - main header
 *
 *      2026/11/1 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BOCHS_DRV_H_
#define INCLUDE_BOCHS_DRV_H_

#include <drivers/bus/pci.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_print.h>
#include <libs/std/errno.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

/* Bochs dispi VGA adapters (QEMU stdvga, Bochs, Simics). */
#define BOCHS_PCI_VENDOR_QEMU   0x1234
#define BOCHS_PCI_DEVICE_STDVGA 0x1111
#define BOCHS_PCI_VENDOR_SIMICS 0x4321

/* BAR0 is the linear framebuffer, BAR2 the VGA/bochs register window. */
#define BOCHS_PCI_BAR_FRAMEBUFFER 0
#define BOCHS_PCI_BAR_REGISTERS   2

/* Legacy index/data pair used when the register BAR is not memory mapped. */
#define VBE_DISPI_IOPORT_INDEX           0x01ce
#define VBE_DISPI_IOPORT_DATA            0x01cf
#define VBE_DISPI_INDEX_ID               0x0
#define VBE_DISPI_INDEX_XRES             0x1
#define VBE_DISPI_INDEX_YRES             0x2
#define VBE_DISPI_INDEX_BPP              0x3
#define VBE_DISPI_INDEX_ENABLE           0x4
#define VBE_DISPI_INDEX_BANK             0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH       0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT      0x7
#define VBE_DISPI_INDEX_X_OFFSET         0x8
#define VBE_DISPI_INDEX_Y_OFFSET         0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xa

/* High twelve bits identify the interface, the low nibble its revision. */
#define VBE_DISPI_ID0         0xb0c0
#define VBE_DISPI_DISABLED    0x00
#define VBE_DISPI_ENABLED     0x01
#define VBE_DISPI_GETCAPS     0x02
#define VBE_DISPI_8BIT_DAC    0x20
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM  0x80

/* Adapter scanout format and limits. */
#define BOCHS_BPP               32                                                   // all scanout is packed 32bpp
#define BOCHS_QEXT_MIN_SIZE     8                                                    // min qext block (rev >= 2) carrying byte-order control
#define BOCHS_MAX_DIMENSION     8192                                                 // largest mode the KMS core may hand us
#define BOCHS_MAX_VIRT_WIDTH    16000                                                // dispi VIRT_WIDTH ceiling (VBE_DISPI_MAX_XRES)
#define BOCHS_EDID_MAX          1024                                                 // at most eight 128-byte EDID blocks
#define BOCHS_MAX_EDID_MODES    64                                                   // EDID modes kept for the boot fallback chain
#define BOCHS_LADDER_MODE_COUNT 18                                                   // reference modes, the default console mode first
#define BOCHS_MAX_TRIED_MODES   (BOCHS_MAX_EDID_MODES + 2 + BOCHS_LADDER_MODE_COUNT) // boot fallback attempt budget
#define BOCHS_CURSOR_MAX        64                                                   // largest cursor userspace expects; composited in VRAM

/* One probed adapter, reached through drm_device->dev_private. */
struct bochs_device {
        struct drm_device  *drm_dev;
        pci_device_cache_t *pci;

        /* Hardware windows */
        uint8_t *mmio;      // register BAR, NULL when only the I/O ports are usable
        uint64_t mmio_size; // bytes of the register window
        uint8_t *fb_map;    // linear framebuffer (the card's VRAM)
        uint64_t fb_phys;   // physical address of the framebuffer BAR
        uint64_t fb_size;   // usable VRAM in bytes
        uint32_t qext_size; // extended register block size, 0 when absent

        /* Currently programmed scanout */
        uint16_t xres;
        uint16_t yres;
        uint16_t yres_virtual;
        uint32_t stride;
        uint32_t bpp;
        uint32_t format; // DRM fourcc the hardware was last told to expect

        /* DRM objects (KMS objects are released by drm_mode_config_cleanup) */
        struct drm_plane       *primary;
        struct drm_crtc        *crtc;
        struct drm_encoder     *encoder;
        struct drm_connector   *connector;
        struct drm_framebuffer *current_fb; // framebuffer the hardware scans out
        struct drm_framebuffer *console_fb; // scanout handed to the kernel console (reference held)

        /* EDID modes of the last connector probe, for the boot fallback chain. */
        struct drm_display_mode *edid_modes[BOCHS_MAX_EDID_MODES];
        int                      edid_mode_count;

        /* Software cursor; driver only borrows the GEM object handed to cursor_set(). */
        spinlock_t             cursor_lock;
        struct drm_gem_object *cursor_obj;
        uint32_t               cursor_width;
        uint32_t               cursor_height;
        int32_t                cursor_hot_x;
        int32_t                cursor_hot_y;
        int32_t                cursor_x; // hotspot position, in CRTC coordinates
        int32_t                cursor_y;
        bool                   cursor_shown;
        bool                   cursor_opaque; // image carries no transparency
};

/* Framebuffer functions the driver hands to drm_framebuffer_init(). */
extern const struct drm_framebuffer_funcs bochs_fb_funcs;

/* Embedded-state accessor used by both driver translation units. */
struct bochs_device *to_bochs_device(const struct drm_device *dev);

/* Probe the first bochs VGA adapter on the PCI bus; 0 when one was attached. */
int bochs_probe(bool *display_owned);

/* Module exit hook (device lifetime is owned by the DRM release callback). */
void bochs_module_exit(void);

/* True when @fb is a layout this adapter can scan out and it fits in VRAM. */
bool bochs_scanout_supported(struct bochs_device *bdev, struct drm_framebuffer *fb);

/* Copy a framebuffer rectangle from its GEM backing into the adapter's VRAM. */
int bochs_scanout_rect(struct bochs_device *bdev, struct drm_framebuffer *fb, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);

/* Upload the whole framebuffer and make it the active scanout (NULL disables). */
int bochs_scanout_fb(struct bochs_device *bdev, struct drm_framebuffer *fb);

/* Program the DISPI mode, its pixel layout and its base address. */
void bochs_hw_setmode(struct bochs_device *bdev, const struct drm_framebuffer *fb);
void bochs_hw_setformat(struct bochs_device *bdev, uint32_t format);
void bochs_hw_setbase(struct bochs_device *bdev, uint32_t stride);

/* Blank or unblank the output. */
void bochs_hw_blank(struct bochs_device *bdev, bool blank);

/* Read the monitor EDID exposed at the start of the register BAR; returns its size. */
int bochs_edid_read(struct bochs_device *bdev, void *buf, size_t bufsize);

/* Attach/detach the kernel console to the scanout framebuffer. */
void bochs_console_attach(struct bochs_device *bdev, struct drm_framebuffer *fb);
void bochs_console_detach(struct bochs_device *bdev);

/* Software cursor composited into VRAM (the adapter has no cursor hardware). */
int  bochs_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *gem, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y);
int  bochs_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y);
void bochs_cursor_repaint(struct bochs_device *bdev, bool wait);
void bochs_cursor_cleanup(struct bochs_device *bdev);

/* Build plane/CRTC/encoder/connector, probe modes and run the boot modeset. */
int bochs_kms_init(struct bochs_device *bdev);

/* Detach the console flush hook, the cursor and the live scanout pointer. */
void bochs_kms_fini(struct bochs_device *bdev);

#endif // INCLUDE_BOCHS_DRV_H_
