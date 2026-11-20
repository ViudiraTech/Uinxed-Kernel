/*
 *
 *      hda_codec.c
 *      Intel HD Audio codec enumeration, path routing, and mixer controls
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/intel/hda.h>
#include <drivers/audio/ulsa/ulsa.h>
#include <kernel/debug/printk.h>
#include <kernel/debug/vsprintf.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#if CONFIG_AUDIO && CONFIG_AUDIO_HDA

#    define HDA_MAX_PATH_DEPTH 16U
#    define HDA_PAR_PCM_RATES  11U

#    define HDA_RATE(base, mult, div) (AC_FMT_BASE_##base##K | (((uint32_t)(mult) - 1U) << AC_FMT_MULT_SHIFT) | (((uint32_t)(div) - 1U) << AC_FMT_DIV_SHIFT))

typedef struct hda_rate_entry {
        unsigned int hz;
        unsigned int alsa_bit;
        uint16_t     hda_fmt;
} hda_rate_entry_t;

static const hda_rate_entry_t hda_rate_bits[] = {
    {8000,   ULSA_PCM_RATE_8000,   (uint16_t)HDA_RATE(48, 1, 6)},
    {11025,  ULSA_PCM_RATE_11025,  (uint16_t)HDA_RATE(44, 1, 4)},
    {16000,  ULSA_PCM_RATE_16000,  (uint16_t)HDA_RATE(48, 1, 3)},
    {22050,  ULSA_PCM_RATE_22050,  (uint16_t)HDA_RATE(44, 1, 2)},
    {32000,  ULSA_PCM_RATE_32000,  (uint16_t)HDA_RATE(48, 2, 3)},
    {44100,  ULSA_PCM_RATE_44100,  (uint16_t)HDA_RATE(44, 1, 1)},
    {48000,  ULSA_PCM_RATE_48000,  (uint16_t)HDA_RATE(48, 1, 1)},
    {88200,  ULSA_PCM_RATE_88200,  (uint16_t)HDA_RATE(44, 2, 1)},
    {96000,  ULSA_PCM_RATE_96000,  (uint16_t)HDA_RATE(48, 2, 1)},
    {176400, ULSA_PCM_RATE_176400, (uint16_t)HDA_RATE(44, 4, 1)},
    {192000, ULSA_PCM_RATE_192000, (uint16_t)HDA_RATE(48, 4, 1)},
};

typedef struct hda_pin_candidate {
        uint16_t nid;
        uint8_t  priority;
        uint8_t  assoc_seq;
        uint32_t def_conf;
} hda_pin_candidate_t;

/* Read one 32-bit codec parameter; returns 0 if the verb times out. */
static uint32_t hda_codec_param(hda_controller_t *ctrl, uint8_t cad, uint16_t nid, uint32_t param)
{
    uint32_t val = 0;
    if (hda_verb_exec(ctrl, cad, nid, AC_VERB_PARAMETERS, param, &val) < 0) return 0;
    return val;
}

/* Issue one verb to a widget ignoring the response value. */
static void hda_codec_write(hda_controller_t *ctrl, const hda_codec_t *codec, uint16_t nid, uint32_t verb, uint32_t param)
{
    uint32_t dummy = 0;
    (void)hda_verb_exec(ctrl, codec->addr, nid, verb, param, &dummy);
}

/* Look up a discovered widget structure by NID. */
static hda_widget_t *hda_find_widget(hda_codec_t *codec, uint16_t nid)
{
    for (uint16_t i = 0; i < codec->num_widgets; i++) {
        if (codec->widgets[i].nid == nid) return &codec->widgets[i];
    }
    return NULL;
}

/* Program an amplifier gain/mute register for both channels or independent L/R values. */
static void hda_set_amp_lr(hda_controller_t *ctrl, const hda_codec_t *codec, uint16_t nid, bool dir_out, uint8_t index, uint8_t mute_l, uint8_t gain_l, uint8_t mute_r, uint8_t gain_r)
{
    uint32_t dir  = dir_out ? AC_AMP_SET_OUTPUT : AC_AMP_SET_INPUT;
    uint32_t base = dir | ((uint32_t)(index & 0x0fU) << AC_AMP_SET_INDEX_SHIFT);

    if (mute_l == mute_r && gain_l == gain_r) {
        uint32_t payload = base | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT | (mute_l ? AC_AMP_MUTE : 0U) | (gain_l & AC_AMP_GAIN);
        hda_codec_write(ctrl, codec, nid, AC_VERB_SET_AMP_GAIN_MUTE, payload);
        return;
    }

    hda_codec_write(ctrl, codec, nid, AC_VERB_SET_AMP_GAIN_MUTE, base | AC_AMP_SET_LEFT | (mute_l ? AC_AMP_MUTE : 0U) | (gain_l & AC_AMP_GAIN));
    hda_codec_write(ctrl, codec, nid, AC_VERB_SET_AMP_GAIN_MUTE, base | AC_AMP_SET_RIGHT | (mute_r ? AC_AMP_MUTE : 0U) | (gain_r & AC_AMP_GAIN));
}

