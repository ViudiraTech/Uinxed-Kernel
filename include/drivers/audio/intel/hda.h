/*
 *
 *      hda.h
 *      Intel High Definition Audio (HDA) driver header file
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_AUDIO_INTEL_HDA_H_
#define INCLUDE_DRIVERS_AUDIO_INTEL_HDA_H_

#include <drivers/audio/ulsa/ulsa.h>
#include <drivers/bus/pci.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <sync/mutex.h>
#include <sync/spin_lock.h>

/* PCI identification */
#define PCI_CLASS_HDA    0x040300U
#define PCI_VENDOR_INTEL 0x8086U
#define HDA_PCI_TCSEL    0x44U

/* Controller MMIO global registers */
#define HDA_GCAP      0x00U
#define HDA_VMIN      0x02U
#define HDA_VMAJ      0x03U
#define HDA_GCTL      0x08U
#define HDA_WAKEEN    0x0cU
#define HDA_STATESTS  0x0eU
#define HDA_GSTS      0x10U
#define HDA_INTCTL    0x20U
#define HDA_INTSTS    0x24U
#define HDA_WALLCLK   0x30U
#define HDA_SSYNC     0x38U
#define HDA_CORBLBASE 0x40U
#define HDA_CORBUBASE 0x44U
#define HDA_CORBWP    0x48U
#define HDA_CORBRP    0x4aU
#define HDA_CORBCTL   0x4cU
#define HDA_CORBSTS   0x4dU
#define HDA_CORBSIZE  0x4eU
#define HDA_RIRBLBASE 0x50U
#define HDA_RIRBUBASE 0x54U
#define HDA_RIRBWP    0x58U
#define HDA_RINTCNT   0x5aU
#define HDA_RIRBCTL   0x5cU
#define HDA_RIRBSTS   0x5dU
#define HDA_RIRBSIZE  0x5eU
#define HDA_IC        0x60U
#define HDA_IR        0x64U
#define HDA_IRS       0x68U
#define HDA_DPLBASE   0x70U
#define HDA_DPUBASE   0x74U

/* Stream descriptor registers */
#define HDA_SD_BASE(idx) (0x80U + (uint32_t)(idx) * 0x20U)
#define HDA_SD_CTL       0x00U
#define HDA_SD_STS       0x03U
#define HDA_SD_LPIB      0x04U
#define HDA_SD_CBL       0x08U
#define HDA_SD_LVI       0x0cU
#define HDA_SD_FIFOW     0x0eU
#define HDA_SD_FIFOSIZE  0x10U
#define HDA_SD_FORMAT    0x12U
#define HDA_SD_BDLPL     0x18U
#define HDA_SD_BDLPU     0x1cU

/* GCAP bits */
#define AZX_GCAP_64OK (1U << 0)

/* GCTL bits */
#define AZX_GCTL_RESET  (1U << 0)
#define AZX_GCTL_FCNTRL (1U << 1)
#define AZX_GCTL_UNSOL  (1U << 8)

/* INTCTL / INTSTS bits */
#define AZX_INT_GLOBAL_EN  0x80000000U
#define AZX_INT_CTRL_EN    0x40000000U
#define AZX_INT_ALL_STREAM 0x3fffffffU

/* CORB / RIRB control and status bits */
#define AZX_CORBRP_RST       (1U << 15)
#define AZX_CORBCTL_CMEIE    (1U << 0)
#define AZX_CORBCTL_RUN      (1U << 1)
#define AZX_CORBSTS_CMEI     (1U << 0)
#define AZX_RIRBWP_RST       (1U << 15)
#define AZX_RBCTL_IRQ_EN     (1U << 0)
#define AZX_RBCTL_DMA_EN     (1U << 1)
#define AZX_RBCTL_OVERRUN_EN (1U << 2)
#define AZX_RBSTS_IRQ        (1U << 0)
#define AZX_RBSTS_OVERRUN    (1U << 2)
#define AZX_RBSTS_INT_MASK   (AZX_RBSTS_IRQ | AZX_RBSTS_OVERRUN)

/* Immediate command interface bits */
#define AZX_IRS_BUSY  (1U << 0)
#define AZX_IRS_VALID (1U << 1)

/* Position buffer register bit */
#define AZX_DPLBASE_ENABLE 0x01U

