/*
 *
 *      hda.c
 *      Intel HD Audio driver (ported from Linux sound/hda/)
 *
 *      2026/7/23 By MicroFish
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <drivers/pci.h>
#include <kernel/audio.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

#define HDA_MAX_CODECS      8
#define HDA_MAX_STREAMS     8
#define HDA_BDL_ENTRIES     256
#define HDA_DMA_BUFFER_SIZE (64 * 1024)

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

/* Stream descriptor offsets (relative to stream base 0x80 + idx * 0x20) */
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
#define AZX_CORBCTL_RUN      (1 << 0)
#define AZX_RIRBWP_RST       (1 << 15)
#define AZX_RBCTL_DMA_EN     (1 << 0)
#define AZX_RBCTL_IRQ_EN     (1 << 1)
#define AZX_MAX_CORB_ENTRIES 256
#define AZX_MAX_RIRB_ENTRIES 256

/* IRS bits */
#define AZX_IRS_BUSY  (1 << 0)
#define AZX_IRS_VALID (1 << 1)

/* Stream control bits */
#define SD_CTL_DMA_START (1 << 0)
#define SD_INT_MASK      (1 << 2)

/* BDL descriptor */
struct hda_bdle {
        uint32_t addr_low;
        uint32_t addr_high;
        uint32_t length;
        uint32_t ioc;
} __attribute__((packed));

/* Codec verbs */
#define AC_VERB_PARAMETERS             0xf00
#define AC_VERB_GET_CONFIG_DEFAULT     0xf1c
#define AC_VERB_GET_CONNECT_LIST       0xf02
#define AC_VERB_SET_STREAM_FORMAT      0x200
#define AC_VERB_SET_CHANNEL_STREAMID   0x600
#define AC_VERB_SET_AMP_GAIN_MUTE      0x300
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
#define AC_PINCTL_OUT_EN (1 << 7)
#define AC_PINCTL_IN_EN  (1 << 5)

/* EAPD verb payload */
#define AC_EAPD_BTLENABLE (1 << 0)
#define AC_EAPD_BALANCED  (1 << 1)
#define AC_EAPD_LR_SWAP   (1 << 2)
#define AC_EAPD_BTL_SET   (1 << 3)

/* AMP set verb payload layout (16-bit):
 *   bits 7:0   = gain (0-127)
 *   bits 9:8   = connection index (0-3)
 *   bit  10    = left (1) / right (0)
 *   bit  11    = input (1) / output (0)
 *   bit  12    = reserved
 *   bit  13    = reserved
 *   bit  14    = reserved
 *   bit  15    = mute (1=mute)
 */
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
#define AC_WCAP_CONN_LIST  (1 << 10)
#define AC_WCAP_DIGITAL    (1 << 11)
#define AC_WCAP_POWER      (1 << 13)
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
#define AC_FMT_BITS_MASK  (7 << 4)
#define AC_FMT_BITS_8     (0 << 4)
#define AC_FMT_BITS_16    (1 << 4)
#define AC_FMT_BITS_24    (3 << 4)
#define AC_FMT_BITS_32    (4 << 4)
#define AC_FMT_CHAN_SHIFT 0
#define AC_FMT_CHAN_MASK  0x0f
#define AC_FMT_DIV_SHIFT  8
#define AC_FMT_DIV_MASK   (7 << 8)
#define AC_FMT_MULT_SHIFT 11
#define AC_FMT_MULT_MASK  (7 << 11)
#define AC_FMT_BASE_RATE  48000

/* PCI vendor/device IDs */
#define PCI_VENDOR_INTEL 0x8086
#define PCI_CLASS_HDA    0x040300

/* HDA codec widget descriptor */
struct hda_widget {
        uint16_t  nid;
        uint8_t   type;
        uint32_t  wcap;
        uint32_t  pincap;
        uint32_t  def_conf;
        uint8_t   num_conns;
        uint16_t *conns;
};