/* Unmute and set 0 dB gain (clamped offset) on a widget's input or output amp. */
static void hda_unmute_amp_default(hda_controller_t *ctrl, const hda_codec_t *codec, const hda_widget_t *w, bool dir_out, uint8_t index)
{
    uint32_t cap       = dir_out ? w->amp_out_cap : w->amp_in_cap;
    uint8_t  num_steps = (uint8_t)((cap >> AC_AMPCAP_NUM_STEPS_SHIFT) & AC_AMPCAP_NUM_STEPS_MASK);
    uint8_t  offset    = (uint8_t)(cap & AC_AMPCAP_OFFSET_MASK);
    if (offset > num_steps) offset = num_steps;
    hda_set_amp_lr(ctrl, codec, w->nid, dir_out, index, 0, offset, 0, offset);
}

/* Query short or long connection list entries (including range items) for one widget. */
static void hda_parse_conn_list(hda_controller_t *ctrl, hda_codec_t *codec, hda_widget_t *w)
{
    uint32_t clen = hda_codec_param(ctrl, codec->addr, w->nid, AC_PAR_CONNLIST_LEN);
    uint32_t len  = clen & AC_CLIST_LENGTH;
    if (len == 0) return;

    bool     is_long   = (clen & AC_CLIST_LONG) != 0;
    uint32_t step      = is_long ? 2U : 4U;
    uint32_t shift_sz  = is_long ? 16U : 8U;
    uint32_t mask      = is_long ? 0x7fffU : 0x7fU;
    uint32_t range_bit = is_long ? 0x8000U : 0x80U;
    uint16_t prev      = 0;

    for (uint32_t i = 0; i < len; i += step) {
        uint32_t val = 0;
        if (hda_verb_exec(ctrl, codec->addr, w->nid, AC_VERB_GET_CONNECT_LIST, i, &val) < 0) break;

        for (uint32_t k = 0; k < step && (i + k) < len; k++) {
            uint32_t entry = (val >> (k * shift_sz)) & (is_long ? 0xffffU : 0xffU);
            uint16_t nid   = (uint16_t)(entry & mask);
            if ((entry & range_bit) && prev != 0 && prev < nid) {
                for (uint16_t r = prev + 1U; r <= nid && w->num_conns < HDA_MAX_CONNS; r++) w->conns[w->num_conns++] = r;
            } else if (nid != 0 && w->num_conns < HDA_MAX_CONNS) {
                w->conns[w->num_conns++] = nid;
            }
            prev = nid;
        }
    }
}

/* Append a unique NID to a bounded list of converter/pin IDs. */
static void hda_nid_list_add_u16(uint16_t *list, uint8_t *count, uint8_t max_count, uint16_t nid)
{
    for (uint8_t i = 0; i < *count; i++) {
        if (list[i] == nid) return;
    }
    if (*count < max_count) list[(*count)++] = nid;
}

/* Depth-first search from an output pin to find and configure a path to an audio DAC. */
static bool hda_dfs_to_dac(hda_controller_t *ctrl, hda_codec_t *codec, hda_widget_t *w, bool *visited, uint8_t depth, uint16_t *out_dac)
{
    if (!w || depth >= HDA_MAX_PATH_DEPTH) return false;
    if (w->type == AC_WID_AUD_OUT && !(w->wcap & AC_WCAP_DIGITAL)) {
        hda_unmute_amp_default(ctrl, codec, w, true, 0);
        *out_dac = w->nid;
        return true;
    }

    size_t idx_in_array = (size_t)(w - codec->widgets);
    if (idx_in_array >= codec->num_widgets || visited[idx_in_array]) return false;
    visited[idx_in_array] = true;

    for (uint8_t c = 0; c < w->num_conns; c++) {
        hda_widget_t *child = hda_find_widget(codec, w->conns[c]);
        if (hda_dfs_to_dac(ctrl, codec, child, visited, depth + 1U, out_dac)) {
            if (w->num_conns > 1 && w->type != AC_WID_AUD_MIX) hda_codec_write(ctrl, codec, w->nid, AC_VERB_SET_CONNECT_SEL, c);
            if (w->wcap & AC_WCAP_IN_AMP) hda_unmute_amp_default(ctrl, codec, w, false, c);
            if (w->wcap & AC_WCAP_OUT_AMP) hda_unmute_amp_default(ctrl, codec, w, true, 0);
            return true;
        }
    }
    return false;
}