/* Stream descriptor control & status bits */
#define HDA_SD_CTL_SRST      (1U << 0)
#define HDA_SD_CTL_DMA_START (1U << 1)
#define HDA_SD_CTL_IOCE      (1U << 2)
#define HDA_SD_CTL_FEIE      (1U << 3)
#define HDA_SD_CTL_DEIE      (1U << 4)
#define HDA_SD_CTL_INT_MASK  0x1cU
#define HDA_SD_CTL_TAG_SHIFT 20

#define HDA_SD_STS_BCIS  (1U << 2)
#define HDA_SD_STS_FIFOE (1U << 3)
#define HDA_SD_STS_DESE  (1U << 4)
#define HDA_SD_INT_MASK  0x1cU

/* Codec verbs */
#define AC_VERB_SET_STREAM_FORMAT        0x200U
#define AC_VERB_SET_AMP_GAIN_MUTE        0x300U
#define AC_VERB_SET_CONNECT_SEL          0x701U
#define AC_VERB_SET_POWER_STATE          0x705U
#define AC_VERB_SET_CHANNEL_STREAMID     0x706U
#define AC_VERB_SET_PIN_WIDGET_CONTROL   0x707U
#define AC_VERB_SET_UNSOLICITED_ENABLE   0x708U
#define AC_VERB_SET_EAPD_BTLENABLE       0x70cU
#define AC_VERB_SET_CODEC_RESET          0x7ffU
#define AC_VERB_GET_STREAM_FORMAT        0xa00U
#define AC_VERB_GET_AMP_GAIN_MUTE        0xb00U
#define AC_VERB_PARAMETERS               0xf00U
#define AC_VERB_GET_CONNECT_SEL          0xf01U
#define AC_VERB_GET_CONNECT_LIST         0xf02U
#define AC_VERB_GET_POWER_STATE          0xf05U
#define AC_VERB_GET_CONV                 0xf06U
#define AC_VERB_GET_PIN_WIDGET_CONTROL   0xf07U
#define AC_VERB_GET_UNSOLICITED_RESPONSE 0xf08U
#define AC_VERB_GET_PIN_SENSE            0xf09U
#define AC_VERB_GET_EAPD_BTLENABLE       0xf0cU
#define AC_VERB_GET_CONFIG_DEFAULT       0xf1cU

/* Codec parameters */
#define AC_PAR_VENDOR_ID        0x00U
#define AC_PAR_SUBSYSTEM_ID     0x01U
#define AC_PAR_REV_ID           0x02U
#define AC_PAR_NODE_COUNT       0x04U
#define AC_PAR_FUNCTION_TYPE    0x05U
#define AC_PAR_AUDIO_WIDGET_CAP 0x09U
#define AC_PAR_PCM              0x0aU
#define AC_PAR_STREAM           0x0bU
#define AC_PAR_PIN_CAP          0x0cU
#define AC_PAR_AMP_IN_CAP       0x0dU
#define AC_PAR_CONNLIST_LEN     0x0eU
#define AC_PAR_AMP_OUT_CAP      0x12U

/* Function group types */
#define AC_GRP_AUDIO_FUNCTION 0x01U

/* Widget types */
#define AC_WID_AUD_OUT 0x0U
#define AC_WID_AUD_IN  0x1U
#define AC_WID_AUD_MIX 0x2U
#define AC_WID_AUD_SEL 0x3U
#define AC_WID_PIN     0x4U
#define AC_WID_POWER   0x5U
#define AC_WID_VOL_KNB 0x6U
#define AC_WID_BEEP    0x7U

/* Audio widget capability bits */
#define AC_WCAP_STEREO      (1U << 0)
#define AC_WCAP_IN_AMP      (1U << 1)
#define AC_WCAP_OUT_AMP     (1U << 2)
#define AC_WCAP_AMP_OVRD    (1U << 3)
#define AC_WCAP_FORMAT_OVRD (1U << 4)
#define AC_WCAP_UNSOL_CAP   (1U << 7)
#define AC_WCAP_CONN_LIST   (1U << 8)
#define AC_WCAP_DIGITAL     (1U << 9)
#define AC_WCAP_POWER       (1U << 10)
#define AC_WCAP_TYPE_SHIFT  20
#define AC_WCAP_TYPE_MASK   0x0fU

