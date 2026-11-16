/*
 *
 *      ac97_bus.c
 *      Generic AC'97 codec bus, rate probing, and ULSA mixer construction
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ac97/ac97.h>
#include <kernel/debug/printk.h>
#include <kernel/debug/vsprintf.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#if CONFIG_AUDIO && CONFIG_AUDIO_AC97

#    define AC97_PACK_VOLSW(reg, lshift, rshift, mask, invert)                                                                                                    \
        ((unsigned long)(reg) | (((unsigned long)(lshift) & 0x0fUL) << 8) | (((unsigned long)(rshift) & 0x0fUL) << 12) | (((unsigned long)(mask) & 0xffUL) << 16) \
         | (((unsigned long)(invert) & 1UL) << 24))

#    define AC97_VOLSW_REG(v)    ((uint16_t)((v) & 0xffUL))
#    define AC97_VOLSW_SHIFT(v)  ((int)(((v) >> 8) & 0x0fUL))
#    define AC97_VOLSW_RSHIFT(v) ((int)(((v) >> 12) & 0x0fUL))
#    define AC97_VOLSW_MASK(v)   ((unsigned int)(((v) >> 16) & 0xffUL))
#    define AC97_VOLSW_INVERT(v) ((int)(((v) >> 24) & 1UL))

typedef struct ac97_codec_id_entry {
        uint32_t    id;
        uint32_t    mask;
        const char *name;
} ac97_codec_id_entry_t;

static const ac97_codec_id_entry_t ac97_codec_ids[] = {
    {0x83847600U, 0xffffffffU, "SigmaTel STAC9700,83,84"},
    {0x83847604U, 0xffffffffU, "SigmaTel STAC9701,3,4,5"},
    {0x83847608U, 0xffffffffU, "SigmaTel STAC9708,11"   },
    {0x83847609U, 0xffffffffU, "SigmaTel STAC9721,23"   },
    {0x83847644U, 0xffffffffU, "SigmaTel STAC9744"      },
    {0x83847656U, 0xffffffffU, "SigmaTel STAC9756,57"   },
    {0x41445348U, 0xffffffffU, "Analog Devices AD1881A" },
    {0x41445360U, 0xffffffffU, "Analog Devices AD1885"  },
    {0x41445370U, 0xffffffffU, "Analog Devices AD1980"  },
    {0x414c4710U, 0xffffffffU, "Realtek ALC200/200P"    },
    {0x414c4720U, 0xffffffffU, "Realtek ALC650"         },
    {0x414c4760U, 0xffffffffU, "Realtek ALC655"         },
    {0x43525900U, 0xfffffff0U, "Cirrus Logic CS4297"    },
    {0x43525910U, 0xfffffff0U, "Cirrus Logic CS4297A"   },
    {0x43525930U, 0xfffffff0U, "Cirrus Logic CS4299"    },
    {0x594d4800U, 0xfffffff0U, "Yamaha YMF743"          },
};

typedef struct ac97_rate_probe_entry {
        unsigned int hz;
        unsigned int rate_bit;
} ac97_rate_probe_entry_t;

static const ac97_rate_probe_entry_t ac97_standard_rates[] = {
    {8000,  ULSA_PCM_RATE_8000 },
    {11025, ULSA_PCM_RATE_11025},
    {16000, ULSA_PCM_RATE_16000},
    {22050, ULSA_PCM_RATE_22050},
    {32000, ULSA_PCM_RATE_32000},
    {44100, ULSA_PCM_RATE_44100},
    {48000, ULSA_PCM_RATE_48000},
};

typedef struct ac97_mix_desc {
        const char *prefix;
        uint16_t    reg;
        bool        default_unmuted;
        bool        is_gain_zero_db;
} ac97_mix_desc_t;

static const ac97_mix_desc_t ac97_playback_mixers[] = {
    {"Master Playback",      AC97_MASTER,      true,  false},
    {"Headphone Playback",   AC97_HEADPHONE,   false, false},
    {"Master Mono Playback", AC97_MASTER_MONO, false, false},
    {"Phone Playback",       AC97_PHONE,       false, true },
    {"Mic Playback",         AC97_MIC,         false, true },
    {"Line Playback",        AC97_LINE,        false, true },
    {"CD Playback",          AC97_CD,          false, true },
    {"Video Playback",       AC97_VIDEO,       false, true },
    {"Aux Playback",         AC97_AUX,         false, true },
    {"PCM Playback",         AC97_PCM,         true,  true },
};

static const char *const ac97_capture_sources[8] = {
    "Mic", "CD", "Video", "Aux", "Line", "Mix", "Mix Mono", "Phone",
};

ULSA_TLV_DB_SCALE(ac97_tlv_db_4bit, -4500, 300, 0);
ULSA_TLV_DB_SCALE(ac97_tlv_db_5bit, -4650, 150, 0);
ULSA_TLV_DB_SCALE(ac97_tlv_db_5bit_12dbmax, -3450, 150, 0);
ULSA_TLV_DB_SCALE(ac97_tlv_db_6bit, -9450, 150, 0);
ULSA_TLV_DB_SCALE(ac97_tlv_db_rec_gain, 0, 150, 0);

/* Read an AC'97 codec register directly via bus ops. */
uint16_t ac97_read(ac97_codec_t *codec, uint16_t reg)
{
    if (!codec || !codec->bus || !codec->bus->ops || !codec->bus->ops->read) return 0xffff;
    if (reg >= 0x80 || (reg & 1U)) return 0;
    return codec->bus->ops->read(codec, reg);
}

