/*
 *
 *      drm_edid.h
 *      DRM EDID (Extended Display Identification Data) parsing
 *
 *      2026/8/10 by MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRM_EDID_H_
#define INCLUDE_DRM_EDID_H_

#include <drivers/gpu/drm/drm_device.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define EDID_LENGTH 128
#define DDC_ADDR    0x50
#define DDC_ADDR2   0x52 /* E-DDC 1.2 - where DisplayID can hide */

#define CEA_EXT       0x02
#define VTB_EXT       0x10
#define DI_EXT        0x40
#define LS_EXT        0x50
#define MI_EXT        0x60
#define DISPLAYID_EXT 0x70

struct est_timings {
        uint8_t t1;
        uint8_t t2;
        uint8_t mfg_rsvd;
} __attribute__((packed));

/* 00=16:10, 01=4:3, 10=5:4, 11=16:9 */
#define EDID_TIMING_ASPECT_SHIFT 6
#define EDID_TIMING_ASPECT_MASK  (0x3 << EDID_TIMING_ASPECT_SHIFT)

/* need to add 60 */
#define EDID_TIMING_VFREQ_SHIFT 0
#define EDID_TIMING_VFREQ_MASK  (0x3f << EDID_TIMING_VFREQ_SHIFT)

struct std_timing {
        uint8_t hsize; /* need to multiply by 8 then add 248 */
        uint8_t vfreq_aspect;
} __attribute__((packed));

#define DRM_EDID_PT_HSYNC_POSITIVE (1 << 1)
#define DRM_EDID_PT_VSYNC_POSITIVE (1 << 2)
#define DRM_EDID_PT_SEPARATE_SYNC  (3 << 3)
#define DRM_EDID_PT_STEREO         (1 << 5)
#define DRM_EDID_PT_INTERLACED     (1 << 7)

/* If detailed data is pixel timing */
struct detailed_pixel_timing {
        uint8_t hactive_lo;
        uint8_t hblank_lo;
        uint8_t hactive_hblank_hi;
        uint8_t vactive_lo;
        uint8_t vblank_lo;
        uint8_t vactive_vblank_hi;
        uint8_t hsync_offset_lo;
        uint8_t hsync_pulse_width_lo;
        uint8_t vsync_offset_pulse_width_lo;
        uint8_t hsync_vsync_offset_pulse_width_hi;
        uint8_t width_mm_lo;
        uint8_t height_mm_lo;
        uint8_t width_height_mm_hi;
        uint8_t hborder;
        uint8_t vborder;
        uint8_t misc;
} __attribute__((packed));

/* If it's not pixel timing, it'll be one of the below */
struct detailed_data_string {
        uint8_t str[13];
} __attribute__((packed));

#define DRM_EDID_RANGE_OFFSET_MIN_VFREQ (1 << 0) /* 1.4 */
#define DRM_EDID_RANGE_OFFSET_MAX_VFREQ (1 << 1) /* 1.4 */
#define DRM_EDID_RANGE_OFFSET_MIN_HFREQ (1 << 2) /* 1.4 */
#define DRM_EDID_RANGE_OFFSET_MAX_HFREQ (1 << 3) /* 1.4 */

#define DRM_EDID_DEFAULT_GTF_SUPPORT_FLAG   0x00 /* 1.3 */
#define DRM_EDID_RANGE_LIMITS_ONLY_FLAG     0x01 /* 1.4 */
#define DRM_EDID_SECONDARY_GTF_SUPPORT_FLAG 0x02 /* 1.3 */
#define DRM_EDID_CVT_SUPPORT_FLAG           0x04 /* 1.4 */

#define DRM_EDID_CVT_FLAGS_STANDARD_BLANKING (1 << 3)
#define DRM_EDID_CVT_FLAGS_REDUCED_BLANKING  (1 << 4)

struct detailed_data_monitor_range {
        uint8_t min_vfreq;
        uint8_t max_vfreq;
        uint8_t min_hfreq_khz;
        uint8_t max_hfreq_khz;
        uint8_t pixel_clock_mhz; /* need to multiply by 10 */
        uint8_t flags;
        union {
                struct {
                        uint8_t  reserved;
                        uint8_t  hfreq_start_khz; /* need to multiply by 2 */
                        uint8_t  c;               /* need to divide by 2 */
                        uint16_t m;
                        uint8_t  k;
                        uint8_t  j; /* need to divide by 2 */
                } __attribute__((packed)) gtf2;
                struct {
                        uint8_t version;
                        uint8_t data1; /* high 6 bits: extra clock resolution */
                        uint8_t data2; /* plus low 2 of above: max hactive */
                        uint8_t supported_aspects;
                        uint8_t flags; /* preferred aspect and blanking support */
                        uint8_t supported_scalings;
                        uint8_t preferred_refresh;
                } __attribute__((packed)) cvt;
        } __attribute__((packed)) formula;
} __attribute__((packed));