/* Connection list flags */
#define AC_CLIST_LENGTH 0x7fU
#define AC_CLIST_LONG   (1U << 7)

/* Pin capabilities */
#define AC_PINCAP_PRES_DETECT (1U << 2)
#define AC_PINCAP_HP_DRV      (1U << 3)
#define AC_PINCAP_OUT         (1U << 4)
#define AC_PINCAP_IN          (1U << 5)
#define AC_PINCAP_VREF_SHIFT  8
#define AC_PINCAP_VREF_50     (1U << 1)
#define AC_PINCAP_VREF_80     (1U << 4)
#define AC_PINCAP_EAPD        (1U << 16)

/* Pin widget control bits */
#define AC_PINCTL_VREF_HIZ 0U
#define AC_PINCTL_VREF_50  1U
#define AC_PINCTL_VREF_80  4U
#define AC_PINCTL_IN_EN    (1U << 5)
#define AC_PINCTL_OUT_EN   (1U << 6)
#define AC_PINCTL_HP_EN    (1U << 7)

/* Pin sense / EAPD / Unsol bits */
#define AC_PINSENSE_PRESENCE   (1U << 31)
#define AC_EAPDBTL_EAPD        (1U << 1)
#define AC_USRSP_EN            (1U << 7)
#define AC_UNSOL_RES_TAG_SHIFT 26
#define AC_UNSOL_RES_TAG_MASK  0x3fU
#define AC_RIRB_EX_UNSOL       (1U << 4)

/* Amplifier capabilities */
#define AC_AMPCAP_OFFSET_MASK     0x7fU
#define AC_AMPCAP_NUM_STEPS_MASK  0x7fU
#define AC_AMPCAP_NUM_STEPS_SHIFT 8
#define AC_AMPCAP_STEP_SIZE_MASK  0x7fU
#define AC_AMPCAP_STEP_SIZE_SHIFT 16
#define AC_AMPCAP_MUTE            (1U << 31)

/* Amplifier SET/GET payload bits */
#define AC_AMP_MUTE            (1U << 7)
#define AC_AMP_GAIN            0x7fU
#define AC_AMP_SET_INDEX_SHIFT 8
#define AC_AMP_SET_RIGHT       (1U << 12)
#define AC_AMP_SET_LEFT        (1U << 13)
#define AC_AMP_SET_INPUT       (1U << 14)
#define AC_AMP_SET_OUTPUT      (1U << 15)
#define AC_AMP_GET_LEFT        (1U << 13)
#define AC_AMP_GET_RIGHT       0U
#define AC_AMP_GET_OUTPUT      (1U << 15)
#define AC_AMP_GET_INPUT       0U

/* PCM format / rate capabilities */
#define AC_SUPFMT_PCM     (1U << 0)
#define AC_SUPPCM_BITS_8  (1U << 16)
#define AC_SUPPCM_BITS_16 (1U << 17)
#define AC_SUPPCM_BITS_20 (1U << 18)
#define AC_SUPPCM_BITS_24 (1U << 19)
#define AC_SUPPCM_BITS_32 (1U << 20)

/* Stream format word fields */
#define AC_FMT_CHAN_SHIFT 0
#define AC_FMT_BITS_SHIFT 4
#define AC_FMT_BITS_8     (0U << 4)
#define AC_FMT_BITS_16    (1U << 4)
#define AC_FMT_BITS_20    (2U << 4)
#define AC_FMT_BITS_24    (3U << 4)
#define AC_FMT_BITS_32    (4U << 4)
#define AC_FMT_DIV_SHIFT  8
#define AC_FMT_MULT_SHIFT 11
#define AC_FMT_BASE_48K   (0U << 14)
#define AC_FMT_BASE_44K   (1U << 14)

/* Power state values */
#define AC_PWRST_D0 0x00U
#define AC_PWRST_D3 0x03U

/* Default configuration pin fields and jack types */
#define AC_DEFCFG_PORT_CONN_SHIFT 30
#define AC_DEFCFG_DEVICE_SHIFT    20
#define AC_JACK_PORT_NONE         1U
#define AC_JACK_LINE_OUT          0U
#define AC_JACK_SPEAKER           1U
#define AC_JACK_HP_OUT            2U
#define AC_JACK_LINE_IN           8U
#define AC_JACK_AUX               9U
#define AC_JACK_MIC_IN            10U