/* Write an AC'97 codec register directly without modifying cache. */
void ac97_write(ac97_codec_t *codec, uint16_t reg, uint16_t val)
{
    if (!codec || !codec->bus || !codec->bus->ops || !codec->bus->ops->write) return;
    if (reg >= 0x80 || (reg & 1U)) return;
    codec->bus->ops->write(codec, reg, val);
}

/* Read an AC'97 codec register from cache, populating on miss. */
static uint16_t ac97_read_cache(ac97_codec_t *codec, uint16_t reg)
{
    if (!codec || reg >= 0x80 || (reg & 1U)) return 0;
    mutex_lock(&codec->reg_mutex);
    if (!codec->reg_valid[reg]) {
        codec->regs[reg]      = ac97_read(codec, reg);
        codec->reg_valid[reg] = true;
    }
    uint16_t val = codec->regs[reg];
    mutex_unlock(&codec->reg_mutex);
    return val;
}

/* Write an AC'97 codec register and record it in the cache. */
void ac97_write_cache(ac97_codec_t *codec, uint16_t reg, uint16_t val)
{
    if (!codec || reg >= 0x80 || (reg & 1U)) return;
    mutex_lock(&codec->reg_mutex);
    codec->regs[reg]      = val;
    codec->reg_valid[reg] = true;
    ac97_write(codec, reg, val);
    mutex_unlock(&codec->reg_mutex);
}

/* Modify masked bits of a cached AC'97 register; return 1 if changed. */
int ac97_update_bits(ac97_codec_t *codec, uint16_t reg, uint16_t mask, uint16_t val)
{
    if (!codec || reg >= 0x80 || (reg & 1U)) return -EINVAL;
    mutex_lock(&codec->reg_mutex);
    if (!codec->reg_valid[reg]) {
        codec->regs[reg]      = ac97_read(codec, reg);
        codec->reg_valid[reg] = true;
    }
    uint16_t old_val = codec->regs[reg];
    uint16_t new_val = (uint16_t)((old_val & (uint16_t)~mask) | (val & mask));
    int      changed = new_val != old_val ? 1 : 0;
    if (changed) {
        codec->regs[reg] = new_val;
        ac97_write(codec, reg, new_val);
    }
    mutex_unlock(&codec->reg_mutex);
    return changed;
}

