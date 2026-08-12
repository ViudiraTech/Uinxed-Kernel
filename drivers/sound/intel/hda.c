/*
 *
 *      hda.c
 *      Intel HD Audio driver
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/bus/pci.h>
#include <drivers/sound/core/audio.h>
#include <drivers/sound/intel/hda.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

#define HDA_MAX_CODECS      4
#define HDA_MAX_STREAMS     8
#define HDA_BDL_ENTRIES     256
#define HDA_DMA_BUFFER_SIZE (64 * 1024)
#define HDA_PERIOD_FRAGS    32

/* Global registers (offset from MMIO base) */
#define GCAP      0x00
#define VMIN      0x02
#define VMAJ      0x03
#define GCTL      0x08
#define WAKEEN    0x0c
#define STATESTS  0x0e
#define GSTS      0x10
#define INTCTL    0x20
#define INTSTS    0x24
#define WALLCLK   0x30
#define SSYNC     0x38
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4a
#define CORBCTL   0x4c
#define CORBSTS   0x4d
#define CORBSIZE  0x4e
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5a
#define RIRBCTL   0x5c
#define RIRBSTS   0x5d
#define RIRBSIZE  0x5e
#define IC        0x60
#define IR        0x64
#define IRS       0x68
#define DPLBASE   0x70
#define DPUBASE   0x74

/* Stream descriptor offsets */
#define SD_CTL      0x00
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0c
#define SD_FIFOW    0x0e
#define SD_FIFOSIZE 0x10
#define SD_FORMAT   0x12
#define SD_BDLPL    0x18
#define SD_BDLPU    0x1c

/* GCTL bits */
#define AZX_GCTL_RESET (1 << 0)
#define AZX_GCTL_UNSOL (1 << 8)

/* INTCTL bits */
#define AZX_INT_GLOBAL_EN (1 << 0)
#define AZX_INT_CTRL_EN   (1 << 1)

/* CORB/RIRB bits */
#define AZX_CORBRP_RST       (1 << 15)
#define AZX_CORBCTL_RUN      (1 << 1)
#define AZX_RIRBWP_RST       (1 << 15)
#define AZX_RBCTL_DMA_EN     (1 << 0)
#define AZX_RBCTL_IRQ_EN     (1 << 1)
#define AZX_MAX_CORB_ENTRIES 256
#define AZX_MAX_RIRB_ENTRIES 256
#define HDA_CORB_ENTRIES     256
#define HDA_RIRB_ENTRIES     256

/* IRS bits */
#define AZX_IRS_BUSY  (1 << 0)
#define AZX_IRS_VALID (1 << 1)

/* Stream control bits */
#define SD_CTL_DMA_START    (1 << 1)
#define SD_INT_MASK         (1 << 2)
#define SD_STS_DMA_COMPLETE (1 << 2)
#define SD_STS_FIFO_ERROR   (1 << 3)
#define SD_STS_DESC_ERROR   (1 << 4)

/* BDL descriptor */
typedef struct hda_bdle {
        uint32_t addr_low;
        uint32_t addr_high;
        uint32_t length;
        uint32_t ioc;
} __attribute__((packed)) hda_bdle_t;

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
#define PCI_VENDOR_INTEL 0x8086
#define PCI_CLASS_HDA    0x040300

/* HDA codec widget descriptor */
typedef struct hda_widget {
        uint16_t  nid;
        uint8_t   type;
        uint32_t  wcap;
        uint32_t  pincap;
        uint32_t  def_conf;
        uint8_t   num_conns;
        uint16_t *conns;
} hda_widget_t;

/* HDA codec descriptor */
typedef struct hda_codec {
        uint8_t       addr;
        uint16_t      vendor_id;
        uint16_t      device_id;
        int           afg_nid;
        int           num_widgets;
        hda_widget_t *widgets;
        int           dac_count;
        int           adc_count;
        int           pin_count;
        int           dac_nid;
        int           adc_nid;
        int           pin_nid;
} hda_codec_t;

/* HDA controller state */
typedef struct hda_controller {
        int                 found;
        volatile void      *mmio;
        pci_device_cache_t *pci_dev;
        uint32_t            irq;
        uint32_t            mmio_size;

        hda_codec_t codecs[HDA_MAX_CODECS];
        int         num_codecs;

        spinlock_t lock;

        /* CORB/RIRB DMA buffers */
        volatile uint32_t *corb_buf;
        volatile uint32_t *rirb_buf;
        uint64_t           corb_phys;
        uint64_t           rirb_phys;
        int                rirb_rp;
        int                corb_entries;
        int                rirb_entries;
        int                cmd_count[HDA_MAX_CODECS];
        uint32_t           res[HDA_MAX_CODECS];

        /* Streams */
        struct {
                int               allocated;
                int               direction; // 0=playback, 1=capture
                volatile uint8_t *buf;
                uint64_t          buf_phys;
                size_t            buf_size;
                volatile void    *bdl;
                uint64_t          bdl_phys;
                int               running;
                int               period_frags;
                snd_pcm_uframes_t period_frames;
                snd_pcm_uframes_t total_frames;
                snd_pcm_uframes_t hw_pos;   // frames consumed
                audio_pcm_file_t *pcm_file; // owner
        } streams[HDA_MAX_STREAMS];

        /* Audio interface */
        audio_pcm_format_t audio_fmt;
        int                audio_registered;
        size_t             buffer_bytes;
        size_t             period_bytes;

        /* Playback / capture stream index */
        int playback_stream;
        int capture_stream;
} hda_controller_t;

static hda_controller_t hda_ctrl;

/* MMIO access helpers */
static inline uint32_t hda_read32(uint16_t reg)
{
    return mmio_read32((void *)((uintptr_t)hda_ctrl.mmio + reg));
}

static inline void hda_write32(uint16_t reg, uint32_t val)
{
    mmio_write32((void *)((uintptr_t)hda_ctrl.mmio + reg), val);
}

static inline uint16_t hda_read16(uint16_t reg)
{
    volatile uint16_t *ptr = (volatile uint16_t *)((uintptr_t)hda_ctrl.mmio + reg);
    return *ptr;
}

static inline void hda_write16(uint16_t reg, uint16_t val)
{
    volatile uint16_t *ptr = (volatile uint16_t *)((uintptr_t)hda_ctrl.mmio + reg);
    *ptr                   = val;
}

static inline uint8_t hda_read8(uint16_t reg)
{
    volatile uint8_t *ptr = (volatile uint8_t *)((uintptr_t)hda_ctrl.mmio + reg);
    return *ptr;
}

