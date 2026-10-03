/*
 *
 *      hda.h
 *      Intel HD Audio driver header file
 *
 *      2026/7/24 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_HDA_H_
#define INCLUDE_HDA_H_

#include <libs/std/stdint.h>

/* Global registers (offset from MMIO base) */
#define HDA_GCAP      0x00
#define HDA_VMIN      0x02
#define HDA_VMAJ      0x03
#define HDA_GCTL      0x08
#define HDA_WAKEEN    0x0c
#define HDA_STATESTS  0x0e
#define HDA_GSTS      0x10
#define HDA_INTCTL    0x20
#define HDA_INTSTS    0x24
#define HDA_WALLCLK   0x30
#define HDA_SSYNC     0x38
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP    0x48
#define HDA_CORBRP    0x4a
#define HDA_CORBCTL   0x4c
#define HDA_CORBSTS   0x4d
#define HDA_CORBSIZE  0x4e
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP    0x58
#define HDA_RINTCNT   0x5a
#define HDA_RIRBCTL   0x5c
#define HDA_RIRBSTS   0x5d
#define HDA_RIRBSIZE  0x5e
#define HDA_IC        0x60
#define HDA_IR        0x64
#define HDA_IRS       0x68
#define HDA_DPLBASE   0x70
#define HDA_DPUBASE   0x74

/* Stream descriptor offsets */
#define HDA_SD_CTL      0x00
#define HDA_SD_STS      0x03
#define HDA_SD_LPIB     0x04
#define HDA_SD_CBL      0x08
#define HDA_SD_LVI      0x0c
#define HDA_SD_FIFOW    0x0e
#define HDA_SD_FIFOSIZE 0x10
#define HDA_SD_FORMAT   0x12
#define HDA_SD_BDLPL    0x18
#define HDA_SD_BDLPU    0x1c

/* GCTL bits */
#define AZX_GCTL_RESET (1 << 0)
#define AZX_GCTL_UNSOL (1 << 8)

/* INTCTL bits */
#define AZX_INT_GLOBAL_EN (1 << 0)
#define AZX_INT_CTRL_EN   (1 << 1)

/* CORBSIZE/RIRBSIZE field (bits 1:0): 00b = 2, 01b = 16, 10b = 256 entries */
#define HDA_RING_SIZE_256 0x02

/* CORB/RIRB bits */
#define AZX_CORBRP_RST   (1 << 15)
#define AZX_CORBCTL_RUN  (1 << 1)
#define AZX_RIRBWP_RST   (1 << 15)
#define AZX_RBCTL_DMA_EN (1 << 0)
#define AZX_RBCTL_IRQ_EN (1 << 1)

/* IRS bits */
#define AZX_IRS_BUSY  (1 << 0)
#define AZX_IRS_VALID (1 << 1)

/* Stream control bits */
#define HDA_SD_CTL_DMA_START    (1 << 1)
#define HDA_SD_INT_MASK         (1 << 2)
#define HDA_SD_STS_DMA_COMPLETE (1 << 2)
#define HDA_SD_STS_FIFO_ERROR   (1 << 3)
#define HDA_SD_STS_DESC_ERROR   (1 << 4)

/* Codec verbs */
#define AC_VERB_PARAMETERS             0xf00
#define AC_VERB_GET_CONFIG_DEFAULT     0xf1c
#define AC_VERB_GET_CONNECT_LIST       0xf02
#define AC_VERB_SET_STREAM_FORMAT      0x200
#define AC_VERB_SET_CHANNEL_STREAMID   0x600
#define AC_VERB_SET_AMP_GAIN_MUTE      0x300
#define AC_VERB_GET_AMP_GAIN_MUTE      0x300
#define AC_VERB_SET_PIN_WIDGET_CONTROL 0x700
#define AC_VERB_SET_CONNECT_SEL        0x701
#define AC_VERB_SET_POWER_STATE        0x705
#define AC_VERB_SET_EAPD_BTLENABLE     0x70c