/* Format a codec ID into a printable human name. */
void ac97_codec_short_name(uint32_t id, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    for (size_t i = 0; i < sizeof(ac97_codec_ids) / sizeof(ac97_codec_ids[0]); i++) {
        if ((id & ac97_codec_ids[i].mask) == ac97_codec_ids[i].id) {
            snprintf(buf, len, "%s", ac97_codec_ids[i].name);
            return;
        }
    }
    char c1 = (char)((id >> 24) & 0xffU);
    char c2 = (char)((id >> 16) & 0xffU);
    char c3 = (char)((id >> 8) & 0xffU);
    if (c1 < ' ' || c1 > '~') c1 = '?';
    if (c2 < ' ' || c2 > '~') c2 = '?';
    if (c3 < ' ' || c3 > '~') c3 = '?';
    snprintf(buf, len, "%c%c%c rev %u", c1, c2, c3, (unsigned int)(id & 0xffU));
}

/* Test whether writing rate to reg sticks as (rate * clock) / 48000. */
static bool ac97_test_rate(ac97_codec_t *codec, uint16_t reg, unsigned int rate)
{
    unsigned int target = (rate * codec->bus->clock) / 48000U;
    if (target == 0 || target > 0xffffU) return false;
    ac97_write_cache(codec, reg, (uint16_t)target);
    return ac97_read(codec, reg) == (uint16_t)target;
}

/* Probe supported sample rates for an AC'97 DAC/ADC rate register. */
static unsigned int ac97_determine_rates(ac97_codec_t *codec, uint16_t reg)
{
    if (codec->bus->no_vra) return ULSA_PCM_RATE_48000;

    uint16_t saved = ac97_read(codec, reg);
    if ((codec->ext_id & AC97_EI_DRA) && reg == AC97_PCM_FRONT_DAC_RATE) { ac97_update_bits(codec, AC97_EXTENDED_STATUS, AC97_EA_DRA, 0); }

    unsigned int mask = 0;
    if (ac97_test_rate(codec, reg, 11000)) mask |= ULSA_PCM_RATE_CONTINUOUS;
    for (size_t i = 0; i < sizeof(ac97_standard_rates) / sizeof(ac97_standard_rates[0]); i++) {
        if (ac97_test_rate(codec, reg, ac97_standard_rates[i].hz)) mask |= ac97_standard_rates[i].rate_bit;
    }

    ac97_write_cache(codec, reg, saved ? saved : 48000U);
    return mask ? mask : ULSA_PCM_RATE_48000;
}

/* Program an AC'97 rate register (DAC, LR ADC, or MIC ADC). */
int ac97_set_rate(ac97_codec_t *codec, uint16_t reg, unsigned int rate)
{
    if (!codec || !codec->bus) return -EINVAL;

    uint16_t eacs = ac97_read_cache(codec, AC97_EXTENDED_STATUS);
    switch (reg) {
        case AC97_PCM_FRONT_DAC_RATE :
        case AC97_PCM_LR_ADC_RATE :
            if (!(eacs & AC97_EA_VRA) && rate != 48000U) return -EINVAL;
            break;
        case AC97_PCM_MIC_ADC_RATE :
            if (!(eacs & AC97_EA_VRM) && rate != 48000U) return -EINVAL;
            break;
        default :
            return -EINVAL;
    }

    unsigned int scaled = (rate * codec->bus->clock) / 48000U;
    if (scaled == 0 || scaled > 0xffffU) return -EINVAL;

    if ((codec->ext_id & AC97_EI_DRA) && reg == AC97_PCM_FRONT_DAC_RATE) { ac97_update_bits(codec, AC97_EXTENDED_STATUS, AC97_EA_DRA, 0); }
    ac97_write_cache(codec, reg, (uint16_t)scaled);
    (void)ac97_read(codec, reg);
    return 0;
}

/* Test whether a single bit in an AC'97 register is writable. */
static bool ac97_try_bit(ac97_codec_t *codec, uint16_t reg, int bit)
{
    uint16_t mask = (uint16_t)(1U << bit);
    uint16_t orig = ac97_read(codec, reg);
    uint16_t test = (uint16_t)(orig ^ mask);
    ac97_write(codec, reg, test);
    uint16_t read_back = ac97_read(codec, reg);
    ac97_write_cache(codec, reg, orig);
    return read_back == test;
}