struct detailed_data_wpindex {
        uint8_t white_yx_lo; /* Lower 2 bits each */
        uint8_t white_x_hi;
        uint8_t white_y_hi;
        uint8_t gamma; /* need to divide by 100 then add 1 */
} __attribute__((packed));

struct detailed_data_color_point {
        uint8_t windex1;
        uint8_t wpindex1[3];
        uint8_t windex2;
        uint8_t wpindex2[3];
} __attribute__((packed));

struct cvt_timing {
        uint8_t code[3];
} __attribute__((packed));

struct detailed_non_pixel {
        uint8_t pad1;
        uint8_t type; /* ff=serial, fe=string, fd=monitor range, fc=monitor name
                         fb=color point data, fa=standard timing data,
                         f9=undefined, f8=mfg. reserved */
        uint8_t pad2;
        union {
                struct detailed_data_string        str;
                struct detailed_data_monitor_range range;
                struct detailed_data_wpindex       color;
                struct std_timing                  timings[6];
                struct cvt_timing                  cvt[4];
        } __attribute__((packed)) data;
} __attribute__((packed));

#define EDID_DETAIL_EST_TIMINGS     0xf7
#define EDID_DETAIL_CVT_3BYTE       0xf8
#define EDID_DETAIL_COLOR_MGMT_DATA 0xf9
#define EDID_DETAIL_STD_MODES       0xfa
#define EDID_DETAIL_MONITOR_CPDATA  0xfb
#define EDID_DETAIL_MONITOR_NAME    0xfc
#define EDID_DETAIL_MONITOR_RANGE   0xfd
#define EDID_DETAIL_MONITOR_STRING  0xfe
#define EDID_DETAIL_MONITOR_SERIAL  0xff

struct detailed_timing {
        uint16_t pixel_clock; /* need to multiply by 10 KHz */
        union {
                struct detailed_pixel_timing pixel_data;
                struct detailed_non_pixel    other_data;
        } __attribute__((packed)) data;
} __attribute__((packed));

#define DRM_EDID_INPUT_SERRATION_VSYNC (1 << 0)
#define DRM_EDID_INPUT_SYNC_ON_GREEN   (1 << 1)
#define DRM_EDID_INPUT_COMPOSITE_SYNC  (1 << 2)
#define DRM_EDID_INPUT_SEPARATE_SYNCS  (1 << 3)
#define DRM_EDID_INPUT_BLANK_TO_BLACK  (1 << 4)
#define DRM_EDID_INPUT_VIDEO_LEVEL     (3 << 5)
#define DRM_EDID_INPUT_DIGITAL         (1 << 7)
#define DRM_EDID_DIGITAL_DEPTH_MASK    (7 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_UNDEF   (0 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_6       (1 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_8       (2 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_10      (3 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_12      (4 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_14      (5 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_16      (6 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_RSVD    (7 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_MASK     (7 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_UNDEF    (0 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_DVI      (1 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_HDMI_A   (2 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_HDMI_B   (3 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_MDDI     (4 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_DP       (5 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_DFP_1_X       (1 << 0) /* 1.3 */

#define DRM_EDID_FEATURE_DEFAULT_GTF      (1 << 0) /* 1.2 */
#define DRM_EDID_FEATURE_CONTINUOUS_FREQ  (1 << 0) /* 1.4 */
#define DRM_EDID_FEATURE_PREFERRED_TIMING (1 << 1)
#define DRM_EDID_FEATURE_STANDARD_COLOR   (1 << 2)
/* If analog */
#define DRM_EDID_FEATURE_DISPLAY_TYPE (3 << 3) /* 00=mono, 01=rgb, 10=non-rgb, 11=unknown */
/* If digital */
#define DRM_EDID_FEATURE_COLOR_MASK   (3 << 3)
#define DRM_EDID_FEATURE_RGB          (0 << 3)
#define DRM_EDID_FEATURE_RGB_YCRCB444 (1 << 3)
#define DRM_EDID_FEATURE_RGB_YCRCB422 (2 << 3)
#define DRM_EDID_FEATURE_RGB_YCRCB    (3 << 3) /* both 4:4:4 and 4:2:2 */