/* Parameters */
#define AC_PAR_VENDOR_ID        0x00
#define AC_PAR_SUBSYSTEM_ID     0x01
#define AC_PAR_REV_ID           0x02
#define AC_PAR_NODE_COUNT       0x04
#define AC_PAR_FUNCTION_TYPE    0x05
#define AC_PAR_AUDIO_WIDGET_CAP 0x09
#define AC_PAR_PCM              0x0a
#define AC_PAR_PIN_CAP          0x0c
#define AC_PAR_AMP_IN_CAP       0x0d
#define AC_PAR_CONNLIST_LEN     0x0e
#define AC_PAR_AMP_OUT_CAP      0x12

/* Widget types */
#define AC_WID_AUD_OUT 0x0
#define AC_WID_AUD_IN  0x1
#define AC_WID_AUD_MIX 0x2
#define AC_WID_AUD_SEL 0x3
#define AC_WID_PIN     0x4
#define AC_WID_POWER   0x5
#define AC_WID_VOL_KNB 0x6

/* Function group types */
#define AC_GRP_AUDIO_FUNCTION 0x01
#define AC_GRP_MODEM_FUNCTION 0x02

/* Pin caps */
#define AC_PINCAP_OUT  (1 << 4)
#define AC_PINCAP_IN   (1 << 5)
#define AC_PINCAP_EAPD (1 << 16)

/* Pin widget control */
#define AC_PINCTL_OUT_EN (1 << 5)
#define AC_PINCTL_HP_EN  (1 << 6)

/* EAPD */
#define AC_EAPD_BTLENABLE (1 << 1)

/* AMP payload */
#define AC_AMP_GET_OUTPUT  0
#define AC_AMP_GET_INPUT   (1 << 14)
#define AC_AMP_GET_LEFT    (1 << 13)
#define AC_AMP_GET_RIGHT   0
#define AC_AMP_SET_OUTPUT  0
#define AC_AMP_SET_INPUT   (1 << 11)
#define AC_AMP_SET_LEFT    (1 << 10)
#define AC_AMP_SET_RIGHT   0
#define AC_AMP_SET_MUTE    (1 << 15)
#define AC_AMP_SET_UNMUTE  0
#define AC_AMP_SET_GAIN(g) ((uint16_t)(g) & 0x7f)

/* Widget cap bits */
#define AC_WCAP_STEREO     (1 << 0)
#define AC_WCAP_IN_AMP     (1 << 1)
#define AC_WCAP_OUT_AMP    (1 << 2)
#define AC_WCAP_CONN_LIST  (1 << 8)
#define AC_WCAP_DIGITAL    (1 << 9)
#define AC_WCAP_POWER      (1 << 10)
#define AC_WCAP_TYPE_SHIFT 20
#define AC_WCAP_TYPE_MASK  0xf

/* Power state */
#define AC_PWRST_D0 0x00
#define AC_PWRST_D3 0x03

/* Config default pin fields */
#define AC_DEFCFG_DEVICE_SHIFT 20
#define AC_DEFCFG_DEVICE_MASK  (0xf << 20)

/* Stream format */
#define AC_FMT_BITS_SHIFT 4
#define AC_FMT_CHAN_SHIFT 0
#define AC_FMT_DIV_SHIFT  8
#define AC_FMT_MULT_SHIFT 11
#define AC_FMT_BASE_RATE  48000

/* PCI */
#define PCI_CLASS_HDA 0x040300

/* Buffer descriptor list entry (DMA scatter-gather element) */
typedef struct hda_bdle {
        uint32_t addr_low;
        uint32_t addr_high;
        uint32_t length;
        uint32_t ioc;
} __attribute__((packed)) hda_bdle_t;

/* Probe and initialize the Intel HD Audio controller. */
#if CONFIG_AUDIO_HDA
void hda_init(void);
#else
static inline void hda_init(void) {}
#endif

#endif // INCLUDE_HDA_H_