/* Test whether an AC'97 volume/mute register is implemented by the codec. */
static bool ac97_try_volume_mix(ac97_codec_t *codec, uint16_t reg)
{
    uint16_t val = ac97_read(codec, reg);
    if (!(val & AC97_MUTE_MASK_MONO)) {
        ac97_write_cache(codec, reg, (uint16_t)(val | AC97_MUTE_MASK_MONO));
        (void)ac97_read(codec, reg);
        val = ac97_read(codec, reg);
        if (!(val & AC97_MUTE_MASK_MONO)) return false;
    }
    return true;
}

/* Detect left and right attenuation bit resolution (6/5/4 bits). */
static void ac97_check_volume_resolution(ac97_codec_t *codec, uint16_t reg, unsigned char *lo_max, unsigned char *hi_max)
{
    static const uint16_t      cbit[3] = {0x20, 0x10, 0x01};
    static const unsigned char maxv[3] = {63, 31, 15};

    *lo_max = 0;
    *hi_max = 0;
    for (size_t i = 0; i < 3; i++) {
        uint16_t probe = (uint16_t)(AC97_MUTE_MASK_STEREO | cbit[i] | (uint16_t)(cbit[i] << 8));
        ac97_write(codec, reg, probe);
        (void)ac97_read(codec, reg);
        uint16_t val = ac97_read(codec, reg);
        if (!*lo_max && (val & 0x7fU) == cbit[i]) *lo_max = maxv[i];
        if (!*hi_max && ((val >> 8) & 0x7fU) == cbit[i]) *hi_max = maxv[i];
        if (*lo_max && *hi_max) break;
    }
}

/* Find the TLV dB scale array for an AC'97 volume control. */
static const unsigned int *ac97_find_tlv(uint16_t reg, unsigned int maxval, bool is_gain_zero_db)
{
    if (reg == AC97_REC_GAIN) return ac97_tlv_db_rec_gain;
    if (is_gain_zero_db && maxval == 31) return ac97_tlv_db_5bit_12dbmax;
    switch (maxval) {
        case 63 :
            return ac97_tlv_db_6bit;
        case 31 :
            return ac97_tlv_db_5bit;
        case 15 :
            return ac97_tlv_db_4bit;
        default :
            return NULL;
    }
}

/* ULSA kcontrol info callback for AC'97 boolean/integer controls. */
static int ac97_ctl_volsw_info(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_info *uinfo)
{
    unsigned long pv     = kctl->private_value;
    int           shift  = AC97_VOLSW_SHIFT(pv);
    int           rshift = AC97_VOLSW_RSHIFT(pv);
    unsigned int  mask   = AC97_VOLSW_MASK(pv);

    uinfo->type              = mask == 1U ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
    uinfo->count             = shift == rshift ? 1U : 2U;
    uinfo->value.integer.min = 0;
    uinfo->value.integer.max = (long)mask;
    return EOK;
}

/* ULSA kcontrol get callback reading a cached AC'97 register. */
static int ac97_ctl_volsw_get(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    ac97_codec_t *codec  = kctl->private_data;
    unsigned long pv     = kctl->private_value;
    uint16_t      reg    = AC97_VOLSW_REG(pv);
    int           shift  = AC97_VOLSW_SHIFT(pv);
    int           rshift = AC97_VOLSW_RSHIFT(pv);
    unsigned int  mask   = AC97_VOLSW_MASK(pv);
    int           invert = AC97_VOLSW_INVERT(pv);

    uint16_t     raw                 = ac97_read_cache(codec, reg);
    unsigned int v0                  = (raw >> shift) & mask;
    ucontrol->value.integer.value[0] = (long)(invert ? mask - v0 : v0);
    if (shift != rshift) {
        unsigned int v1                  = (raw >> rshift) & mask;
        ucontrol->value.integer.value[1] = (long)(invert ? mask - v1 : v1);
    }
    return EOK;
}