static inline void hda_write8(uint16_t reg, uint8_t val)
{
    volatile uint8_t *ptr = (volatile uint8_t *)((uintptr_t)hda_ctrl.mmio + reg);
    *ptr                  = val;
}

static inline uint32_t sd_read32(int stream, uint16_t reg)
{
    return hda_read32(0x80 + stream * 0x20 + reg);
}

static inline void sd_write32(int stream, uint16_t reg, uint32_t val)
{
    hda_write32(0x80 + stream * 0x20 + reg, val);
}

static inline uint16_t sd_read16(int stream, uint16_t reg)
{
    return hda_read16(0x80 + stream * 0x20 + reg);
}

static inline void sd_write16(int stream, uint16_t reg, uint16_t val)
{
    hda_write16(0x80 + stream * 0x20 + reg, val);
}

static inline uint8_t sd_read8(int stream, uint16_t reg)
{
    return hda_read8(0x80 + stream * 0x20 + reg);
}

static inline void sd_write8(int stream, uint16_t reg, uint8_t val)
{
    hda_write8(0x80 + stream * 0x20 + reg, val);
}

/* BAR size probing */
static uint32_t hda_read_bar_size(pci_device_cache_t *dev, int bar_idx)
{
    pci_device_reg_t reg  = {.parent = dev, .offset = (uint16_t)(0x10 + 4 * bar_idx)};
    uint32_t         orig = read_pci(reg);
    if (!orig || orig == 0xFFFFFFFF) return 0;

    uint32_t bar_type = (orig >> 1) & 0b11;
    int      is_64bit = (bar_type == BAR_S64);

    write_pci(reg, 0xFFFFFFFF);
    uint32_t sized = read_pci(reg) & 0xFFFFFFF0;
    write_pci(reg, orig);

    if (is_64bit) {
        pci_device_reg_t reg_hi  = {.parent = dev, .offset = (uint16_t)(0x10 + 4 * (bar_idx + 1))};
        uint32_t         orig_hi = read_pci(reg_hi);
        write_pci(reg_hi, 0xFFFFFFFF);
        uint32_t sized_hi = read_pci(reg_hi);
        write_pci(reg_hi, orig_hi);
        uint64_t val64 = (uint64_t)sized | ((uint64_t)sized_hi << 32);
        if (!val64) return 0;
        uint64_t sz = ~val64 + 1;
        if (sz > 0xFFFFFFFFULL) return 0xFFFFFFFF;
        return (uint32_t)sz;
    }

    if (!sized) return 0;
    return (~sized + 1) & 0xFFFFFFFF;
}

/* Verb helpers */
static inline uint32_t hda_mk_verb(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload)
{
    return ((uint32_t)(addr & 0xf) << 28) | ((uint32_t)(nid & 0xff) << 20) | (verb_id << 8) | (payload & 0xffff);
}

/* Queue one verb command in the CORB, advancing the write pointer. */
static int hda_send_corb(uint32_t cmd)
{
    uint16_t wp     = hda_read16(CORBWP) & 0xFF;
    uint16_t new_wp = (wp + 1) % hda_ctrl.corb_entries;
    uint16_t rp     = hda_read16(CORBRP) & 0xFF;
    if (new_wp == rp) return -EAGAIN;
    hda_ctrl.corb_buf[new_wp] = cmd;
    hda_write16(CORBWP, new_wp);
    return 0;
}

/* Wait for and collect the RIRB response for a codec, with timeout. */
static int hda_get_resp_rirb(int addr, uint32_t *res)
{
    int timeout = 50000;
    while (timeout--) {
        uint16_t wp = hda_read16(RIRBWP) & 0xFF;
        if (wp == hda_ctrl.rirb_rp) {
            usleep(2);
            continue;
        }
        while (hda_ctrl.rirb_rp != wp) {
            hda_ctrl.rirb_rp = (hda_ctrl.rirb_rp + 1) % hda_ctrl.rirb_entries;
            int      entry   = hda_ctrl.rirb_rp * 2;
            uint32_t res_ex  = hda_ctrl.rirb_buf[entry + 1];
            uint32_t resp    = hda_ctrl.rirb_buf[entry];
            int      cad     = res_ex & 0xf;
            if (cad < HDA_MAX_CODECS && !(res_ex & (1 << 4))) {
                hda_ctrl.res[cad] = resp;
                hda_ctrl.cmd_count[cad]--;
            }
        }
        if (!hda_ctrl.cmd_count[addr]) {
            if (res) *res = hda_ctrl.res[addr];
            return 0;
        }
    }
    plogk("hda: RIRB response timeout addr=%d (cmd_count=%d)\n", addr, hda_ctrl.cmd_count[addr]);
    return -EIO;
}

/* Execute one verb and track the outstanding command count. */
static int hda_verb_exec(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload, uint32_t *res)
{
    int aidx = addr;
    if (aidx >= HDA_MAX_CODECS) return -EINVAL;
    hda_ctrl.cmd_count[aidx]++;
    int err = hda_send_corb(hda_mk_verb(addr, nid, verb_id, payload));
    if (err) {
        hda_ctrl.cmd_count[aidx]--;
        return err;
    }
    return hda_get_resp_rirb(addr, res);
}

/* Read one codec parameter verb, logging failures. */
static uint32_t hda_get_param(int addr, uint16_t nid, int param)
{
    uint32_t res = 0;
    int      ret = hda_verb_exec(addr, nid, AC_VERB_PARAMETERS, param, &res);
    if (ret) plogk("hda: GET_PARAM addr=%d nid=0x%02x param=0x%02x failed: %d\n", addr, nid, param, ret);
    return res;
}

/* Fire-and-forget verb write. */
static void hda_set_verb(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload)
{
    hda_verb_exec(addr, nid, verb_id, payload, NULL);
}

/* Execute a verb and return its response. */
static uint32_t hda_get_verb(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload)
{
    uint32_t res = 0;
    hda_verb_exec(addr, nid, verb_id, payload, &res);
    return res;
}

/* Controller reset */
static void hda_reset_controller(void)
{
    int timeout;

    hda_write32(INTCTL, 0);
    hda_write16(STATESTS, 0x7fff);

    hda_write32(GCTL, hda_read32(GCTL) & ~AZX_GCTL_RESET);
    timeout = 200;
    while ((hda_read32(GCTL) & AZX_GCTL_RESET) && timeout--) usleep(500);
    if (timeout <= 0) plogk("hda: Timeout waiting for CRST=0\n");

    usleep(2000);

    hda_write32(GCTL, hda_read32(GCTL) | AZX_GCTL_RESET);
    timeout = 200;
    while (!(hda_read32(GCTL) & AZX_GCTL_RESET) && timeout--) usleep(500);
    if (timeout <= 0) plogk("hda: Timeout waiting for CRST=1\n");

    msleep(2);
}