/* HDA codec descriptor */
struct hda_codec {
        uint8_t            addr;
        uint16_t           vendor_id;
        uint16_t           device_id;
        uint16_t           start_nid;
        uint16_t           end_nid;
        int                afg_nid;
        int                num_widgets;
        struct hda_widget *widgets;
        int                dac_nid;
        int                pin_nid;
};

/* HDA controller state */
struct hda_controller {
        int                 found;
        volatile void      *mmio;
        pci_device_cache_t *pci_dev;
        uint32_t            irq;
        int                 num_playback;
        int                 num_capture;

        struct hda_codec codecs[HDA_MAX_CODECS];
        int              num_codecs;

        spinlock_t lock;

        /* CORB/RIRB DMA buffers */
        volatile void *corb_buf;
        volatile void *rirb_buf;
        uint64_t       corb_phys;
        uint64_t       rirb_phys;
        int            rirb_rp;
        int            use_pio;
        int            cmd_count[HDA_MAX_CODECS];
        uint32_t       res[HDA_MAX_CODECS];

        /* Streams */
        struct {
                int            allocated;
                volatile void *buf;
                uint64_t       buf_phys;
                size_t         buf_size;
                volatile void *bdl;
                uint64_t       bdl_phys;
                int            running;
        } streams[HDA_MAX_STREAMS];

        /* Audio interface */
        audio_pcm_format_t audio_fmt;
        int                audio_registered;
};

static struct hda_controller hda_ctrl;

/* ------------------------------------------------------------------ */
/* MMIO access helpers                                                */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* Verb helpers                                                       */
/* ------------------------------------------------------------------ */

/* Encode a verb command: codec_addr<<28 | nid<<20 | verb_id<<8 | payload */
static inline uint32_t hda_mk_verb(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload)
{
    return ((uint32_t)(addr & 0xf) << 28) | ((uint32_t)(nid & 0xff) << 20) | (verb_id << 8) | (payload & 0xffff);
}

/* Send a verb via PIO (immediate command) */
static int hda_send_cmd_pio(uint32_t cmd)
{
    int timeout = 2000;
    while (timeout--) {
        if (!(hda_read16(IRS) & AZX_IRS_BUSY)) {
            hda_write16(IRS, hda_read16(IRS) | AZX_IRS_VALID);
            hda_write32(IC, cmd);
            hda_write16(IRS, hda_read16(IRS) | AZX_IRS_BUSY);
            return 0;
        }
        usleep(1);
    }
    return -EIO;
}

static int hda_get_resp_pio(uint32_t *res)
{
    int timeout = 2000;
    while (timeout--) {
        if (hda_read16(IRS) & AZX_IRS_VALID) {
            *res = hda_read32(IR);
            return 0;
        }
        usleep(1);
    }
    *res = (uint32_t)-1;
    return -EIO;
}

static int hda_exec_verb_pio(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload, uint32_t *res)
{
    uint32_t cmd = hda_mk_verb(addr, nid, verb_id, payload);
    int      err = hda_send_cmd_pio(cmd);
    if (err) return err;
    if (res) return hda_get_resp_pio(res);
    return 0;
}

/* CORB-based command sending */
static int hda_send_corb(uint32_t cmd)
{
    uint16_t wp = hda_read16(CORBWP);
    if (wp == 0xffff) return -EIO;
    wp          = (wp + 1) % AZX_MAX_CORB_ENTRIES;
    uint16_t rp = hda_read16(CORBRP);
    if (wp == rp) return -EAGAIN;
    *((volatile uint32_t *)hda_ctrl.corb_buf + wp) = cmd;
    hda_write16(CORBWP, wp);
    return 0;
}