/* ULSA kcontrol put callback updating an AC'97 register. */
static int ac97_ctl_volsw_put(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    ac97_codec_t *codec  = kctl->private_data;
    unsigned long pv     = kctl->private_value;
    uint16_t      reg    = AC97_VOLSW_REG(pv);
    int           shift  = AC97_VOLSW_SHIFT(pv);
    int           rshift = AC97_VOLSW_RSHIFT(pv);
    unsigned int  mask   = AC97_VOLSW_MASK(pv);
    int           invert = AC97_VOLSW_INVERT(pv);

    long raw0 = ucontrol->value.integer.value[0];
    if (raw0 < 0 || raw0 > (long)mask) return -EINVAL;
    unsigned int v0 = (unsigned int)raw0;
    if (invert) v0 = mask - v0;

    uint16_t val_mask = (uint16_t)(mask << shift);
    uint16_t val      = (uint16_t)(v0 << shift);

    if (shift != rshift) {
        long raw1 = ucontrol->value.integer.value[1];
        if (raw1 < 0 || raw1 > (long)mask) return -EINVAL;
        unsigned int v1 = (unsigned int)raw1;
        if (invert) v1 = mask - v1;
        val_mask = (uint16_t)(val_mask | (uint16_t)(mask << rshift));
        val      = (uint16_t)(val | (uint16_t)(v1 << rshift));
    }

    return ac97_update_bits(codec, reg, val_mask, val);
}

/* ULSA kcontrol info callback for the stereo AC'97 Capture Source mux. */
static int ac97_ctl_capture_src_info(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_info *uinfo)
{
    (void)kctl;
    return ulsa_ctl_enum_info(uinfo, 2, 8, ac97_capture_sources);
}

/* ULSA kcontrol get callback for Capture Source (AC97_REC_SEL). */
static int ac97_ctl_capture_src_get(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    ac97_codec_t *codec                = kctl->private_data;
    uint16_t      raw                  = ac97_read_cache(codec, AC97_REC_SEL);
    ucontrol->value.enumerated.item[0] = (raw >> 8) & 7U;
    ucontrol->value.enumerated.item[1] = raw & 7U;
    return EOK;
}

/* ULSA kcontrol put callback for Capture Source (AC97_REC_SEL). */
static int ac97_ctl_capture_src_put(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    ac97_codec_t *codec = kctl->private_data;
    unsigned int  l     = ucontrol->value.enumerated.item[0];
    unsigned int  r     = ucontrol->value.enumerated.item[1];
    if (l >= 8 || r >= 8) return -EINVAL;
    uint16_t val = (uint16_t)(((l & 7U) << 8) | (r & 7U));
    return ac97_update_bits(codec, AC97_REC_SEL, 0x0707U, val);
}

/* Helper to register a single AC'97 kcontrol element on card. */
static int ac97_add_volsw_ctl(ac97_codec_t *codec, const char *name, unsigned long pv, const unsigned int *tlv)
{
    ulsa_kcontrol_new_t knew = {
        .iface         = SNDRV_CTL_ELEM_IFACE_MIXER,
        .name          = name,
        .index         = codec->num,
        .access        = SNDRV_CTL_ELEM_ACCESS_READWRITE | (tlv ? SNDRV_CTL_ELEM_ACCESS_TLV_READ : 0),
        .info          = ac97_ctl_volsw_info,
        .get           = ac97_ctl_volsw_get,
        .put           = ac97_ctl_volsw_put,
        .tlv           = tlv,
        .private_value = pv,
    };
    ulsa_kcontrol_t *kctl = ulsa_ctl_new1(&knew, codec);
    if (!kctl) return -ENOMEM;
    int err = ulsa_ctl_add(codec->bus->card, kctl);
    if (err < 0) free(kctl);
    return err;
}