/* CORB/RIRB */
static int hda_alloc_corb_rirb(void)
{
    int      corb_entries = HDA_CORB_ENTRIES;
    int      rirb_entries = HDA_RIRB_ENTRIES;
    size_t   corb_bytes   = (size_t)corb_entries * 4;
    size_t   rirb_bytes   = (size_t)rirb_entries * 8;
    size_t   total        = ALIGN_UP(corb_bytes + rirb_bytes, PAGE_4K_SIZE);
    uint64_t frame        = alloc_frames(total / PAGE_4K_SIZE);
    if (!frame) return -ENOMEM;

    hda_ctrl.corb_phys    = frame;
    hda_ctrl.corb_buf     = (volatile uint32_t *)phys_to_virt(frame);
    hda_ctrl.corb_entries = corb_entries;
    memset((void *)hda_ctrl.corb_buf, 0, corb_bytes);

    hda_ctrl.rirb_phys    = frame + corb_bytes;
    hda_ctrl.rirb_buf     = (volatile uint32_t *)phys_to_virt(frame + corb_bytes);
    hda_ctrl.rirb_entries = rirb_entries;
    memset((void *)hda_ctrl.rirb_buf, 0, rirb_bytes);

    return 0;
}

/* Program the CORB/RIRB base addresses and start their DMA engines. */
static void hda_init_corb_rirb(void)
{
    int timeout;

    hda_write32(CORBLBASE, (uint32_t)(hda_ctrl.corb_phys));
    hda_write32(CORBUBASE, (uint32_t)(hda_ctrl.corb_phys >> 32));
    hda_write8(CORBSIZE, 0x02);
    hda_write16(CORBWP, 0);
    hda_write16(CORBRP, AZX_CORBRP_RST);

    timeout = 100;
    while (timeout--) {
        if (hda_read16(CORBRP) & AZX_CORBRP_RST) break;
        usleep(1);
    }
    hda_write16(CORBRP, 0);
    timeout = 100;
    while (timeout--) {
        if (hda_read16(CORBRP) == 0) break;
        usleep(1);
    }

    hda_write32(RIRBLBASE, (uint32_t)(hda_ctrl.rirb_phys));
    hda_write32(RIRBUBASE, (uint32_t)(hda_ctrl.rirb_phys >> 32));
    hda_write8(RIRBSIZE, 0x02);
    hda_write16(RIRBWP, AZX_RIRBWP_RST);
    hda_write16(RINTCNT, 1);

    hda_write8(CORBCTL, AZX_CORBCTL_RUN);
    hda_write8(RIRBCTL, AZX_RBCTL_DMA_EN | AZX_RBCTL_IRQ_EN);
    hda_write32(GCTL, hda_read32(GCTL) | AZX_GCTL_UNSOL);

    hda_ctrl.rirb_rp = 0;
}

/* Codec probing */
static int hda_probe_codec(int addr)
{
    uint32_t vid = hda_get_param(addr, 0, AC_PAR_VENDOR_ID);
    if (vid == 0 || vid == 0xffffffff) return -ENODEV;

    hda_codec_t *codec = &hda_ctrl.codecs[hda_ctrl.num_codecs];
    memset(codec, 0, sizeof(*codec));
    codec->addr      = addr;
    codec->vendor_id = (vid >> 16) & 0xffff;
    codec->device_id = vid & 0xffff;

    plogk("hda: Codec #%d vendor=0x%04x device=0x%04x\n", addr, codec->vendor_id, codec->device_id);

    hda_ctrl.num_codecs++;
    return 0;
}

/* Enumerate the codec's widget tree and locate DAC/ADC/pin nodes. */
static int hda_parse_widgets(hda_codec_t *codec)
{
    codec->afg_nid   = -1;
    codec->dac_nid   = -1;
    codec->adc_nid   = -1;
    codec->pin_nid   = -1;
    codec->dac_count = 0;
    codec->adc_count = 0;
    codec->pin_count = 0;

    uint32_t root_count  = hda_get_param(codec->addr, 0, AC_PAR_NODE_COUNT);
    uint16_t total_nodes = (uint16_t)(root_count & 0xffff);
    if (total_nodes == 0) {
        plogk("hda: Codec #%d root node count is 0\n", codec->addr);
        return -ENODEV;
    }
    plogk("hda: Codec #%d total nodes: %u\n", codec->addr, total_nodes);

    for (int nid = 1; nid <= total_nodes; nid++) {
        uint32_t ftype = hda_get_param(codec->addr, (uint16_t)nid, AC_PAR_FUNCTION_TYPE);
        if (ftype == AC_GRP_AUDIO_FUNCTION) {
            codec->afg_nid = nid;
            break;
        }
    }

    if (codec->afg_nid < 1) {
        plogk("hda: Codec #%d has no AFG.\n", codec->addr);
        return -ENODEV;
    }

    uint32_t node_count   = hda_get_param(codec->addr, (uint16_t)codec->afg_nid, AC_PAR_NODE_COUNT);
    uint16_t start_nid    = (uint16_t)(node_count >> 16);
    uint16_t widget_total = (uint16_t)(node_count & 0xffff);
    int      count        = (int)widget_total;
    if (count <= 0) {
        plogk("hda: Codec #%d has no widgets.\n", codec->addr);
        return -ENODEV;
    }

    codec->num_widgets = count;
    codec->widgets     = malloc(sizeof(hda_widget_t) * count);
    if (!codec->widgets) return -ENOMEM;
    memset(codec->widgets, 0, sizeof(hda_widget_t) * count);

    for (int i = 0; i < count; i++) {
        uint16_t      nid = start_nid + (uint16_t)i;
        hda_widget_t *w   = &codec->widgets[i];
        w->nid            = nid;
        w->wcap           = hda_get_param(codec->addr, nid, AC_PAR_AUDIO_WIDGET_CAP);
        w->type           = (w->wcap >> AC_WCAP_TYPE_SHIFT) & AC_WCAP_TYPE_MASK;

        if (w->wcap & AC_WCAP_CONN_LIST) {
            uint32_t cl  = hda_get_param(codec->addr, nid, AC_PAR_CONNLIST_LEN);
            w->num_conns = cl & 0xff;
            if (w->num_conns > 0) {
                w->conns = malloc(sizeof(uint16_t) * w->num_conns);
                if (!w->conns) return -ENOMEM;
                for (int j = 0; j < w->num_conns; j += 4) {
                    uint32_t entry  = hda_get_verb(codec->addr, nid, AC_VERB_GET_CONNECT_LIST, (uint32_t)j);
                    int      remain = w->num_conns - j;
                    for (int k = 0; k < 4 && k < remain; k++) w->conns[j + k] = (uint16_t)((entry >> (8 * k)) & 0xff);
                }
            }
        }

        if (w->type == AC_WID_PIN) {
            w->pincap   = hda_get_param(codec->addr, nid, AC_PAR_PIN_CAP);
            w->def_conf = hda_get_verb(codec->addr, nid, AC_VERB_GET_CONFIG_DEFAULT, 0);
            codec->pin_count++;
            if (codec->pin_nid < 0 && (w->pincap & AC_PINCAP_OUT)) codec->pin_nid = nid;
        }

        if (w->type == AC_WID_AUD_OUT) {
            codec->dac_count++;
            if (codec->dac_nid < 0) codec->dac_nid = nid;
        }

        if (w->type == AC_WID_AUD_IN) {
            codec->adc_count++;
            if (codec->adc_nid < 0) codec->adc_nid = nid;
        }
    }

    plogk("hda: Codec #%d widgets: %d (DAC=%d, ADC=%d, PIN=%d)\n", codec->addr, count, codec->dac_count, codec->adc_count, codec->pin_count);
    return 0;
}