#define DRM_EDID_FEATURE_PM_ACTIVE_OFF (1 << 5)
#define DRM_EDID_FEATURE_PM_SUSPEND    (1 << 6)
#define DRM_EDID_FEATURE_PM_STANDBY    (1 << 7)

#define DRM_EDID_HDMI_DC_48   (1 << 6)
#define DRM_EDID_HDMI_DC_36   (1 << 5)
#define DRM_EDID_HDMI_DC_30   (1 << 4)
#define DRM_EDID_HDMI_DC_Y444 (1 << 3)

struct edid {
        uint8_t header[8];
        /* Vendor & product info */
        uint8_t  mfg_id[2];
        uint8_t  prod_code[2];
        uint32_t serial; /* FIXME: byte order */
        uint8_t  mfg_week;
        uint8_t  mfg_year;
        /* EDID version */
        uint8_t version;
        uint8_t revision;
        /* Display info: */
        uint8_t input;
        uint8_t width_cm;
        uint8_t height_cm;
        uint8_t gamma;
        uint8_t features;
        /* Color characteristics */
        uint8_t red_green_lo;
        uint8_t blue_white_lo;
        uint8_t red_x;
        uint8_t red_y;
        uint8_t green_x;
        uint8_t green_y;
        uint8_t blue_x;
        uint8_t blue_y;
        uint8_t white_x;
        uint8_t white_y;
        /* Est. timings and mfg rsvd timings */
        struct est_timings established_timings;
        /* Standard timings 1-8 */
        struct std_timing standard_timings[8];
        /* Detailing timings 1-4 */
        struct detailed_timing detailed_timings[4];
        /* Number of 128 byte ext. blocks */
        uint8_t extensions;
        /* Checksum */
        uint8_t checksum;
} __attribute__((packed));

#define EDID_PRODUCT_ID(e) ((e)->prod_code[0] | ((e)->prod_code[1] << 8))

/* Short Audio Descriptor */
struct cea_sad {
        uint8_t format;
        uint8_t channels; /* max number of channels - 1 */
        uint8_t freq;
        uint8_t byte2; /* meaning depends on format */
};

/*
 * Decode the manufacturer ID.
 * @mfg_id: The manufacturer ID
 * @vend: A 4-byte buffer to store the 3-letter vendor string plus a '\0'
 *        termination.
 */
static inline const char *drm_edid_decode_mfg_id(uint16_t mfg_id, char vend[4])
{
    vend[0] = '@' + ((mfg_id >> 10) & 0x1f);
    vend[1] = '@' + ((mfg_id >> 5) & 0x1f);
    vend[2] = '@' + ((mfg_id >> 0) & 0x1f);
    vend[3] = '\0';

    return vend;
}

/*
 * Encode an ID for matching against drm_edid_get_panel_id().
 * @vend_chr_0..2: The three vendor string characters.
 * @product_id: The 16-bit product ID.
 */
#define drm_edid_encode_panel_id(vend_chr_0, vend_chr_1, vend_chr_2, product_id)                   \
    ((((uint32_t)(vend_chr_0) - '@') & 0x1f) << 26 | (((uint32_t)(vend_chr_1) - '@') & 0x1f) << 21 \
     | (((uint32_t)(vend_chr_2) - '@') & 0x1f) << 16 | ((product_id) & 0xffff))

struct drm_display_mode *drm_mode_find_dmt(struct drm_device *dev, int hsize, int vsize, int fresh, bool rb);

struct drm_display_mode *drm_display_mode_from_cea_vic(struct drm_device *dev, uint8_t video_code);

uint8_t drm_match_cea_mode(const struct drm_display_mode *to_match);

int          drm_edid_header_is_valid(const void *edid);
bool         drm_edid_is_valid(struct edid *edid);
struct edid *drm_edid_duplicate(const struct edid *edid);
void         drm_edid_get_monitor_name(const struct edid *edid, char *name, int bufsize);
void         drm_edid_to_display_info(struct drm_connector *connector, const struct edid *edid);
int          drm_add_edid_modes(struct drm_connector *connector, struct edid *edid);
bool         drm_edid_is_digital(const struct edid *edid);
bool         drm_detect_hdmi_monitor(const struct edid *edid);
bool         drm_detect_monitor_audio(const struct edid *edid);

#endif // INCLUDE_DRM_EDID_H_