/* Register playback switch+volume controls for one AC'97 mixer descriptor. */
static int ac97_build_one_playback(ac97_codec_t *codec, const ac97_mix_desc_t *desc)
{
    if (!ac97_try_volume_mix(codec, desc->reg)) return EOK;

    char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
    if (ac97_try_bit(codec, desc->reg, 15)) {
        snprintf(name, sizeof(name), "%s Switch", desc->prefix);
        bool     stereo = (codec->flags & AC97_STEREO_MUTES) != 0;
        uint16_t probe  = (uint16_t)(ac97_read(codec, desc->reg) | AC97_MUTE_MASK_STEREO);
        ac97_write(codec, desc->reg, probe);
        if (ac97_read(codec, desc->reg) == probe) stereo = true;

        unsigned long pv  = stereo ? AC97_PACK_VOLSW(desc->reg, 15, 7, 1, 1) : AC97_PACK_VOLSW(desc->reg, 15, 15, 1, 1);
        int           err = ac97_add_volsw_ctl(codec, name, pv, NULL);
        if (err < 0) return err;
    }

    unsigned char lo_max = 0, hi_max = 0;
    ac97_check_volume_resolution(codec, desc->reg, &lo_max, &hi_max);
    if (lo_max > 0) {
        snprintf(name, sizeof(name), "%s Volume", desc->prefix);
        unsigned long       pv  = hi_max > 0 ? AC97_PACK_VOLSW(desc->reg, 8, 0, lo_max, 1) : AC97_PACK_VOLSW(desc->reg, 0, 0, lo_max, 1);
        const unsigned int *tlv = ac97_find_tlv(desc->reg, lo_max, desc->is_gain_zero_db);
        int                 err = ac97_add_volsw_ctl(codec, name, pv, tlv);
        if (err < 0) return err;
    }

    /* Default Master/PCM to 0 dB unmuted so playback works cleanly out of box. */
    uint16_t init_val = desc->default_unmuted ? 0x0000U : AC97_MUTE_MASK_MONO;
    ac97_write_cache(codec, desc->reg, init_val);
    return EOK;
}

/* Build all standard AC'97 playback and capture ULSA mixer controls. */
static int ac97_mixer_build(ac97_codec_t *codec)
{
    for (size_t i = 0; i < sizeof(ac97_playback_mixers) / sizeof(ac97_playback_mixers[0]); i++) {
        int err = ac97_build_one_playback(codec, &ac97_playback_mixers[i]);
        if (err < 0) return err;
    }

    /* Capture source, switch, and gain */
    ulsa_kcontrol_new_t src_new = {
        .iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
        .name   = "Capture Source",
        .index  = codec->num,
        .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
        .info   = ac97_ctl_capture_src_info,
        .get    = ac97_ctl_capture_src_get,
        .put    = ac97_ctl_capture_src_put,
    };
    ulsa_kcontrol_t *src_kctl = ulsa_ctl_new1(&src_new, codec);
    if (!src_kctl) return -ENOMEM;
    int err = ulsa_ctl_add(codec->bus->card, src_kctl);
    if (err < 0) {
        free(src_kctl);
        return err;
    }

    if (ac97_try_bit(codec, AC97_REC_GAIN, 15)) {
        err = ac97_add_volsw_ctl(codec, "Capture Switch", AC97_PACK_VOLSW(AC97_REC_GAIN, 15, 15, 1, 1), NULL);
        if (err < 0) return err;
    }
    err = ac97_add_volsw_ctl(codec, "Capture Volume", AC97_PACK_VOLSW(AC97_REC_GAIN, 8, 0, 15, 0), ac97_tlv_db_rec_gain);
    if (err < 0) return err;

    ac97_write_cache(codec, AC97_REC_SEL, 0x0000U);
    ac97_write_cache(codec, AC97_REC_GAIN, 0x0000U);
    return EOK;
}

/* Allocate and initialize an AC'97 codec bus attached to card. */
ac97_bus_t *ac97_bus_create(ulsa_card_t *card, const ac97_bus_ops_t *ops, void *private_data)
{
    if (!card || !ops || !ops->read || !ops->write) return NULL;
    ac97_bus_t *bus = calloc(1, sizeof(ac97_bus_t));
    if (!bus) return NULL;
    bus->card         = card;
    bus->ops          = ops;
    bus->clock        = 48000U;
    bus->private_data = private_data;
    return bus;
}

/* Free an AC'97 codec bus and all attached codec structures. */
void ac97_bus_destroy(ac97_bus_t *bus)
{
    if (!bus) return;
    for (unsigned int i = 0; i < AC97_BUS_MAX_CODECS; i++) {
        free(bus->codecs[i]);
        bus->codecs[i] = NULL;
    }
    free(bus);
}

