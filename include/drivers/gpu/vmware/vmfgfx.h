/*
 *
 *      vmfgfx.h
 *      VMware SVGA II display driver (vmfgfx) header file
 *
 *      2026/10/04 By Yinyuan34513
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VMFGFX_H_
#define INCLUDE_VMFGFX_H_

#include <drivers/bus/pci.h>
#include <drivers/gpu/drm/drm_device.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

/* VMware SVGA II display controller (PCI 15ad:0405). */
#define VMFGFX_PCI_VENDOR 0x15AD
#define VMFGFX_PCI_DEVICE 0x0405

/* The device only ever scans out 32-bit XRGB pixels. */
#define VMFGFX_BPP      32
#define VMFGFX_BYTES_PP (VMFGFX_BPP / 8)

/* Mode bounds: reported by SVGA_REG_MAX_*, with spec limits as fallback. */
#define VMFGFX_MAX_WIDTH  2368
#define VMFGFX_MAX_HEIGHT 1770
#define VMFGFX_MIN_WIDTH  640
#define VMFGFX_MIN_HEIGHT 480

/* SVGA_REG_ID identification values (SVGA_MAKE_ID from the specification). */
#define SVGA_ID_MAKE(ver) (0x90000000u | (uint32_t)(ver))
#define SVGA_ID_0         SVGA_ID_MAKE(0)
#define SVGA_ID_1         SVGA_ID_MAKE(1)
#define SVGA_ID_2         SVGA_ID_MAKE(2)

/* Offset of the register index port inside I/O BAR 0. */
#define VMFGFX_IO_INDEX 0

/* SVGA_REG_CAPABILITIES bits this driver makes use of. */
#define SVGA_CAP_RECT_FILL       (1u << 0)
#define SVGA_CAP_RECT_COPY       (1u << 1)
#define SVGA_CAP_CURSOR          (1u << 5)
#define SVGA_CAP_CURSOR_BYPASS   (1u << 6)
#define SVGA_CAP_CURSOR_BYPASS_2 (1u << 7)
#define SVGA_CAP_EXTENDED_FIFO   (1u << 15)

/* Register numbers of the command FIFO (VMware SVGA II specification). */
enum vmfgfx_fifo_reg {
    SVGA_FIFO_MIN  = 0,
    SVGA_FIFO_MAX  = 1,
    SVGA_FIFO_NEXT = 2,
    SVGA_FIFO_STOP = 3,
};

/* The FIFO control words are byte offsets and occupy the first 16 bytes. */
#define VMFGFX_FIFO_CTRL_BYTES 16

/* The host rejects a FIFO whose command area is smaller than 10 KiB. */
#define VMFGFX_FIFO_MIN_COMMANDS (10 * 1024)

/* Register numbers of the index/value register pair (SVGA_REG_*). */
enum vmfgfx_reg {
    SVGA_REG_ID                  = 0,
    SVGA_REG_ENABLE              = 1,
    SVGA_REG_WIDTH               = 2,
    SVGA_REG_HEIGHT              = 3,
    SVGA_REG_MAX_WIDTH           = 4,
    SVGA_REG_MAX_HEIGHT          = 5,
    SVGA_REG_DEPTH               = 6,
    SVGA_REG_BITS_PER_PIXEL      = 7,
    SVGA_REG_PSEUDOCOLOR         = 8,
    SVGA_REG_RED_MASK            = 9,
    SVGA_REG_GREEN_MASK          = 10,
    SVGA_REG_BLUE_MASK           = 11,
    SVGA_REG_BYTES_PER_LINE      = 12,
    SVGA_REG_FB_START            = 13,
    SVGA_REG_FB_OFFSET           = 14,
    SVGA_REG_VRAM_SIZE           = 15,
    SVGA_REG_FB_SIZE             = 16,
    SVGA_REG_CAPABILITIES        = 17,
    SVGA_REG_MEM_START           = 18,
    SVGA_REG_MEM_SIZE            = 19,
    SVGA_REG_CONFIG_DONE         = 20,
    SVGA_REG_SYNC                = 21,
    SVGA_REG_BUSY                = 22,
    SVGA_REG_GUEST_ID            = 23,
    SVGA_REG_CURSOR_ID           = 24,
    SVGA_REG_CURSOR_X            = 25,
    SVGA_REG_CURSOR_Y            = 26,
    SVGA_REG_CURSOR_ON           = 27,
    SVGA_REG_HOST_BITS_PER_PIXEL = 28,
    SVGA_REG_SCRATCH_SIZE        = 29,
    SVGA_REG_MEM_REGS            = 30,
    SVGA_REG_NUM_DISPLAYS        = 31,
    SVGA_REG_PITCHLOCK           = 32,
};