/* Depth-first search from an audio ADC to an analog input pin, configuring mux/amps along the path. */
static bool hda_dfs_to_in_pin(hda_controller_t *ctrl, hda_codec_t *codec, hda_widget_t *w, bool *visited, uint8_t depth, uint16_t *out_pin)
{
    if (!w || depth >= HDA_MAX_PATH_DEPTH) return false;

    if (w->type == AC_WID_PIN && !(w->wcap & AC_WCAP_DIGITAL) && (w->pincap & AC_PINCAP_IN) && ((w->def_conf >> AC_DEFCFG_PORT_CONN_SHIFT) & 0x03U) != AC_JACK_PORT_NONE) {
        *out_pin = w->nid;
        return true;
    }

    size_t idx_in_array = (size_t)(w - codec->widgets);
    if (idx_in_array >= codec->num_widgets || visited[idx_in_array]) return false;
    visited[idx_in_array] = true;

    for (uint8_t c = 0; c < w->num_conns; c++) {
        hda_widget_t *child = hda_find_widget(codec, w->conns[c]);
        if (hda_dfs_to_in_pin(ctrl, codec, child, visited, depth + 1U, out_pin)) {
            if (w->num_conns > 1 && w->type != AC_WID_AUD_MIX) hda_codec_write(ctrl, codec, w->nid, AC_VERB_SET_CONNECT_SEL, c);
            if (w->wcap & AC_WCAP_IN_AMP) {
                hda_unmute_amp_default(ctrl, codec, w, false, c);
                if (w->type == AC_WID_AUD_IN && codec->cap_amp_nid == 0) {
                    codec->cap_amp_nid = w->nid;
                    codec->cap_amp_idx = c;
                    codec->cap_amp_out = false;
                    codec->cap_amp_cap = w->amp_in_cap;
                }
            }
            if (w->wcap & AC_WCAP_OUT_AMP) hda_unmute_amp_default(ctrl, codec, w, true, 0);
            return true;
        }
    }
    return false;
}

/* Sort collected output pin candidates by jack priority (Line Out -> Speaker -> HP) and association. */
static void hda_sort_pin_candidates(hda_pin_candidate_t *pins, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1U; j < count; j++) {
            bool swap = pins[j].priority < pins[i].priority || (pins[j].priority == pins[i].priority && pins[j].assoc_seq < pins[i].assoc_seq);
            if (swap) {
                hda_pin_candidate_t tmp = pins[i];
                pins[i]                 = pins[j];
                pins[j]                 = tmp;
            }
        }
    }
}

/* Configure active analog output pins and route each one from a DAC. */
static void hda_config_outputs(hda_controller_t *ctrl, hda_codec_t *codec)
{
    hda_pin_candidate_t cands[HDA_MAX_PINS];
    uint8_t             num_cands = 0;

    for (uint16_t i = 0; i < codec->num_widgets && num_cands < HDA_MAX_PINS; i++) {
        hda_widget_t *w = &codec->widgets[i];
        if (w->type != AC_WID_PIN || (w->wcap & AC_WCAP_DIGITAL)) continue;
        if (!(w->pincap & AC_PINCAP_OUT)) continue;
        if (((w->def_conf >> AC_DEFCFG_PORT_CONN_SHIFT) & 0x03U) == AC_JACK_PORT_NONE) continue;

        uint32_t dev  = (w->def_conf >> AC_DEFCFG_DEVICE_SHIFT) & 0x0fU;
        uint8_t  prio = 3;
        if (dev == AC_JACK_LINE_OUT)
            prio = 0;
        else if (dev == AC_JACK_SPEAKER)
            prio = 1;
        else if (dev == AC_JACK_HP_OUT)
            prio = 2;

        cands[num_cands++] = (hda_pin_candidate_t) {
            .nid       = w->nid,
            .priority  = prio,
            .assoc_seq = (uint8_t)(w->def_conf & 0xffU),
            .def_conf  = w->def_conf,
        };
    }

    if (num_cands == 0 || codec->num_widgets == 0) return;

    hda_sort_pin_candidates(cands, num_cands);

    bool *visited = calloc(codec->num_widgets, sizeof(bool));
    if (!visited) return;

    for (uint8_t i = 0; i < num_cands; i++) {
        hda_widget_t *pin = hda_find_widget(codec, cands[i].nid);
        if (!pin) continue;

        memset(visited, 0, codec->num_widgets * sizeof(bool));
        uint16_t dac_nid = 0;
        if (!hda_dfs_to_dac(ctrl, codec, pin, visited, 0, &dac_nid)) continue;

        uint32_t dev    = (pin->def_conf >> AC_DEFCFG_DEVICE_SHIFT) & 0x0fU;
        uint32_t pinctl = AC_PINCTL_OUT_EN;
        if (dev == AC_JACK_HP_OUT && (pin->pincap & AC_PINCAP_HP_DRV)) pinctl |= AC_PINCTL_HP_EN;

        hda_codec_write(ctrl, codec, pin->nid, AC_VERB_SET_PIN_WIDGET_CONTROL, pinctl);
        if (pin->pincap & AC_PINCAP_EAPD) hda_codec_write(ctrl, codec, pin->nid, AC_VERB_SET_EAPD_BTLENABLE, AC_EAPDBTL_EAPD);

        hda_nid_list_add_u16(codec->play_dacs, &codec->num_play_dacs, HDA_MAX_CONVS, dac_nid);
        hda_nid_list_add_u16(codec->play_pin_nids, &codec->num_play_pins, HDA_MAX_PINS, pin->nid);

        if (dev == AC_JACK_HP_OUT && codec->hp_pin_nid == 0) codec->hp_pin_nid = pin->nid;
        if (dev == AC_JACK_SPEAKER && codec->spk_pin_nid == 0) codec->spk_pin_nid = pin->nid;
    }

    free(visited);

    /* Enable unsolicited jack sense notifications when HP and speaker coexist. */
    if (codec->hp_pin_nid != 0) {
        hda_widget_t *hp = hda_find_widget(codec, codec->hp_pin_nid);
        if (hp && (hp->pincap & AC_PINCAP_PRES_DETECT) && (hp->wcap & AC_WCAP_UNSOL_CAP)) {
            hda_codec_write(ctrl, codec, codec->hp_pin_nid, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | HDA_UNSOL_HP_TAG);
            hda_codec_handle_unsol(ctrl, codec->addr, (uint32_t)HDA_UNSOL_HP_TAG << AC_UNSOL_RES_TAG_SHIFT);
        }
    }

    /* Choose the primary playback amp: prefer DAC OUT_AMP, fallback to first output pin OUT_AMP. */
    if (codec->num_play_dacs > 0) {
        hda_widget_t *dac = hda_find_widget(codec, codec->play_dacs[0]);
        if (dac && (dac->wcap & AC_WCAP_OUT_AMP)) {
            codec->play_amp_nid = dac->nid;
            codec->play_amp_out = true;
            codec->play_amp_cap = dac->amp_out_cap;
        }
    }
    if (codec->play_amp_nid == 0 && codec->num_play_pins > 0) {
        hda_widget_t *pin = hda_find_widget(codec, codec->play_pin_nids[0]);
        if (pin && (pin->wcap & AC_WCAP_OUT_AMP)) {
            codec->play_amp_nid = pin->nid;
            codec->play_amp_out = true;
            codec->play_amp_cap = pin->amp_out_cap;
        }
    }
}