/* Driver sizing limits */
#define HDA_MAX_CONTROLLERS 4
#define HDA_MAX_CODECS      15
#define HDA_MAX_STREAMS     30
#define HDA_MAX_CONNS       32
#define HDA_MAX_PINS        16
#define HDA_MAX_CONVS       8
#define HDA_CORB_ENTRIES    256
#define HDA_RIRB_ENTRIES    256
#define HDA_BDL_ENTRIES     256
#define HDA_UNSOL_RING_SIZE 32
#define HDA_STREAM_NONE     0xffU

#define HDA_DMA_BUFFER_MAX   (256UL * 1024UL)
#define HDA_PERIOD_BYTES_MIN 128U
#define HDA_PERIOD_BYTES_MAX (128UL * 1024UL)
#define HDA_PERIODS_MIN      2U
#define HDA_PERIODS_MAX      32U
#define HDA_UNSOL_HP_TAG     1U

#if CONFIG_AUDIO && CONFIG_AUDIO_HDA

typedef struct hda_controller hda_controller_t;
typedef struct hda_codec      hda_codec_t;

/* Buffer descriptor list entry (16-byte DMA scatter-gather descriptor) */
typedef struct hda_bdle {
        uint32_t addr_low;
        uint32_t addr_high;
        uint32_t length;
        uint32_t ioc;
} __attribute__((packed)) hda_bdle_t;

/* RIRB entry (64-bit hardware response record) */
typedef struct hda_rirb_entry {
        uint32_t resp;
        uint32_t resp_ex;
} __attribute__((packed)) hda_rirb_entry_t;

/* Queued unsolicited codec response */
typedef struct hda_unsol_event {
        uint32_t resp;
        uint32_t resp_ex;
} hda_unsol_event_t;

/* Enumerated codec widget descriptor */
typedef struct hda_widget {
        uint16_t nid;
        uint8_t  type;
        uint8_t  num_conns;
        uint32_t wcap;
        uint32_t pincap;
        uint32_t def_conf;
        uint32_t amp_in_cap;
        uint32_t amp_out_cap;
        uint16_t conns[HDA_MAX_CONNS];
} hda_widget_t;

/* Probed audio capabilities for one stream direction */
typedef struct hda_dir_caps {
        uint64_t     formats;
        uint32_t     subformats;
        unsigned int rates;
        unsigned int rate_min;
        unsigned int rate_max;
        unsigned int channels_min;
        unsigned int channels_max;
        unsigned int maxbps;
} hda_dir_caps_t;

/* Shared mute/volume shadow state for an amplifier node */
typedef struct hda_amp_state {
        long vol[2];
        long mute[2];
} hda_amp_state_t;

/* Per-control state shared with ULSA kcontrol callbacks */
typedef struct hda_mixer_ctl {
        hda_controller_t *ctrl;
        hda_codec_t      *codec;
        hda_amp_state_t  *state;
        uint16_t          nid;
        uint8_t           index;
        bool              dir_out;
        bool              is_switch;
        uint8_t           channels;
        uint8_t           num_steps;
        bool              has_mute;
        unsigned int      tlv[4];
} hda_mixer_ctl_t;

/* Discovered HDA codec topology and capabilities */
struct hda_codec {
        uint8_t       addr;
        uint32_t      vendor_id;
        uint32_t      subsystem_id;
        uint32_t      rev_id;
        uint16_t      afg_nid;
        uint32_t      afg_pcm;
        uint32_t      afg_stream;
        uint32_t      afg_amp_in_cap;
        uint32_t      afg_amp_out_cap;
        uint16_t      num_widgets;
        hda_widget_t *widgets;

        uint16_t play_dacs[HDA_MAX_CONVS];
        uint8_t  num_play_dacs;
        uint16_t play_pin_nids[HDA_MAX_PINS];
        uint8_t  num_play_pins;

        uint16_t cap_adcs[HDA_MAX_CONVS];
        uint8_t  num_cap_adcs;
        uint16_t cap_pin_nids[HDA_MAX_PINS];
        uint8_t  num_cap_pins;