/* Command opcodes pushed into the FIFO. */
enum vmfgfx_cmd {
    SVGA_CMD_INVALID_CMD    = 0,
    SVGA_CMD_UPDATE         = 1,
    SVGA_CMD_RECT_FILL      = 2,
    SVGA_CMD_RECT_COPY      = 3,
    SVGA_CMD_DEFINE_CURSOR  = 19,
    SVGA_CMD_DISPLAY_CURSOR = 20,
    SVGA_CMD_MOVE_CURSOR    = 21,
};

/* Legal values for SVGA_REG_CURSOR_ON. */
#define SVGA_CURSOR_ON_HIDE 0
#define SVGA_CURSOR_ON_SHOW 1

/*
 * A SVGA_CMD_DEFINE_CURSOR image holds at most 4096 words, which caps the
 * cursor at 64x64 pixels at 32 bits per pixel - the size every display
 * server already expects from a hardware cursor.
 */
#define VMFGFX_CURSOR_MAX 64

/* Per-device state. */
struct vmfgfx_device {
        struct drm_device  *drm;
        pci_device_cache_t *pci;

        /* I/O BAR 0: register index/value pair. */
        uint16_t io_base;
        uint8_t  io_value; // byte offset of the value port

        /* I/O BAR 1: video RAM, the scanout surface starts at offset 0. */
        void    *vram;
        uint64_t vram_size;

        /* I/O BAR 2: command FIFO, a window of 32-bit words. */
        volatile uint32_t *fifo;
        uint64_t           fifo_size;

        uint32_t caps;
        uint32_t max_width;
        uint32_t max_height;

        /* Programmed mode: the scanout surface is vram[0 .. mode_pitch * mode_height). */
        uint32_t mode_width;
        uint32_t mode_height;
        uint32_t mode_pitch;

        /* KMS objects, allocated by vmfgfx_kms_init(). */
        struct drm_crtc      *crtc;
        struct drm_plane     *primary;
        struct drm_encoder   *encoder;
        struct drm_connector *connector;

        struct drm_framebuffer *current_fb; // framebuffer being scanned out
        raw_spinlock_t          flush_lock;
};

/* Framebuffer callbacks, referenced by the drm_driver in vmfgfx.c. */
extern const struct drm_framebuffer_funcs vmfgfx_fb_funcs;

/* Register file and FIFO access (vmfgfx.c). */
bool     vmfgfx_mode_supported(struct vmfgfx_device *vdev, uint32_t width, uint32_t height);
uint32_t vmfgfx_reg_read(struct vmfgfx_device *vdev, uint32_t reg);
void     vmfgfx_reg_write(struct vmfgfx_device *vdev, uint32_t reg, uint32_t value);
int      vmfgfx_set_mode(struct vmfgfx_device *vdev, uint32_t width, uint32_t height);
int      vmfgfx_scanout_rect(struct vmfgfx_device *vdev, struct drm_framebuffer *fb, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
int      vmfgfx_cursor_set(struct vmfgfx_device *vdev, struct drm_gem_object *gem, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y);
int      vmfgfx_cursor_move(struct vmfgfx_device *vdev, int32_t x, int32_t y);

/* GEM objects (vmfgfx_gem.c). */
struct drm_gem_object *vmfgfx_gem_create(struct drm_device *dev, size_t size);
struct drm_gem_object *vmfgfx_gem_prime_import(struct drm_device *dev, void *dma_buf);

/* KMS pipeline (vmfgfx_kms.c). */
int  vmfgfx_kms_init(struct vmfgfx_device *vdev);
int  vmfgfx_kms_initial_modeset(struct vmfgfx_device *vdev);
void vmfgfx_kms_cleanup(struct vmfgfx_device *vdev);

/*
 * Probe the VMware SVGA II controller and register a KMS device that owns
 * the display: one CRTC scanning a software framebuffer out of video RAM
 * through the SVGA command FIFO.  Returns 0 when a device was attached and
 * a negative errno otherwise, so the GPU driver bus can fall back to the
 * next driver.
 */
int vmfgfx_probe(void);

#endif // INCLUDE_VMFGFX_H_