/* Codec configuration */
static void hda_config_codec(hda_codec_t *codec)
{
    if (codec->dac_nid < 0 && codec->adc_nid < 0) {
        plogk("hda: Codec #%d has no DAC/ADC, skipping.\n", codec->addr);
        return;
    }

    int addr = codec->addr;

    hda_set_verb(addr, (uint16_t)codec->afg_nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);

    if (codec->dac_nid > 0) hda_set_verb(addr, (uint16_t)codec->dac_nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);

    if (codec->pin_nid > 0) {
        hda_set_verb(addr, (uint16_t)codec->pin_nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);

        uint32_t pincap = 0;
        for (int i = 0; i < codec->num_widgets; i++) {
            if (codec->widgets[i].nid == codec->pin_nid) {
                pincap = codec->widgets[i].pincap;
                break;
            }
        }

        hda_set_verb(addr, (uint16_t)codec->pin_nid, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_OUT_EN | AC_PINCTL_HP_EN);
        if (pincap & AC_PINCAP_EAPD) hda_set_verb(addr, (uint16_t)codec->pin_nid, AC_VERB_SET_EAPD_BTLENABLE, AC_EAPD_BTLENABLE);

        /* Route DAC to PIN through the connection tree */
        int cur_nid = codec->pin_nid;
        int depth   = 10;
        while (depth--) {
            uint32_t clen  = hda_get_param(addr, (uint16_t)cur_nid, AC_PAR_CONNLIST_LEN);
            int      conns = clen & 0xff;
            if (conns <= 0) break;

            uint32_t wcap = hda_get_param(addr, (uint16_t)cur_nid, AC_PAR_AUDIO_WIDGET_CAP);
            int      type = (wcap >> AC_WCAP_TYPE_SHIFT) & AC_WCAP_TYPE_MASK;

            if (type == AC_WID_AUD_SEL || type == AC_WID_AUD_MIX) {
                int found = 0;
                for (int c = 0; c < conns; c++) {
                    uint32_t entry = hda_get_verb(addr, (uint16_t)cur_nid, AC_VERB_GET_CONNECT_LIST, (uint32_t)c);
                    uint16_t conn  = (uint16_t)(entry & 0xff);
                    for (int wi = 0; wi < codec->num_widgets; wi++) {
                        if (codec->widgets[wi].nid == conn && codec->widgets[wi].type == AC_WID_AUD_OUT) {
                            hda_set_verb(addr, (uint16_t)cur_nid, AC_VERB_SET_CONNECT_SEL, (uint32_t)c);
                            found = 1;
                            break;
                        }
                    }
                    if (found) break;
                }
                if (!found && type == AC_WID_AUD_SEL && conns > 0) hda_set_verb(addr, (uint16_t)cur_nid, AC_VERB_SET_CONNECT_SEL, 0);
            }

            if (conns > 0 && type != AC_WID_AUD_OUT) {
                uint32_t fc   = hda_get_verb(addr, (uint16_t)cur_nid, AC_VERB_GET_CONNECT_LIST, 0);
                uint16_t prev = (uint16_t)(fc & 0xff);
                if (prev == cur_nid || prev == 0) break;
                cur_nid = prev;
            } else {
                break;
            }
        }

        if (codec->dac_nid > 0) {
            hda_set_verb(addr, (uint16_t)codec->dac_nid, AC_VERB_SET_AMP_GAIN_MUTE,
                         AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AC_AMP_SET_UNMUTE | AC_AMP_SET_GAIN(0x4c));
            hda_set_verb(addr, (uint16_t)codec->dac_nid, AC_VERB_SET_AMP_GAIN_MUTE,
                         AC_AMP_SET_OUTPUT | AC_AMP_SET_RIGHT | AC_AMP_SET_UNMUTE | AC_AMP_SET_GAIN(0x4c));
        }
    }

    /* Configure ADC for capture if available */
    if (codec->adc_nid > 0 && codec->pin_nid > 0) {
        hda_set_verb(addr, (uint16_t)codec->adc_nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);

        /* Route pin to ADC */
        int      cur_nid = codec->adc_nid;
        uint32_t clen    = hda_get_param(addr, (uint16_t)cur_nid, AC_PAR_CONNLIST_LEN);
        int      conns   = clen & 0xff;
        if (conns > 0) {
            for (int c = 0; c < conns; c++) {
                uint32_t entry = hda_get_verb(addr, (uint16_t)cur_nid, AC_VERB_GET_CONNECT_LIST, (uint32_t)c);
                uint16_t conn  = (uint16_t)(entry & 0xff);
                for (int wi = 0; wi < codec->num_widgets; wi++) {
                    if (codec->widgets[wi].nid == conn) {
                        hda_set_verb(addr, (uint16_t)cur_nid, AC_VERB_SET_CONNECT_SEL, (uint32_t)c);
                        break;
                    }
                }
            }
        }
    }
}