/* Configure analog input ADCs and pins, routing from ADCs back to input jacks. */
static void hda_config_inputs(hda_controller_t *ctrl, hda_codec_t *codec)
{
    if (codec->num_widgets == 0) return;

    bool *visited = calloc(codec->num_widgets, sizeof(bool));
    if (!visited) return;

    for (uint16_t i = 0; i < codec->num_widgets; i++) {
        hda_widget_t *adc = &codec->widgets[i];
        if (adc->type != AC_WID_AUD_IN || (adc->wcap & AC_WCAP_DIGITAL)) continue;

        memset(visited, 0, codec->num_widgets * sizeof(bool));
        uint16_t pin_nid = 0;
        if (!hda_dfs_to_in_pin(ctrl, codec, adc, visited, 0, &pin_nid)) continue;

        hda_widget_t *pin = hda_find_widget(codec, pin_nid);
        if (pin) {
            uint32_t dev    = (pin->def_conf >> AC_DEFCFG_DEVICE_SHIFT) & 0x0fU;
            uint32_t pinctl = AC_PINCTL_IN_EN;
            if (dev == AC_JACK_MIC_IN) {
                uint32_t vref_caps = (pin->pincap >> AC_PINCAP_VREF_SHIFT) & 0x3fU;
                if (vref_caps & AC_PINCAP_VREF_80)
                    pinctl |= AC_PINCTL_VREF_80;
                else if (vref_caps & AC_PINCAP_VREF_50)
                    pinctl |= AC_PINCTL_VREF_50;
            }
            hda_codec_write(ctrl, codec, pin->nid, AC_VERB_SET_PIN_WIDGET_CONTROL, pinctl);
            hda_nid_list_add_u16(codec->cap_pin_nids, &codec->num_cap_pins, HDA_MAX_PINS, pin->nid);
        }
        hda_nid_list_add_u16(codec->cap_adcs, &codec->num_cap_adcs, HDA_MAX_CONVS, adc->nid);
    }

    free(visited);
}