static int hda_get_resp_rirb(int addr, uint32_t *res)
{
    int timeout = 50000;
    while (timeout--) {
        uint16_t wp = hda_read16(RIRBWP);
        if (wp == hda_ctrl.rirb_rp) {
            usleep(2);
            continue;
        }
        while (hda_ctrl.rirb_rp != wp) {
            hda_ctrl.rirb_rp = (hda_ctrl.rirb_rp + 1) % AZX_MAX_RIRB_ENTRIES;
            int      entry   = hda_ctrl.rirb_rp * 2;
            uint32_t res_ex  = *((volatile uint32_t *)hda_ctrl.rirb_buf + entry + 1);
            uint32_t resp    = *((volatile uint32_t *)hda_ctrl.rirb_buf + entry);
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
    return -EIO;
}

/* High-level verb exec: uses CORB or PIO depending on mode */
static int hda_verb_exec(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload, uint32_t *res)
{
    uint32_t cmd = hda_mk_verb(addr, nid, verb_id, payload);
    if (hda_ctrl.use_pio) return hda_exec_verb_pio(addr, nid, verb_id, payload, res);

    int aidx = addr;
    hda_ctrl.cmd_count[aidx]++;
    int err = hda_send_corb(cmd);
    if (err) {
        hda_ctrl.cmd_count[aidx]--;
        return err;
    }
    return hda_get_resp_rirb(addr, res);
}

/* Convenience: read a parameter from a node */
static uint32_t hda_get_param(int addr, uint16_t nid, int param)
{
    uint32_t res = 0;
    hda_verb_exec(addr, nid, AC_VERB_PARAMETERS, param, &res);
    return res;
}

/* Convenience: send a set verb (no response) */
static void hda_set_verb(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload)
{
    hda_verb_exec(addr, nid, verb_id, payload, NULL);
}

/* Convenience: send a get verb and return the response */
static uint32_t hda_get_verb(int addr, uint16_t nid, uint32_t verb_id, uint32_t payload)
{
    uint32_t res = 0;
    hda_verb_exec(addr, nid, verb_id, payload, &res);
    return res;
}

/* ------------------------------------------------------------------ */
/* Controller reset and initialization                                */
/* ------------------------------------------------------------------ */
static void hda_reset_controller(void)
{
    int timeout;

    hda_write32(INTCTL, 0);

    hda_write32(GCTL, hda_read32(GCTL) & ~AZX_GCTL_RESET);
    timeout = 200;
    while ((hda_read8(GCTL) & AZX_GCTL_RESET) && timeout--) usleep(500);

    usleep(1000);

    hda_write8(GCTL, hda_read8(GCTL) | AZX_GCTL_RESET);
    timeout = 200;
    while (!(hda_read8(GCTL) & AZX_GCTL_RESET) && timeout--) usleep(500);
}

static int hda_alloc_corb_rirb(void)
{
    size_t   corb_size = AZX_MAX_CORB_ENTRIES * 4;
    size_t   rirb_size = AZX_MAX_RIRB_ENTRIES * 8;
    size_t   total     = ALIGN_UP(corb_size + rirb_size, PAGE_4K_SIZE);
    uint64_t frame     = alloc_frames(total / PAGE_4K_SIZE);
    if (!frame) return -ENOMEM;

    hda_ctrl.corb_phys = frame;
    hda_ctrl.corb_buf  = phys_to_virt(frame);
    memset((void *)hda_ctrl.corb_buf, 0, corb_size);

    hda_ctrl.rirb_phys = frame + corb_size;
    hda_ctrl.rirb_buf  = phys_to_virt(frame + corb_size);
    memset((void *)hda_ctrl.rirb_buf, 0, rirb_size);

    plogk("hda: CORB at phys=0x%llx, RIRB at phys=0x%llx\n", hda_ctrl.corb_phys, hda_ctrl.rirb_phys);
    return 0;
}

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
    hda_write8(RIRBCTL, AZX_RBCTL_DMA_EN);
    hda_write32(GCTL, hda_read32(GCTL) | AZX_GCTL_UNSOL);

    hda_ctrl.rirb_rp = 0;
}

static void hda_clear_interrupts(void)
{
    hda_write8(RIRBSTS, 0x1f);
    hda_write16(STATESTS, 0x7fff);
    hda_write32(INTSTS, 0xffffffff);
}

/* ------------------------------------------------------------------ */
/* Codec probing                                                      */
/* ------------------------------------------------------------------ */
static int hda_probe_codec(int addr)
{
    uint32_t vid = hda_get_param(addr, 0, AC_PAR_VENDOR_ID);
    if (vid == 0 || vid == 0xffffffff) return -ENODEV;

    struct hda_codec *codec = &hda_ctrl.codecs[hda_ctrl.num_codecs];
    memset(codec, 0, sizeof(*codec));
    codec->addr      = addr;
    codec->vendor_id = vid & 0xffff;
    codec->device_id = vid >> 16;

    uint32_t node    = hda_get_param(addr, 0, AC_PAR_NODE_COUNT);
    codec->start_nid = (uint16_t)(node >> 16);
    codec->end_nid   = (uint16_t)(node & 0xffff);

    plogk("hda: codec #%d vendor=0x%04x device=0x%04x nodes=%u-%u\n", addr, codec->vendor_id, codec->device_id, codec->start_nid,
          codec->end_nid);

    hda_ctrl.num_codecs++;
    return 0;
}

static int hda_parse_widgets(struct hda_codec *codec)
{
    codec->afg_nid = -1;
    codec->dac_nid = -1;
    codec->pin_nid = -1;

    for (uint16_t nid = codec->start_nid; nid <= codec->end_nid; nid++) {
        uint32_t ftype = hda_get_param(codec->addr, nid, AC_PAR_FUNCTION_TYPE);
        if (ftype == AC_GRP_AUDIO_FUNCTION) {
            codec->afg_nid = nid;
            plogk("hda: AFG at nid=%u\n", nid);
        }
    }

    int count = 0;
    if (codec->afg_nid > 0) {
        uint32_t node = hda_get_param(codec->addr, (uint16_t)codec->afg_nid, AC_PAR_NODE_COUNT);
        uint16_t ws   = (uint16_t)(node >> 16);
        uint16_t we   = (uint16_t)(node & 0xffff);
        if (ws > 0 && we > ws) count = we - ws + 1;
    }
    if (count <= 0) count = codec->end_nid - codec->start_nid + 1;

    codec->num_widgets = count;
    codec->widgets     = malloc(sizeof(struct hda_widget) * count);
    if (!codec->widgets) return -ENOMEM;
    memset(codec->widgets, 0, sizeof(struct hda_widget) * count);

    uint16_t base_nid = codec->end_nid - count + 1;

    for (int i = 0; i < count; i++) {
        uint16_t           nid = base_nid + i;
        struct hda_widget *w   = &codec->widgets[i];
        w->nid                 = nid;
        w->wcap                = hda_get_param(codec->addr, nid, AC_PAR_AUDIO_WIDGET_CAP);
        w->type                = (w->wcap >> AC_WCAP_TYPE_SHIFT) & AC_WCAP_TYPE_MASK;

        if (w->wcap & AC_WCAP_CONN_LIST) {
            uint32_t clen = hda_get_param(codec->addr, nid, AC_PAR_CONNLIST_LEN);
            w->num_conns  = clen & 0xff;
            if (w->num_conns > 0) {
                w->conns = malloc(sizeof(uint16_t) * w->num_conns);
                for (int j = 0; j < w->num_conns; j += 4) {
                    uint32_t cl  = hda_get_verb(codec->addr, nid, AC_VERB_GET_CONNECT_LIST, (uint32_t)j);
                    int      rem = w->num_conns - j;
                    for (int k = 0; k < 4 && k < rem; k++) w->conns[j + k] = (cl >> (8 * k)) & 0xff;
                }
            }
        }

        if (w->type == AC_WID_PIN) {
            w->pincap   = hda_get_param(codec->addr, nid, AC_PAR_PIN_CAP);
            w->def_conf = hda_get_verb(codec->addr, nid, AC_VERB_GET_CONFIG_DEFAULT, 0);
        }

        if (w->type == AC_WID_AUD_OUT && codec->dac_nid < 0) codec->dac_nid = nid;

        if (w->type == AC_WID_PIN && codec->pin_nid < 0 && (w->pincap & AC_PINCAP_OUT)) {
            uint32_t dev = (w->def_conf >> AC_DEFCFG_DEVICE_SHIFT) & 0xf;
            if (dev != 0) codec->pin_nid = nid;
        }
    }

    if (codec->pin_nid < 0) {
        for (int i = 0; i < count; i++) {
            if (codec->widgets[i].type == AC_WID_PIN && (codec->widgets[i].pincap & AC_PINCAP_OUT)) {
                codec->pin_nid = codec->widgets[i].nid;
                break;
            }
        }
    }

    plogk("hda: %d widgets, DAC=%d PIN=%d\n", count, codec->dac_nid, codec->pin_nid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Codec configuration                                                */
/* ------------------------------------------------------------------ */
static void hda_config_codec(struct hda_codec *codec)
{
    if (codec->dac_nid < 0) {
        plogk("hda: codec #%d has no DAC, skipping.\n", codec->addr);
        return;
    }

    int addr = codec->addr;

    hda_set_verb(addr, (uint16_t)codec->afg_nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
    msleep(1);
    hda_set_verb(addr, (uint16_t)codec->dac_nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);

    if (codec->pin_nid > 0) {
        hda_set_verb(addr, (uint16_t)codec->pin_nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);

        uint32_t pincap = 0;
        for (int i = 0; i < codec->num_widgets; i++) {
            if (codec->widgets[i].nid == codec->pin_nid) {
                pincap = codec->widgets[i].pincap;
                break;
            }
        }

        hda_set_verb(addr, (uint16_t)codec->pin_nid, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_OUT_EN);
        if (pincap & AC_PINCAP_EAPD) hda_set_verb(addr, (uint16_t)codec->pin_nid, AC_VERB_SET_EAPD_BTLENABLE, AC_EAPD_BTLENABLE);

        /* Walk up the connection tree from pin to find and set the DAC path */
        int cur_nid   = codec->pin_nid;
        int max_depth = 10;
        while (max_depth--) {
            uint32_t clen  = hda_get_param(addr, (uint16_t)cur_nid, AC_PAR_CONNLIST_LEN);
            int      conns = clen & 0xff;
            if (conns <= 0) break;

            uint32_t wcap = hda_get_param(addr, (uint16_t)cur_nid, AC_PAR_AUDIO_WIDGET_CAP);
            int      type = (wcap >> AC_WCAP_TYPE_SHIFT) & AC_WCAP_TYPE_MASK;

            if (type == AC_WID_AUD_SEL || type == AC_WID_AUD_MIX) {
                for (int c = 0; c < conns; c++) {
                    uint32_t cl;
                    if (c < 4)
                        cl = hda_get_verb(addr, (uint16_t)cur_nid, AC_VERB_GET_CONNECT_LIST, 0);
                    else
                        cl = hda_get_verb(addr, (uint16_t)cur_nid, AC_VERB_GET_CONNECT_LIST, (uint32_t)c);
                    int conn = (cl >> (8 * (c % 4))) & 0xff;

                    for (int wi = 0; wi < codec->num_widgets; wi++) {
                        if (codec->widgets[wi].nid == conn && codec->widgets[wi].type == AC_WID_AUD_OUT) {
                            hda_set_verb(addr, (uint16_t)cur_nid, AC_VERB_SET_CONNECT_SEL, (uint32_t)c);
                            goto routed;
                        }
                    }
                }
                if (type == AC_WID_AUD_SEL && conns > 0) hda_set_verb(addr, (uint16_t)cur_nid, AC_VERB_SET_CONNECT_SEL, 0);
            }

            if (conns > 0 && type != AC_WID_AUD_OUT) {
                uint32_t fc   = hda_get_verb(addr, (uint16_t)cur_nid, AC_VERB_GET_CONNECT_LIST, 0);
                int      prev = fc & 0xff;
                if (prev == cur_nid || prev == 0) break;
                cur_nid = prev;
            } else {
                break;
            }
        }
routed:;

        /* Unmute DAC output amp, set gain to 0dB (0x30) */
        hda_set_verb(addr, (uint16_t)codec->dac_nid, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AC_AMP_SET_GAIN(0x30));
        hda_set_verb(addr, (uint16_t)codec->dac_nid, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_OUTPUT | AC_AMP_SET_RIGHT | AC_AMP_SET_GAIN(0x30));
    }

    plogk("hda: codec #%d configured.\n", codec->addr);
}

/* ------------------------------------------------------------------ */
/* Stream DMA                                                         */
/* ------------------------------------------------------------------ */
static int hda_setup_stream(int stream_idx, uint32_t format, size_t buf_size)
{
    if (stream_idx >= HDA_MAX_STREAMS || hda_ctrl.streams[stream_idx].allocated) return -EBUSY;

    size_t bdl_size = sizeof(struct hda_bdle) * HDA_BDL_ENTRIES;
    size_t total    = buf_size + ALIGN_UP(bdl_size, PAGE_4K_SIZE);
    total           = ALIGN_UP(total, PAGE_4K_SIZE);
    uint64_t frame  = alloc_frames(total / PAGE_4K_SIZE);
    if (!frame) return -ENOMEM;

    hda_ctrl.streams[stream_idx].allocated = 1;
    hda_ctrl.streams[stream_idx].buf_phys  = frame;
    hda_ctrl.streams[stream_idx].buf       = phys_to_virt(frame);
    hda_ctrl.streams[stream_idx].buf_size  = buf_size;
    hda_ctrl.streams[stream_idx].bdl_phys  = frame + buf_size;
    hda_ctrl.streams[stream_idx].bdl       = phys_to_virt(frame + buf_size);
    hda_ctrl.streams[stream_idx].running   = 0;

    memset((void *)hda_ctrl.streams[stream_idx].buf, 0, buf_size);
    memset((void *)hda_ctrl.streams[stream_idx].bdl, 0, bdl_size);

    struct hda_bdle *bdl = (struct hda_bdle *)hda_ctrl.streams[stream_idx].bdl;
    bdl[0].addr_low      = (uint32_t)frame;
    bdl[0].addr_high     = (uint32_t)(frame >> 32);
    bdl[0].length        = (uint32_t)buf_size;
    bdl[0].ioc           = 1;

    sd_write32(stream_idx, SD_CBL, (uint32_t)buf_size);
    sd_write16(stream_idx, SD_LVI, 0);
    sd_write16(stream_idx, SD_FORMAT, (uint16_t)format);
    sd_write32(stream_idx, SD_BDLPL, (uint32_t)hda_ctrl.streams[stream_idx].bdl_phys);
    sd_write32(stream_idx, SD_BDLPU, (uint32_t)(hda_ctrl.streams[stream_idx].bdl_phys >> 32));
    sd_write8(stream_idx, SD_CTL, 0);

    return 0;
}

static void hda_start_stream(int stream_idx)
{
    if (stream_idx >= HDA_MAX_STREAMS || !hda_ctrl.streams[stream_idx].allocated) return;
    if (hda_ctrl.streams[stream_idx].running) return;
    hda_write32(INTCTL, hda_read32(INTCTL) | (1 << stream_idx));
    sd_write8(stream_idx, SD_CTL, SD_CTL_DMA_START | SD_INT_MASK);
    hda_ctrl.streams[stream_idx].running = 1;
}

static void hda_stop_stream(int stream_idx)
{
    if (stream_idx >= HDA_MAX_STREAMS || !hda_ctrl.streams[stream_idx].allocated) return;
    sd_write8(stream_idx, SD_CTL, 0);
    sd_write8(stream_idx, SD_STS, SD_INT_MASK);
    hda_write32(INTCTL, hda_read32(INTCTL) & ~(1 << stream_idx));
    hda_ctrl.streams[stream_idx].running = 0;
}

/* ------------------------------------------------------------------ */
/* Audio subsystem interface                                          */
/* ------------------------------------------------------------------ */
static int playback_stream = -1;

static size_t hda_audio_write(audio_card_t *card, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    struct hda_controller *ctrl = card->driver_data;
    if (!ctrl || playback_stream < 0) return 0;

    size_t buf_size = ctrl->streams[playback_stream].buf_size;
    size_t to_copy  = (size > buf_size) ? buf_size : size;
    memcpy((void *)ctrl->streams[playback_stream].buf, addr, to_copy);

    return to_copy;
}

static int hda_audio_set_format(audio_card_t *card, const audio_pcm_format_t *format)
{
    uint32_t               fmt_val;
    struct hda_controller *ctrl;

    if (!card || !format) return -EINVAL;
    ctrl = card->driver_data;
    if (!ctrl) return -ENODEV;

    if (format->channels < 1 || format->channels > 2) return -EINVAL;
    if (format->bits != 8 && format->bits != 16 && format->bits != 24 && format->bits != 32) return -EINVAL;
    if (format->sample_rate < 8000 || format->sample_rate > 192000) return -EINVAL;

    card->format = *format;

    fmt_val = (format->channels - 1) & AC_FMT_CHAN_MASK;
    if (format->bits == 8)
        fmt_val |= AC_FMT_BITS_8;
    else if (format->bits == 16)
        fmt_val |= AC_FMT_BITS_16;
    else if (format->bits == 24)
        fmt_val |= AC_FMT_BITS_24;
    else
        fmt_val |= AC_FMT_BITS_32;

    /* Simplified rate encoding */
    if (format->sample_rate >= 96000)
        fmt_val |= (2 << AC_FMT_MULT_SHIFT);
    else if (format->sample_rate > 48000)
        fmt_val |= (1 << AC_FMT_MULT_SHIFT);
    else if (format->sample_rate <= 24000)
        fmt_val |= (1 << AC_FMT_DIV_SHIFT);
    else if (format->sample_rate <= 11025)
        fmt_val |= (2 << AC_FMT_DIV_SHIFT);

    if (playback_stream >= 0) hda_stop_stream(playback_stream);

    playback_stream = 0;
    int err         = hda_setup_stream(playback_stream, fmt_val, HDA_DMA_BUFFER_SIZE);
    if (err) return err;

    ctrl->audio_fmt = *format;
    plogk("hda: format %dHz %dbit %dch fmt=0x%04x\n", format->sample_rate, format->bits, format->channels, fmt_val);
    return EOK;
}

static int hda_audio_stop(audio_card_t *card)
{
    if (!card) return -EINVAL;
    if (playback_stream >= 0) hda_stop_stream(playback_stream);
    return EOK;
}

static int hda_audio_set_volume(audio_card_t *card, const audio_volume_t *volume)
{
    struct hda_controller *ctrl;
    if (!card || !volume) return -EINVAL;
    ctrl = card->driver_data;
    if (!ctrl) return -ENODEV;

    for (int c = 0; c < ctrl->num_codecs; c++) {
        struct hda_codec *codec = &ctrl->codecs[c];
        if (codec->dac_nid > 0) {
            uint32_t gain_l = ((uint32_t)volume->left * 0x7f) / 255;
            uint32_t gain_r = ((uint32_t)volume->right * 0x7f) / 255;
            hda_set_verb(codec->addr, (uint16_t)codec->dac_nid, AC_VERB_SET_AMP_GAIN_MUTE,
                         AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AC_AMP_SET_GAIN(gain_l));
            hda_set_verb(codec->addr, (uint16_t)codec->dac_nid, AC_VERB_SET_AMP_GAIN_MUTE,
                         AC_AMP_SET_OUTPUT | AC_AMP_SET_RIGHT | AC_AMP_SET_GAIN(gain_r));
        }
    }
    return EOK;
}

static int hda_audio_get_volume(audio_card_t *card, audio_volume_t *volume)
{
    if (!card || !volume) return -EINVAL;
    volume->left  = 0x80;
    volume->right = 0x80;
    return EOK;
}

static const audio_card_ops_t hda_audio_ops = {
    .pcm_read   = 0,
    .pcm_write  = hda_audio_write,
    .set_format = hda_audio_set_format,
    .stop       = hda_audio_stop,
    .set_volume = hda_audio_set_volume,
    .get_volume = hda_audio_get_volume,
};

/* ------------------------------------------------------------------ */
/* Interrupt handler                                                  */
/* ------------------------------------------------------------------ */
static void hda_interrupt_handler(interrupt_frame_t *frame)
{
    (void)frame;
    uint32_t intsts = hda_read32(INTSTS);
    if (intsts == 0 || intsts == 0xffffffff) return;

    uint8_t rirb_sts = hda_read8(RIRBSTS);
    if (rirb_sts & 0x1f) {
        hda_write8(RIRBSTS, 0x1f);
        if (rirb_sts & 0x01) hda_read16(RIRBWP);
    }
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */
static void hda_dump_capabilities(void)
{
    uint32_t gcap  = hda_read32(GCAP);
    uint8_t  minor = hda_read8(VMIN);
    uint8_t  major = hda_read8(VMAJ);
    int      oss   = (gcap >> 12) & 0x0f;
    int      iss   = (gcap >> 8) & 0x0f;
    plogk("hda: GCAP=0x%08x (rev %d.%d, %d in, %d out)\n", gcap, major, minor, iss, oss);
    hda_ctrl.num_playback = oss;
    hda_ctrl.num_capture  = iss;
}

void hda_init(void)
{
    pci_device_cache_t     *dev;
    pci_device_request_t    req;
    base_address_register_t bar;
    uint32_t                irq;

    memset(&hda_ctrl, 0, sizeof(hda_ctrl));
    hda_ctrl.lock.lock   = 0;
    hda_ctrl.lock.rflags = 0;
    hda_ctrl.use_pio     = 1;

    /* Find Intel HDA controller */
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

    if (!dev) {
        plogk("hda: No Intel HD Audio controller found.\n");
        return;
    }

    plogk("hda: Found Intel HD Audio at %04x:%02x:%02x.%x (0x%04x:0x%04x)\n", dev->device->domain, dev->device->bus, dev->device->slot,
          dev->device->func, dev->vendor_id, dev->device_id);

    bar = get_base_address_register(dev, 0);
    if (bar.type != 0) {
        plogk("hda: BAR0 not MMIO.\n");
        return;
    }

    irq = pci_get_irq(dev);
    plogk("hda: MMIO=%p size=%u IRQ=%u\n", (void *)bar.address, bar.size, irq);

    hda_ctrl.pci_dev = dev;
    hda_ctrl.mmio    = bar.address;
    hda_ctrl.irq     = irq;

    hda_reset_controller();
    hda_dump_capabilities();

    if (hda_alloc_corb_rirb()) plogk("hda: CORB/RIRB alloc failed.\n");
    return;

    hda_clear_interrupts();
    hda_init_corb_rirb();

    register_interrupt_handler(IRQ_0 + hda_ctrl.irq, hda_interrupt_handler, 0, 0x8e);
    hda_write32(INTCTL, AZX_INT_GLOBAL_EN | AZX_INT_CTRL_EN);

    uint16_t state_sts = hda_read16(STATESTS);
    plogk("hda: STATESTS=0x%04x\n", state_sts);

    int codec_mask = state_sts & 0xff;
    if (!codec_mask) {
        plogk("hda: No codec status bits, trying all 4 slots.\n");
        codec_mask = 0x0f;
    }

    for (int i = 0; i < 4 && i < HDA_MAX_CODECS; i++) {
        if (codec_mask & (1 << i)) {
            if (hda_probe_codec(i) == 0) {
                struct hda_codec *codec = &hda_ctrl.codecs[hda_ctrl.num_codecs - 1];
                hda_parse_widgets(codec);
            }
        }
    }

    if (hda_ctrl.num_codecs == 0) plogk("hda: No codecs found.\n");
    return;
    for (int c = 0; c < hda_ctrl.num_codecs; c++) hda_config_codec(&hda_ctrl.codecs[c]);

    audio_pcm_format_t fmt = {.sample_rate = 48000, .bits = 16, .channels = 2};
    hda_ctrl.audio_fmt     = fmt;
    audio_register_card("Intel HD Audio", &fmt, &hda_audio_ops, &hda_ctrl);
    hda_ctrl.audio_registered = 1;

    plogk("hda: Intel HD Audio initialized.\n");
}