/* Stream DMA with multi-BDL entries */
static int hda_setup_stream(int stream_idx, uint32_t format, size_t buf_size, size_t period_bytes, int direction)
{
    if (stream_idx >= HDA_MAX_STREAMS || hda_ctrl.streams[stream_idx].allocated) return -EBUSY;

    size_t period_frags = HDA_BDL_ENTRIES;
    size_t bdl_entries  = HDA_BDL_ENTRIES;
    if (period_bytes > 0 && period_bytes <= buf_size) {
        period_frags = buf_size / period_bytes;
        if (period_frags > HDA_BDL_ENTRIES) period_frags = HDA_BDL_ENTRIES;
        if (period_frags == 0) period_frags = 1;
        bdl_entries = period_frags;
    }

    size_t   bdl_bytes = sizeof(hda_bdle_t) * bdl_entries;
    size_t   total     = ALIGN_UP(buf_size + bdl_bytes, PAGE_4K_SIZE);
    uint64_t frame     = alloc_frames(total / PAGE_4K_SIZE);
    if (!frame) {
        plogk("hda: Stream %u DMA buffer allocation failed (%zu bytes)\n", stream_idx, buf_size);
        return -ENOMEM;
    }

    hda_bdle_t *bdl = (hda_bdle_t *)phys_to_virt(frame + buf_size);

    hda_ctrl.streams[stream_idx].allocated    = 1;
    hda_ctrl.streams[stream_idx].direction    = direction;
    hda_ctrl.streams[stream_idx].buf_phys     = frame;
    hda_ctrl.streams[stream_idx].buf          = (volatile uint8_t *)phys_to_virt(frame);
    hda_ctrl.streams[stream_idx].buf_size     = buf_size;
    hda_ctrl.streams[stream_idx].bdl_phys     = frame + buf_size;
    hda_ctrl.streams[stream_idx].bdl          = (volatile void *)bdl;
    hda_ctrl.streams[stream_idx].running      = 0;
    hda_ctrl.streams[stream_idx].period_frags = (int)period_frags;
    hda_ctrl.streams[stream_idx].hw_pos       = 0;

    memset((void *)hda_ctrl.streams[stream_idx].buf, 0, buf_size);
    memset(bdl, 0, bdl_bytes);

    /* Set up BDL with fragment entries */
    size_t frag_size = buf_size / bdl_entries;
    if (frag_size == 0) frag_size = buf_size;

    for (size_t i = 0; i < bdl_entries; i++) {
        uint64_t frag_phys = frame + i * frag_size;
        size_t   frag_sz   = (i == bdl_entries - 1) ? buf_size - i * frag_size : frag_size;

        bdl[i].addr_low  = (uint32_t)(frag_phys & 0xFFFFFFFF);
        bdl[i].addr_high = (uint32_t)(frag_phys >> 32);
        bdl[i].length    = (uint32_t)frag_sz;
        bdl[i].ioc       = 1; // Interrupt on completion for each fragment
    }

    sd_write32(stream_idx, SD_CBL, (uint32_t)buf_size);
    sd_write16(stream_idx, SD_LVI, (uint16_t)(bdl_entries - 1));
    sd_write16(stream_idx, SD_FORMAT, (uint16_t)format);
    sd_write32(stream_idx, SD_BDLPL, (uint32_t)(hda_ctrl.streams[stream_idx].bdl_phys & 0xFFFFFFFF));
    sd_write32(stream_idx, SD_BDLPU, (uint32_t)(hda_ctrl.streams[stream_idx].bdl_phys >> 32));
    sd_write8(stream_idx, SD_CTL, 0);

    return 0;
}

/* Start DMA on a stream and enable its interrupts. */
static void hda_start_stream(int stream_idx)
{
    if (stream_idx >= HDA_MAX_STREAMS || !hda_ctrl.streams[stream_idx].allocated) return;
    if (hda_ctrl.streams[stream_idx].running) return;

    /* Clear stream status */
    sd_write8(stream_idx, SD_STS, 0x1c);

    hda_write32(INTCTL, hda_read32(INTCTL) | (1u << stream_idx));
    sd_write8(stream_idx, SD_CTL, SD_CTL_DMA_START | SD_INT_MASK);
    hda_ctrl.streams[stream_idx].running = 1;
}

/* Stop DMA on a stream and release its descriptor. */
static void hda_stop_stream(int stream_idx)
{
    if (stream_idx >= HDA_MAX_STREAMS || !hda_ctrl.streams[stream_idx].allocated) return;
    sd_write8(stream_idx, SD_CTL, 0);
    sd_write8(stream_idx, SD_STS, 0x1c);
    hda_write32(INTCTL, hda_read32(INTCTL) & ~(1u << stream_idx));
    hda_ctrl.streams[stream_idx].running   = 0;
    hda_ctrl.streams[stream_idx].allocated = 0;
}

/* Audio subsystem interface */
/* Find a free stream descriptor for the given direction. */
static int hda_allocate_stream(int direction)
{
    for (int s = 0; s < HDA_MAX_STREAMS; s++) {
        if (!hda_ctrl.streams[s].allocated) {
            if (direction == 0)
                hda_ctrl.playback_stream = s;
            else
                hda_ctrl.capture_stream = s;
            return s;
        }
    }
    return -EBUSY;
}

/* audio callback: start both allocated streams. */
static int hda_audio_start(audio_card_t *card)
{
    (void)card;
    spin_lock(&hda_ctrl.lock);
    if (hda_ctrl.playback_stream >= 0 && hda_ctrl.streams[hda_ctrl.playback_stream].allocated) hda_start_stream(hda_ctrl.playback_stream);
    if (hda_ctrl.capture_stream >= 0 && hda_ctrl.streams[hda_ctrl.capture_stream].allocated) hda_start_stream(hda_ctrl.capture_stream);
    spin_unlock(&hda_ctrl.lock);
    return EOK;
}

/* audio callback: stop both streams. */
static int hda_audio_stop(audio_card_t *card)
{
    (void)card;
    spin_lock(&hda_ctrl.lock);
    if (hda_ctrl.playback_stream >= 0) hda_stop_stream(hda_ctrl.playback_stream);
    if (hda_ctrl.capture_stream >= 0) hda_stop_stream(hda_ctrl.capture_stream);
    spin_unlock(&hda_ctrl.lock);
    return EOK;
}

/* audio callback: wait for playback DMA to finish before stopping. */
static int hda_audio_drain(audio_card_t *card)
{
    (void)card;

    /* Wait for all pending DMA to complete */
    spin_lock(&hda_ctrl.lock);
    if (hda_ctrl.playback_stream >= 0 && hda_ctrl.streams[hda_ctrl.playback_stream].running) {
        /* Let the BDL finish, then stop */
        int timeout = 50000;
        while (timeout--) {
            uint32_t lpib = sd_read32(hda_ctrl.playback_stream, SD_LPIB);
            if (lpib >= hda_ctrl.streams[hda_ctrl.playback_stream].buf_size) break;
            usleep(10);
        }
        if (timeout < 0) {
            plogk("hda: Playback stream %d drain timed out (LPIB=0x%x)\n", hda_ctrl.playback_stream,
                  sd_read32(hda_ctrl.playback_stream, SD_LPIB));
        }
        hda_stop_stream(hda_ctrl.playback_stream);
    }
    spin_unlock(&hda_ctrl.lock);
    return EOK;
}