/* Query Linux-compatible supported rates, formats, and subformats for a converter widget. */
static void hda_query_dir_caps(hda_controller_t *ctrl, const hda_codec_t *codec, const hda_widget_t *conv, hda_dir_caps_t *caps)
{
    uint32_t pcm = 0;
    uint32_t st  = 0;

    if (conv && (conv->wcap & AC_WCAP_FORMAT_OVRD)) {
        pcm = hda_codec_param(ctrl, codec->addr, conv->nid, AC_PAR_PCM);
        st  = hda_codec_param(ctrl, codec->addr, conv->nid, AC_PAR_STREAM);
    }
    if (pcm == 0) pcm = codec->afg_pcm;
    if (st == 0) st = codec->afg_stream;
    if (st == 0) st = AC_SUPFMT_PCM;

    unsigned int rates    = 0;
    unsigned int rate_min = 0;
    unsigned int rate_max = 0;
    for (unsigned int i = 0; i < HDA_PAR_PCM_RATES; i++) {
        if (pcm & (1U << i)) {
            rates |= hda_rate_bits[i].alsa_bit;
            if (rate_min == 0 || hda_rate_bits[i].hz < rate_min) rate_min = hda_rate_bits[i].hz;
            if (hda_rate_bits[i].hz > rate_max) rate_max = hda_rate_bits[i].hz;
        }
    }
    if (rates == 0) {
        rates    = ULSA_PCM_RATE_48000;
        rate_min = 48000;
        rate_max = 48000;
    }

    uint64_t     formats    = 0;
    uint32_t     subformats = 0;
    unsigned int bps        = 0;

    if (st & AC_SUPFMT_PCM) {
        if (pcm & AC_SUPPCM_BITS_8) {
            formats |= ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_U8);
            bps = 8;
        }
        if (pcm & AC_SUPPCM_BITS_16) {
            formats |= ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S16_LE);
            bps = 16;
        }
        if (pcm & AC_SUPPCM_BITS_20) {
            formats |= ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S32_LE);
            subformats |= (1U << SNDRV_PCM_SUBFORMAT_MSBITS_20);
            bps = 20;
        }
        if (pcm & AC_SUPPCM_BITS_24) {
            formats |= ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S32_LE);
            subformats |= (1U << SNDRV_PCM_SUBFORMAT_MSBITS_24);
            bps = 24;
        }
        if (pcm & AC_SUPPCM_BITS_32) {
            formats |= ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S32_LE);
            subformats |= (1U << SNDRV_PCM_SUBFORMAT_MSBITS_MAX);
            bps = 32;
        }
    }
    if (formats == 0) {
        formats = ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S16_LE);
        bps     = 16;
    }

    caps->formats      = formats;
    caps->subformats   = subformats;
    caps->rates        = rates;
    caps->rate_min     = rate_min;
    caps->rate_max     = rate_max;
    caps->channels_min = 1;
    caps->channels_max = 2;
    caps->maxbps       = bps;
}

/* Encode rate, channels, and PCM format into an HDA stream format descriptor word. */
int hda_calc_stream_format(unsigned int rate, unsigned int channels, snd_pcm_format_t format, unsigned int maxbps)
{
    uint16_t rate_bits = 0;
    bool     found     = false;
    for (unsigned int i = 0; i < HDA_PAR_PCM_RATES; i++) {
        if (hda_rate_bits[i].hz == rate) {
            rate_bits = hda_rate_bits[i].hda_fmt;
            found     = true;
            break;
        }
    }
    if (!found || channels == 0 || channels > 16) return -EINVAL;

    uint16_t fmt_bits = 0;
    switch (format) {
        case SNDRV_PCM_FORMAT_U8 :
            fmt_bits = AC_FMT_BITS_8;
            break;
        case SNDRV_PCM_FORMAT_S16_LE :
            fmt_bits = AC_FMT_BITS_16;
            break;
        case SNDRV_PCM_FORMAT_S24_LE :
        case SNDRV_PCM_FORMAT_S32_LE :
            if (maxbps >= 32)
                fmt_bits = AC_FMT_BITS_32;
            else if (maxbps >= 24)
                fmt_bits = AC_FMT_BITS_24;
            else if (maxbps >= 20)
                fmt_bits = AC_FMT_BITS_20;
            else
                fmt_bits = AC_FMT_BITS_16;
            break;
        default :
            return -EINVAL;
    }

    return (int)(rate_bits | fmt_bits | ((channels - 1U) & 0x0fU));
}