        uint16_t        hp_pin_nid;
        uint16_t        spk_pin_nid;
        uint16_t        play_amp_nid;
        bool            play_amp_out;
        uint32_t        play_amp_cap;
        hda_amp_state_t play_amp_state;
        uint16_t        cap_amp_nid;
        uint8_t         cap_amp_idx;
        bool            cap_amp_out;
        uint32_t        cap_amp_cap;
        hda_amp_state_t cap_amp_state;

        hda_dir_caps_t play_caps;
        hda_dir_caps_t cap_caps;
};

/* Active DMA stream descriptor (playback or capture) */
typedef struct hda_stream {
        uint8_t               index;
        uint8_t               tag;
        uint32_t              sd_base;
        uint64_t              bdl_phys;
        volatile hda_bdle_t  *bdl;
        ulsa_pcm_substream_t *substream;
        size_t                buf_bytes;
        size_t                period_bytes;
        unsigned int          periods;
        uint16_t              format_val;
        bool                  opened;
        bool                  prepared;
        bool                  running;
} hda_stream_t;

/* Per-controller HDA hardware state */
struct hda_controller {
        pci_device_cache_t *pci;
        pci_bar_t           bar;
        pci_irq_state_t     irq_state;
        uint8_t             irq_slot;
        volatile uint32_t   irq_active;
        bool                stopping;
        bool                ok64;
        bool                use_pio;

        uint8_t maj_rev;
        uint8_t min_rev;
        uint8_t iss;
        uint8_t oss;
        uint8_t bss;
        uint8_t num_streams;

        spinlock_t reg_lock;
        mutex_t    cmd_mutex;

        uint64_t                   rb_page_phys;
        volatile uint32_t         *corb;
        uint64_t                   corb_phys;
        uint16_t                   corb_entries;
        uint16_t                   corb_wp;
        volatile hda_rirb_entry_t *rirb;
        uint64_t                   rirb_phys;
        uint16_t                   rirb_entries;
        uint16_t                   rirb_rp;
        volatile uint32_t         *posbuf;
        uint64_t                   posbuf_phys;

        uint32_t          rirb_resp[HDA_MAX_CODECS];
        bool              rirb_got[HDA_MAX_CODECS];
        hda_unsol_event_t unsol_ring[HDA_UNSOL_RING_SIZE];
        uint16_t          unsol_head;
        uint16_t          unsol_tail;

        hda_codec_t *codecs[HDA_MAX_CODECS];
        hda_codec_t *primary_codec;
        uint8_t      num_codecs;

        hda_stream_t streams[2];
        ulsa_card_t *card;
};

/* Execute one HDA codec verb and return its 32-bit response. */
int hda_verb_exec(hda_controller_t *ctrl, uint8_t cad, uint16_t nid, uint32_t verb, uint32_t param, uint32_t *resp);

/* Drain and process any pending unsolicited jack-detect responses. */
void hda_process_unsol(hda_controller_t *ctrl);

/* Encode rate/channels/format into an HDA stream format descriptor word, or negative errno. */
int hda_calc_stream_format(unsigned int rate, unsigned int channels, snd_pcm_format_t format, unsigned int maxbps);

/* Probe codec at address cad, enumerate its AFG/widgets, and route analog paths. */
int hda_codec_probe_and_init(hda_controller_t *ctrl, uint8_t cad, hda_codec_t *codec);

/* Register ULSA mixer kcontrols for the codec's playback/capture amps. */
void hda_codec_build_controls(hda_controller_t *ctrl, hda_codec_t *codec);

/* Program converter stream tag and format registers for all active DACs or ADCs. */
void hda_codec_setup_stream(hda_controller_t *ctrl, hda_codec_t *codec, int dir, uint8_t stream_tag, uint16_t fmt);

/* Disassociate all active DACs or ADCs in dir from any DMA stream tag. */
void hda_codec_cleanup_stream(hda_controller_t *ctrl, hda_codec_t *codec, int dir);

/* Handle one unsolicited RIRB response queued by the controller ISR. */
void hda_codec_handle_unsol(hda_controller_t *ctrl, uint8_t cad, uint32_t res);

/* Probe and initialize all Intel HD Audio PCI controllers. */
void hda_init(void);

#else

/* Probe and initialize all Intel HD Audio PCI controllers (no-op when disabled). */
static inline void hda_init(void) {}

#endif // CONFIG_AUDIO && CONFIG_AUDIO_HDA

#endif // INCLUDE_DRIVERS_AUDIO_INTEL_HDA_H_