/* audio callback: copy a chunk into the playback DMA buffer. */
static size_t hda_audio_write(audio_card_t *card, const void *addr, size_t offset, size_t size)
{
    (void)card;
    (void)offset;
    if (hda_ctrl.playback_stream < 0) return 0;

    spin_lock(&hda_ctrl.lock);
    if (!hda_ctrl.streams[hda_ctrl.playback_stream].allocated) {
        spin_unlock(&hda_ctrl.lock);
        return 0;
    }

    size_t buf_sz  = hda_ctrl.streams[hda_ctrl.playback_stream].buf_size;
    size_t to_copy = (size > buf_sz) ? buf_sz : size;

    memcpy((void *)hda_ctrl.streams[hda_ctrl.playback_stream].buf, addr, to_copy);

    spin_unlock(&hda_ctrl.lock);
    return to_copy;
}

/* audio callback: copy a chunk out of the capture DMA buffer. */
static size_t hda_audio_read(audio_card_t *card, void *addr, size_t offset, size_t size)
{
    (void)card;
    (void)offset;
    if (hda_ctrl.capture_stream < 0) return 0;

    spin_lock(&hda_ctrl.lock);
    if (!hda_ctrl.streams[hda_ctrl.capture_stream].allocated) {
        spin_unlock(&hda_ctrl.lock);
        return 0;
    }

    size_t buf_sz  = hda_ctrl.streams[hda_ctrl.capture_stream].buf_size;
    size_t to_copy = (size > buf_sz) ? buf_sz : size;

    memcpy(addr, (const void *)hda_ctrl.streams[hda_ctrl.capture_stream].buf, to_copy);

    spin_unlock(&hda_ctrl.lock);
    return to_copy;
}

/* audio callback: validate and store the PCM format. */
static int hda_audio_set_format(audio_card_t *card, const audio_pcm_format_t *format)
{
    if (!card || !format) return -EINVAL;
    if (format->channels < 1 || format->channels > 8) return -EINVAL;
    if (format->bits != 8 && format->bits != 16 && format->bits != 24 && format->bits != 32) return -EINVAL;
    if (format->sample_rate < 8000 || format->sample_rate > 192000) return -EINVAL;

    spin_lock(&hda_ctrl.lock);
    card->format       = *format;
    hda_ctrl.audio_fmt = *format;
    spin_unlock(&hda_ctrl.lock);
    return EOK;
}

/* audio callback: program format verbs and set up the DMA streams. */
static int hda_audio_set_params(audio_card_t *card, const audio_pcm_format_t *fmt, size_t buffer_bytes, size_t period_bytes)
{
    if (!card || !fmt) return -EINVAL;
    spin_lock(&hda_ctrl.lock);

    card->format          = *fmt;
    hda_ctrl.audio_fmt    = *fmt;
    hda_ctrl.buffer_bytes = buffer_bytes;
    hda_ctrl.period_bytes = period_bytes;

    uint16_t fmt_val = (uint16_t)((fmt->channels - 1) & 0xf);
    switch (fmt->bits) {
        case 8 :
            fmt_val |= (uint16_t)(0 << 4);
            break;
        case 16 :
            fmt_val |= (uint16_t)(1 << 4);
            break;
        case 24 :
            fmt_val |= (uint16_t)(3 << 4);
            break;
        case 32 :
            fmt_val |= (uint16_t)(4 << 4);
            break;
        default :
            break;
    }

    uint32_t base = AC_FMT_BASE_RATE;
    uint32_t rate = fmt->sample_rate;
    uint32_t mult = rate / base;
    if (rate % base == 0 && mult >= 1 && mult <= 4) {
        fmt_val |= (uint16_t)((mult - 1) << 11);
    } else if (base % rate == 0 && base / rate <= 8) {
        fmt_val |= (uint16_t)((base / rate - 1) << 8);
    }

    /* Set format verbs for all codecs */
    for (int c = 0; c < hda_ctrl.num_codecs; c++) {
        hda_codec_t *codec = &hda_ctrl.codecs[c];
        if (codec->dac_nid > 0) {
            hda_set_verb(codec->addr, (uint16_t)codec->dac_nid, AC_VERB_SET_STREAM_FORMAT, fmt_val);
            hda_set_verb(codec->addr, (uint16_t)codec->dac_nid, AC_VERB_SET_CHANNEL_STREAMID, 0);
        }
        if (codec->adc_nid > 0) {
            hda_set_verb(codec->addr, (uint16_t)codec->adc_nid, AC_VERB_SET_STREAM_FORMAT, fmt_val);
            hda_set_verb(codec->addr, (uint16_t)codec->adc_nid, AC_VERB_SET_CHANNEL_STREAMID, 1);
        }
    }

    /* Set up playback stream */
    if (hda_ctrl.playback_stream >= 0) {
        hda_stop_stream(hda_ctrl.playback_stream);
        hda_ctrl.playback_stream = -1;
    }

    int ps = hda_allocate_stream(0);
    if (ps >= 0) {
        int err = hda_setup_stream(ps, fmt_val, buffer_bytes, period_bytes, 0);
        if (err) {
            plogk("hda: Playback stream %d setup failed: %d\n", ps, err);
            spin_unlock(&hda_ctrl.lock);
            return err;
        }
    } else {
        plogk("hda: No playback stream available.\n");
        spin_unlock(&hda_ctrl.lock);
        return ps;
    }

    /* Set up capture stream (if ADC available) */
    if (hda_ctrl.capture_stream >= 0) {
        hda_stop_stream(hda_ctrl.capture_stream);
        hda_ctrl.capture_stream = -1;
    }

    int has_adc = 0;
    for (int c = 0; c < hda_ctrl.num_codecs; c++) {
        if (hda_ctrl.codecs[c].adc_nid > 0) {
            has_adc = 1;
            break;
        }
    }

    if (has_adc) {
        int cs = hda_allocate_stream(1);
        if (cs >= 0) {
            int err = hda_setup_stream(cs, fmt_val, buffer_bytes, period_bytes, 1);
            if (err) plogk("hda: Capture stream %d setup failed: %d\n", cs, err);
        } else {
            plogk("hda: No capture stream available.\n");
        }
    }

    spin_unlock(&hda_ctrl.lock);
    return EOK;
}