/* Probe codec at address cad, enumerate its AFG/widgets, and route analog paths. */
int hda_codec_probe_and_init(hda_controller_t *ctrl, uint8_t cad, hda_codec_t *codec)
{
    uint32_t vid = 0;
    if (hda_verb_exec(ctrl, cad, 0, AC_VERB_PARAMETERS, AC_PAR_VENDOR_ID, &vid) < 0) return -ENODEV;
    if (vid == 0 || vid == 0xffffffffU) return -ENODEV;

    memset(codec, 0, sizeof(*codec));
    codec->addr         = cad;
    codec->vendor_id    = vid;
    codec->subsystem_id = hda_codec_param(ctrl, cad, 0, AC_PAR_SUBSYSTEM_ID);
    codec->rev_id       = hda_codec_param(ctrl, cad, 0, AC_PAR_REV_ID);

    uint32_t root_nc = hda_codec_param(ctrl, cad, 0, AC_PAR_NODE_COUNT);
    uint16_t f_start = (uint16_t)((root_nc >> 16) & 0xffU);
    uint16_t f_count = (uint16_t)(root_nc & 0xffU);

    for (uint16_t i = 0; i < f_count; i++) {
        uint16_t fnid  = f_start + i;
        uint32_t ftype = hda_codec_param(ctrl, cad, fnid, AC_PAR_FUNCTION_TYPE) & 0xffU;
        if (ftype == AC_GRP_AUDIO_FUNCTION) {
            codec->afg_nid = fnid;
            break;
        }
    }
    if (codec->afg_nid == 0) return -ENODEV;

    hda_codec_write(ctrl, codec, codec->afg_nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
    usleep(1000);

    codec->afg_pcm         = hda_codec_param(ctrl, cad, codec->afg_nid, AC_PAR_PCM);
    codec->afg_stream      = hda_codec_param(ctrl, cad, codec->afg_nid, AC_PAR_STREAM);
    codec->afg_amp_in_cap  = hda_codec_param(ctrl, cad, codec->afg_nid, AC_PAR_AMP_IN_CAP);
    codec->afg_amp_out_cap = hda_codec_param(ctrl, cad, codec->afg_nid, AC_PAR_AMP_OUT_CAP);

    uint32_t afg_nc  = hda_codec_param(ctrl, cad, codec->afg_nid, AC_PAR_NODE_COUNT);
    uint16_t w_start = (uint16_t)((afg_nc >> 16) & 0xffU);
    uint16_t w_count = (uint16_t)(afg_nc & 0xffU);
    if (w_count == 0) return -ENODEV;

    codec->widgets = calloc(w_count, sizeof(hda_widget_t));
    if (!codec->widgets) return -ENOMEM;
    codec->num_widgets = w_count;

    for (uint16_t i = 0; i < w_count; i++) {
        hda_widget_t *w = &codec->widgets[i];
        w->nid          = w_start + i;
        w->wcap         = hda_codec_param(ctrl, cad, w->nid, AC_PAR_AUDIO_WIDGET_CAP);
        w->type         = (uint8_t)((w->wcap >> AC_WCAP_TYPE_SHIFT) & AC_WCAP_TYPE_MASK);

        if (w->wcap & AC_WCAP_POWER) hda_codec_write(ctrl, codec, w->nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
        if (w->wcap & AC_WCAP_AMP_OVRD) {
            w->amp_in_cap  = hda_codec_param(ctrl, cad, w->nid, AC_PAR_AMP_IN_CAP);
            w->amp_out_cap = hda_codec_param(ctrl, cad, w->nid, AC_PAR_AMP_OUT_CAP);
        } else {
            w->amp_in_cap  = codec->afg_amp_in_cap;
            w->amp_out_cap = codec->afg_amp_out_cap;
        }
        if (w->wcap & AC_WCAP_CONN_LIST) hda_parse_conn_list(ctrl, codec, w);
        if (w->type == AC_WID_PIN) {
            w->pincap   = hda_codec_param(ctrl, cad, w->nid, AC_PAR_PIN_CAP);
            w->def_conf = hda_codec_param(ctrl, cad, w->nid, AC_VERB_GET_CONFIG_DEFAULT);
        }
    }

    hda_config_outputs(ctrl, codec);
    hda_config_inputs(ctrl, codec);

    if (codec->num_play_dacs > 0) { hda_query_dir_caps(ctrl, codec, hda_find_widget(codec, codec->play_dacs[0]), &codec->play_caps); }
    if (codec->num_cap_adcs > 0) { hda_query_dir_caps(ctrl, codec, hda_find_widget(codec, codec->cap_adcs[0]), &codec->cap_caps); }

    plogk("hda: Codec #%u vendor=0x%08x AFG=0x%02x widgets=%u dacs=%u adcs=%u\n", cad, codec->vendor_id, codec->afg_nid, codec->num_widgets, codec->num_play_dacs, codec->num_cap_adcs);
    return (codec->num_play_dacs > 0 || codec->num_cap_adcs > 0) ? EOK : -ENODEV;
}

/* Free mixer control private state when the card tears down. */
static void hda_mixer_ctl_free(ulsa_kcontrol_t *kctl)
{
    if (kctl && kctl->private_data) {
        free(kctl->private_data);
        kctl->private_data = NULL;
    }
}

/* ULSA kcontrol info callback for an HDA volume integer slider. */
static int hda_vol_info(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_info *uinfo)
{
    const hda_mixer_ctl_t *mc = kctl->private_data;
    uinfo->type               = SNDRV_CTL_ELEM_TYPE_INTEGER;
    uinfo->count              = mc->channels;
    uinfo->value.integer.min  = 0;
    uinfo->value.integer.max  = mc->num_steps;
    return EOK;
}

/* Push current software volume and mute values for an amp widget to hardware. */
static void hda_mixer_apply_amp(const hda_mixer_ctl_t *mc)
{
    uint8_t ml = mc->has_mute && !mc->state->mute[0] ? 1U : 0U;
    uint8_t mr = mc->has_mute && !mc->state->mute[mc->channels > 1 ? 1 : 0] ? 1U : 0U;
    uint8_t gl = mc->num_steps > 0 ? (uint8_t)mc->state->vol[0] : 0U;
    uint8_t gr = mc->num_steps > 0 ? (uint8_t)mc->state->vol[mc->channels > 1 ? 1 : 0] : 0U;
    hda_set_amp_lr(mc->ctrl, mc->codec, mc->nid, mc->dir_out, mc->index, ml, gl, mr, gr);
}

/* ULSA kcontrol get callback for both volume and mute controls. */
static int hda_mixer_get(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    const hda_mixer_ctl_t *mc = kctl->private_data;
    for (unsigned int i = 0; i < mc->channels && i < 2; i++) { ucontrol->value.integer.value[i] = mc->is_switch ? mc->state->mute[i] : mc->state->vol[i]; }
    return EOK;
}

/* ULSA kcontrol put callback for HDA volume controls. */
static int hda_vol_put(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    hda_mixer_ctl_t *mc      = kctl->private_data;
    int              changed = 0;

    for (unsigned int i = 0; i < mc->channels && i < 2; i++) {
        long val = ucontrol->value.integer.value[i];
        if (val < 0 || val > mc->num_steps) return -EINVAL;
        if (mc->state->vol[i] != val) {
            mc->state->vol[i] = val;
            changed           = 1;
        }
    }
    if (changed) hda_mixer_apply_amp(mc);
    return changed;
}

/* ULSA kcontrol put callback for HDA mute switch controls. */
static int hda_switch_put(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    hda_mixer_ctl_t *mc      = kctl->private_data;
    int              changed = 0;

    for (unsigned int i = 0; i < mc->channels && i < 2; i++) {
        long val = ucontrol->value.integer.value[i] ? 1 : 0;
        if (mc->state->mute[i] != val) {
            mc->state->mute[i] = val;
            changed            = 1;
        }
    }
    if (changed) hda_mixer_apply_amp(mc);
    return changed;
}

/* Add a volume slider and/or mute switch pair for one HDA amplifier node. */
static void hda_add_amp_controls(hda_controller_t *ctrl, hda_codec_t *codec, hda_amp_state_t *state, const char *vol_name, const char *sw_name, uint16_t nid, bool dir_out, uint8_t index,
                                 uint32_t amp_cap)
{
    const hda_widget_t *w         = hda_find_widget(codec, nid);
    uint8_t             channels  = (!w || (w->wcap & AC_WCAP_STEREO)) ? 2U : 1U;
    uint8_t             num_steps = (uint8_t)((amp_cap >> AC_AMPCAP_NUM_STEPS_SHIFT) & AC_AMPCAP_NUM_STEPS_MASK);
    uint8_t             offset    = (uint8_t)(amp_cap & AC_AMPCAP_OFFSET_MASK);
    uint8_t             step_size = (uint8_t)((amp_cap >> AC_AMPCAP_STEP_SIZE_SHIFT) & AC_AMPCAP_STEP_SIZE_MASK);
    bool                has_mute  = (amp_cap & AC_AMPCAP_MUTE) != 0;

    if (offset > num_steps) offset = num_steps;
    if (num_steps == 0 && !has_mute) return;

    state->vol[0]  = offset;
    state->vol[1]  = offset;
    state->mute[0] = 1;
    state->mute[1] = 1;

    hda_mixer_ctl_t shared = {
        .ctrl      = ctrl,
        .codec     = codec,
        .state     = state,
        .nid       = nid,
        .index     = index,
        .dir_out   = dir_out,
        .channels  = channels,
        .num_steps = num_steps,
        .has_mute  = has_mute,
    };
    hda_mixer_apply_amp(&shared);

    if (num_steps > 0) {
        hda_mixer_ctl_t *vmc = calloc(1, sizeof(hda_mixer_ctl_t));
        if (vmc) {
            *vmc        = shared;
            int step_db = ((int)step_size + 1) * 25;
            int min_db  = -(int)offset * step_db;
            vmc->tlv[0] = SNDRV_CTL_TLVT_DB_SCALE;
            vmc->tlv[1] = 2U * (unsigned int)sizeof(unsigned int);
            vmc->tlv[2] = (unsigned int)min_db;
            vmc->tlv[3] = ((unsigned int)step_db & SNDRV_CTL_TLVD_DB_SCALE_MASK) | (has_mute ? SNDRV_CTL_TLVD_DB_SCALE_MUTE : 0U);

            ulsa_kcontrol_new_t tmpl = {
                .iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
                .name   = vol_name,
                .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
                .count  = 1,
                .info   = hda_vol_info,
                .get    = hda_mixer_get,
                .put    = hda_vol_put,
                .tlv    = vmc->tlv,
            };
            ulsa_kcontrol_t *kctl = ulsa_ctl_new1(&tmpl, vmc);
            if (kctl) {
                kctl->private_free = hda_mixer_ctl_free;
                (void)ulsa_ctl_add(ctrl->card, kctl);
            } else {
                free(vmc);
            }
        }
    }

    if (has_mute) {
        hda_mixer_ctl_t *smc = calloc(1, sizeof(hda_mixer_ctl_t));
        if (smc) {
            *smc                  = shared;
            smc->is_switch        = true;
            ulsa_kcontrol_new_t t = {
                .iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
                .name   = sw_name,
                .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
                .count  = 1,
                .info   = channels > 1 ? ulsa_ctl_boolean_stereo_info : ulsa_ctl_boolean_mono_info,
                .get    = hda_mixer_get,
                .put    = hda_switch_put,
            };
            ulsa_kcontrol_t *kctl = ulsa_ctl_new1(&t, smc);
            if (kctl) {
                kctl->private_free = hda_mixer_ctl_free;
                (void)ulsa_ctl_add(ctrl->card, kctl);
            } else {
                free(smc);
            }
        }
    }
}

/* Register ULSA mixer kcontrols for the codec's playback/capture amps. */
void hda_codec_build_controls(hda_controller_t *ctrl, hda_codec_t *codec)
{
    if (codec->play_amp_nid != 0) {
        hda_add_amp_controls(ctrl, codec, &codec->play_amp_state, "Master Playback Volume", "Master Playback Switch", codec->play_amp_nid, codec->play_amp_out, 0, codec->play_amp_cap);
    }
    if (codec->cap_amp_nid != 0) {
        hda_add_amp_controls(ctrl, codec, &codec->cap_amp_state, "Capture Volume", "Capture Switch", codec->cap_amp_nid, codec->cap_amp_out, codec->cap_amp_idx, codec->cap_amp_cap);
    }
}

/* Program converter stream tag and format registers for all active DACs or ADCs. */
void hda_codec_setup_stream(hda_controller_t *ctrl, hda_codec_t *codec, int dir, uint8_t stream_tag, uint16_t fmt)
{
    const uint16_t *nids  = dir == SNDRV_PCM_STREAM_PLAYBACK ? codec->play_dacs : codec->cap_adcs;
    uint8_t         count = dir == SNDRV_PCM_STREAM_PLAYBACK ? codec->num_play_dacs : codec->num_cap_adcs;

    for (uint8_t i = 0; i < count; i++) {
        uint16_t nid = nids[i];
        hda_codec_write(ctrl, codec, nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
        hda_codec_write(ctrl, codec, nid, AC_VERB_SET_CHANNEL_STREAMID, ((uint32_t)stream_tag << 4) | 0U);
        hda_codec_write(ctrl, codec, nid, AC_VERB_SET_STREAM_FORMAT, fmt);
    }
}

/* Disassociate all active DACs or ADCs in dir from any DMA stream tag. */
void hda_codec_cleanup_stream(hda_controller_t *ctrl, hda_codec_t *codec, int dir)
{
    const uint16_t *nids  = dir == SNDRV_PCM_STREAM_PLAYBACK ? codec->play_dacs : codec->cap_adcs;
    uint8_t         count = dir == SNDRV_PCM_STREAM_PLAYBACK ? codec->num_play_dacs : codec->num_cap_adcs;

    for (uint8_t i = 0; i < count; i++) { hda_codec_write(ctrl, codec, nids[i], AC_VERB_SET_CHANNEL_STREAMID, 0U); }
}

/* Handle one unsolicited RIRB response queued by the controller ISR. */
void hda_codec_handle_unsol(hda_controller_t *ctrl, uint8_t cad, uint32_t res)
{
    hda_codec_t *codec = (cad < HDA_MAX_CODECS) ? ctrl->codecs[cad] : NULL;
    if (!codec || codec->hp_pin_nid == 0 || codec->spk_pin_nid == 0) return;

    uint32_t tag = (res >> AC_UNSOL_RES_TAG_SHIFT) & AC_UNSOL_RES_TAG_MASK;
    if (tag != HDA_UNSOL_HP_TAG) return;

    uint32_t sense = 0;
    if (hda_verb_exec(ctrl, codec->addr, codec->hp_pin_nid, AC_VERB_GET_PIN_SENSE, 0, &sense) < 0) return;

    bool hp_present = (sense & AC_PINSENSE_PRESENCE) != 0;
    hda_codec_write(ctrl, codec, codec->spk_pin_nid, AC_VERB_SET_PIN_WIDGET_CONTROL, hp_present ? 0U : AC_PINCTL_OUT_EN);
}

#endif // CONFIG_AUDIO && CONFIG_AUDIO_HDA