/* Probe an AC'97 codec on bus, detect VRA/rates, and populate ULSA mixer controls. */
int ac97_codec_probe(ac97_bus_t *bus, unsigned int num, void *private_data, ac97_codec_t **out_codec)
{
    if (!bus || num >= AC97_BUS_MAX_CODECS) return -EINVAL;

    ac97_codec_t *codec = calloc(1, sizeof(ac97_codec_t));
    if (!codec) return -ENOMEM;

    codec->bus          = bus;
    codec->num          = num;
    codec->private_data = private_data;
    mutex_init(&codec->reg_mutex);

    if (bus->ops->reset) bus->ops->reset(codec);

    uint16_t id1 = ac97_read(codec, AC97_VENDOR_ID1);
    uint16_t id2 = ac97_read(codec, AC97_VENDOR_ID2);
    if ((id1 == 0xffffU && id2 == 0xffffU) || (id1 == 0 && id2 == 0)) {
        uint16_t rst = ac97_read(codec, AC97_RESET);
        if (rst == 0 || rst == 0xffffU) {
            free(codec);
            return -EIO;
        }
    }
    codec->id = ((uint32_t)id1 << 16) | id2;

    /* Power up analog subsections and reset mixer to defaults. */
    ac97_write_cache(codec, AC97_POWERDOWN, 0x0000U);
    ac97_write_cache(codec, AC97_RESET, 0x0000U);
    usleep(100);
    ac97_write_cache(codec, AC97_POWERDOWN, 0x0000U);
    ac97_write_cache(codec, AC97_GENERAL_PURPOSE, 0x0000U);

    for (int i = 0; i < 250; i++) {
        if ((ac97_read(codec, AC97_POWERDOWN) & 0x0fU) == 0x0fU) break;
        msleep(2);
    }

    codec->caps   = ac97_read(codec, AC97_RESET);
    codec->ext_id = ac97_read(codec, AC97_EXTENDED_ID);
    codec->addr   = (codec->ext_id & AC97_EI_ADDR_MASK) >> AC97_EI_ADDR_SHIFT;

    if (codec->ext_id & 0x01c9U) {
        uint16_t eacs = ac97_read(codec, AC97_EXTENDED_STATUS);
        eacs |= (uint16_t)(codec->ext_id & 0x01c0U);
        if (!bus->no_vra) eacs |= (uint16_t)(codec->ext_id & 0x0009U);
        ac97_write_cache(codec, AC97_EXTENDED_STATUS, eacs);
    }

    if (codec->ext_id & AC97_EI_VRA) {
        codec->rates_dac = ac97_determine_rates(codec, AC97_PCM_FRONT_DAC_RATE);
        codec->rates_adc = ac97_determine_rates(codec, AC97_PCM_LR_ADC_RATE);
    } else {
        codec->rates_dac = ULSA_PCM_RATE_48000;
        codec->rates_adc = ULSA_PCM_RATE_48000;
    }

    if (codec->ext_id & AC97_EI_VRM) {
        codec->rates_mic = ac97_determine_rates(codec, AC97_PCM_MIC_ADC_RATE);
    } else {
        codec->rates_mic = ULSA_PCM_RATE_48000;
    }

    ac97_codec_short_name(codec->id, codec->name, sizeof(codec->name));
    if (bus->card->mixername[0] == '\0') { snprintf(bus->card->mixername, sizeof(bus->card->mixername), "%s", codec->name); }

    char comp[24];
    snprintf(comp, sizeof(comp), "AC97a:%08x", codec->id);
    (void)ulsa_component_add(bus->card, comp);

    int err = ac97_mixer_build(codec);
    if (err < 0) {
        free(codec);
        return err;
    }

    bus->codecs[num] = codec;
    if (num >= bus->num_codecs) bus->num_codecs = num + 1;
    if (out_codec) *out_codec = codec;
    return EOK;
}

#endif // CONFIG_AUDIO && CONFIG_AUDIO_AC97