/* audio callback: write DAC gain for left and right channels. */
static int hda_audio_set_volume(audio_card_t *card, const audio_volume_t *volume)
{
    if (!card || !volume) return -EINVAL;
    for (int c = 0; c < hda_ctrl.num_codecs; c++) {
        hda_codec_t *codec = &hda_ctrl.codecs[c];
        if (codec->dac_nid > 0) {
            uint32_t gl = ((uint32_t)volume->left * 0x7f) / 255;
            uint32_t gr = ((uint32_t)volume->right * 0x7f) / 255;
            hda_set_verb(codec->addr, (uint16_t)codec->dac_nid, AC_VERB_SET_AMP_GAIN_MUTE,
                         AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AC_AMP_SET_GAIN(gl));
            hda_set_verb(codec->addr, (uint16_t)codec->dac_nid, AC_VERB_SET_AMP_GAIN_MUTE,
                         AC_AMP_SET_OUTPUT | AC_AMP_SET_RIGHT | AC_AMP_SET_GAIN(gr));
        }
    }
    return EOK;
}

/* audio callback: read DAC gain from the first available codec. */
static int hda_audio_get_volume(audio_card_t *card, audio_volume_t *volume)
{
    if (!card || !volume) return -EINVAL;

    /* Read volume from first available codec's DAC */
    for (int c = 0; c < hda_ctrl.num_codecs; c++) {
        hda_codec_t *codec = &hda_ctrl.codecs[c];
        if (codec->dac_nid > 0) {
            uint32_t vl   = hda_get_verb(codec->addr, (uint16_t)codec->dac_nid, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_OUTPUT | AC_AMP_GET_LEFT);
            uint32_t vr   = hda_get_verb(codec->addr, (uint16_t)codec->dac_nid, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_OUTPUT | AC_AMP_GET_RIGHT);
            volume->left  = (uint8_t)((vl & 0x7f) * 255 / 0x7f);
            volume->right = (uint8_t)((vr & 0x7f) * 255 / 0x7f);
            return EOK;
        }
    }

    volume->left  = 0x80;
    volume->right = 0x80;
    return EOK;
}

/* audio callback: report the hardware playback position in frames. */
static int hda_audio_get_position(audio_card_t *card, snd_pcm_uframes_t *pos)
{
    (void)card;
    if (!pos) return -EINVAL;

    spin_lock(&hda_ctrl.lock);
    if (hda_ctrl.playback_stream >= 0 && hda_ctrl.streams[hda_ctrl.playback_stream].running) {
        uint32_t lpib = sd_read32(hda_ctrl.playback_stream, SD_LPIB);
        size_t   fb   = (size_t)(hda_ctrl.audio_fmt.bits / 8) * hda_ctrl.audio_fmt.channels;
        if (fb > 0) *pos = lpib / fb;
    }
    spin_unlock(&hda_ctrl.lock);
    return EOK;
}

/* audio callback: report free space left in the playback buffer. */
static int hda_audio_get_avail(audio_card_t *card, size_t *avail)
{
    (void)card;
    if (!avail) return -EINVAL;
    spin_lock(&hda_ctrl.lock);
    if (hda_ctrl.playback_stream >= 0 && hda_ctrl.streams[hda_ctrl.playback_stream].allocated) {
        uint32_t lpib   = sd_read32(hda_ctrl.playback_stream, SD_LPIB);
        size_t   buf_sz = hda_ctrl.streams[hda_ctrl.playback_stream].buf_size;
        if (lpib > buf_sz)
            *avail = 0;
        else
            *avail = buf_sz - lpib;
    } else {
        *avail = 0;
    }
    spin_unlock(&hda_ctrl.lock);
    return EOK;
}

static const audio_card_ops_t hda_audio_ops = {
    .pcm_read     = hda_audio_read,
    .pcm_write    = hda_audio_write,
    .set_format   = hda_audio_set_format,
    .start        = hda_audio_start,
    .stop         = hda_audio_stop,
    .drain        = hda_audio_drain,
    .set_volume   = hda_audio_set_volume,
    .get_volume   = hda_audio_get_volume,
    .get_position = hda_audio_get_position,
    .get_avail    = hda_audio_get_avail,
    .set_params   = hda_audio_set_params,
};

/* Interrupt handler */
static void hda_interrupt_handler(interrupt_frame_t *frame)
{
    (void)frame;
    uint32_t intsts = hda_read32(INTSTS);
    if (intsts == 0 || intsts == 0xffffffff) return;

    /* Handle RIRB response interrupt */
    uint8_t rirb_sts = hda_read8(RIRBSTS);
    if (rirb_sts & 0x05) {
        hda_write8(RIRBSTS, rirb_sts);
        if (rirb_sts & 0x01) hda_read16(RIRBWP);
    }

    /* Handle stream interrupts */
    for (int s = 0; s < HDA_MAX_STREAMS; s++) {
        if (!(intsts & (1u << s))) continue;
        if (!hda_ctrl.streams[s].allocated) continue;

        uint8_t sts = sd_read8(s, SD_STS);
        if (sts & SD_STS_DMA_COMPLETE) {
            /* DMA completed a BDL entry - update position */
            hda_ctrl.streams[s].hw_pos += hda_ctrl.streams[s].buf_size / hda_ctrl.streams[s].period_frags;

            if (hda_ctrl.streams[s].hw_pos >= hda_ctrl.streams[s].buf_size) hda_ctrl.streams[s].hw_pos = 0;

            /* Notify the PCM layer */
            if (hda_ctrl.streams[s].pcm_file) {
                audio_pcm_file_t *pf = hda_ctrl.streams[s].pcm_file;
                spin_lock(&pf->lock);
                snd_pcm_uframes_t frames_consumed = hda_ctrl.streams[s].buf_size
                                                    / ((size_t)(hda_ctrl.audio_fmt.bits / 8) * hda_ctrl.audio_fmt.channels)
                                                    / hda_ctrl.streams[s].period_frags;

                pcm_ring_buffer_advance_hw(pf, frames_consumed);
                pf->period_event = 1;
                spin_unlock(&pf->lock);
                wait_queue_wake_all(&pf->write_wait);
                wait_queue_wake_all(&pf->read_wait);
            }

            /* Clear status bit */
            sd_write8(s, SD_STS, SD_STS_DMA_COMPLETE);
        }

        if (sts & SD_STS_FIFO_ERROR) {
            plogk("hda: Stream %d FIFO error (sts=0x%02x)\n", s, sts);
            sd_write8(s, SD_STS, SD_STS_FIFO_ERROR);
        }
        if (sts & SD_STS_DESC_ERROR) {
            plogk("hda: Stream %d descriptor error (sts=0x%02x)\n", s, sts);
            sd_write8(s, SD_STS, SD_STS_DESC_ERROR);
        }
    }
}

/* Initialization */
void hda_init(void)
{
#if !CONFIG_SOUND_HDA
    return;
#endif
    pci_device_cache_t  *dev;
    pci_device_request_t req;
    uint32_t             irq;

    memset(&hda_ctrl, 0, sizeof(hda_ctrl));
    hda_ctrl.playback_stream = -1;
    hda_ctrl.capture_stream  = -1;

    req.vendor_id = PCI_VENDOR_INTEL;
    req.device_id = 0;
    dev           = NULL;
    while (1) {
        dev = pci_found_device_cache(dev, req);
        if (!dev) break;
        if ((dev->class_code & 0xffffff00) == PCI_CLASS_HDA) break;
    }
    if (!dev) {
        pci_class_request_t class_req = {.class_code = PCI_CLASS_HDA};
        dev                           = pci_found_class_cache(NULL, class_req);
    }
    if (!dev) return;

    plogk("hda: Controller at PCI %04x:%02x:%02x.%01x, vendor 0x%04x, device 0x%04x\n", dev->device->domain, dev->device->bus, dev->device->slot,
          dev->device->func, dev->vendor_id, dev->device_id);

    {
        pci_device_reg_t bar_reg  = {.parent = dev, .offset = 0x10};
        uint64_t         bar_phys = read_pci(bar_reg);

        if (bar_phys == 0xFFFFFFFF || !(bar_phys & 0xFFFFFFF0)) {
            plogk("hda: BAR0 is invalid.\n");
            return;
        }

        if ((bar_phys & 1) || (((bar_phys >> 1) & 0b11) == BAR_Reserved)) {
            plogk("hda: BAR0 is I/O BAR, not supported.\n");
            return;
        }

        uint32_t bar_type = (bar_phys >> 1) & 0b11;
        if (bar_type == BAR_S64) {
            bar_reg.offset    = 0x14;
            uint32_t bar_high = read_pci(bar_reg);
            if (bar_high != 0xFFFFFFFF) bar_phys |= (uint64_t)bar_high << 32;
        }
        bar_phys &= ~0xFULL;

        hda_ctrl.mmio_size = hda_read_bar_size(dev, 0);
        if (!hda_ctrl.mmio_size || hda_ctrl.mmio_size == 0xFFFFFFFF) hda_ctrl.mmio_size = 0x4000;

        {
            uint64_t map_start = bar_phys & ~(uint64_t)0xFFF;
            uint64_t map_len   = (bar_phys + hda_ctrl.mmio_size + 0xFFF) & ~(uint64_t)0xFFF;
            map_len -= map_start;
            page_map_range_to(get_kernel_pagedir(), map_start, map_len, PTE_MMIO_FLAGS);
        }

        hda_ctrl.mmio = (volatile void *)phys_to_virt(bar_phys);
    }

    irq = pci_get_irq(dev);

    plogk("hda: MMIO base %p, size %u, IRQ %u\n", hda_ctrl.mmio, hda_ctrl.mmio_size, irq);

    hda_ctrl.pci_dev = dev;
    hda_ctrl.irq     = irq;

    {
        uint32_t gcap  = hda_read32(GCAP);
        uint8_t  major = hda_read8(VMAJ);
        uint8_t  minor = hda_read8(VMIN);
        int      iss   = (int)((gcap >> 8) & 0x0f);
        int      oss   = (int)((gcap >> 12) & 0x0f);
        int      bss   = (int)((gcap >> 3) & 0x1f);
        plogk("hda: GCAP=0x%08x rev %d.%d, ISS=%d, OSS=%d, BSS=%d\n", gcap, major, minor, iss, oss, bss);
    }

    {
        uint32_t cmd = pci_read_command_status(dev) & 0xFFFF;
        cmd |= (1u << 1) | (1u << 2);
        pci_write_command_status(dev, cmd);
    }

    hda_reset_controller();

    if (hda_alloc_corb_rirb()) {
        plogk("hda: CORB/RIRB allocation failed.\n");
        return;
    }
    hda_init_corb_rirb();
    plogk("hda: CORB/RIRB configured, %d entries each.\n", hda_ctrl.corb_entries);

    register_interrupt_handler(IRQ_0 + irq, hda_interrupt_handler, 0, 0x8e);
    hda_write32(INTCTL, AZX_INT_GLOBAL_EN | AZX_INT_CTRL_EN);

    uint16_t state_sts  = 0;
    int      codec_mask = 0;
    for (int retry = 0; retry < 6; retry++) {
        int delay = (2 << retry);
        msleep(delay);
        state_sts = hda_read16(STATESTS);
        plogk("hda: STATESTS=0x%04x (retry %d, delay %dms)\n", state_sts, retry + 1, delay);
        codec_mask = state_sts & 0x0f;
        if (codec_mask) break;
    }

    if (!codec_mask) {
        plogk("hda: STATESTS=0x0000 after all retries, probing all codec addresses.\n");
        codec_mask = 0x0f;
    }

    for (int i = 0; i < HDA_MAX_CODECS; i++) {
        if (codec_mask & (1u << i)) {
            if (hda_probe_codec(i) == 0) {
                hda_codec_t *codec = &hda_ctrl.codecs[hda_ctrl.num_codecs - 1];
                if (hda_parse_widgets(codec) != 0) {
                    if (codec->widgets) {
                        for (int wi = 0; wi < codec->num_widgets; wi++)
                            if (codec->widgets[wi].conns) free(codec->widgets[wi].conns);
                        free(codec->widgets);
                    }
                    hda_ctrl.num_codecs--;
                }
            }
        }
    }

    if (hda_ctrl.num_codecs == 0) {
        plogk("hda: HDA controller initialized, no codecs present (STATESTS=0x%04x)\n", state_sts);
        return;
    }

    for (int c = 0; c < hda_ctrl.num_codecs; c++) hda_config_codec(&hda_ctrl.codecs[c]);

    audio_pcm_format_t fmt = {.sample_rate = 48000, .bits = 16, .channels = 2};
    hda_ctrl.audio_fmt     = fmt;
    hda_ctrl.buffer_bytes  = (size_t)HDA_DMA_BUFFER_SIZE;
    hda_ctrl.period_bytes  = HDA_DMA_BUFFER_SIZE / HDA_PERIOD_FRAGS;

    audio_register_card("Intel HD Audio", &fmt, &hda_audio_ops, &hda_ctrl);
    hda_ctrl.audio_registered = 1;
}
