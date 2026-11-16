/*
 *
 *      info.c
 *      ULSA /proc/asound content formatting and lookup helpers
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <kernel/debug/vsprintf.h>
#include <kernel/timer/timer.h>
#include <kernel/uinxed.h>
#include <libs/std/stdarg.h>
#include <libs/std/string.h>

#if CONFIG_AUDIO

struct info_cursor {
        char  *buf;
        size_t cap;
        size_t pos;
};

/* Append formatted text safely into a fixed-capacity buffer. */
__attribute__((format(printf, 2, 3))) static void info_printf(struct info_cursor *c, const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (!c->buf || c->pos >= c->cap) return;
    va_start(ap, fmt);
    n = vsnprintf(c->buf + c->pos, c->cap - c->pos, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= c->cap - c->pos) {
        c->pos = c->cap - 1;
    } else {
        c->pos += (size_t)n;
    }
}

/* Unpack (card, pcm_dev, stream, sub) from a 31-bit procfs cookie. */
static void info_unpack_id(uint32_t packed, int *card, int *pcm_dev, int *stream, unsigned int *sub)
{
    *card    = (int)((packed >> 20) & 0xffU);
    *pcm_dev = (int)((packed >> 12) & 0xffU);
    *stream  = (int)((packed >> 11) & 1U);
    *sub     = packed & 0x7ffU;
}

/* Return the ALSA name for a PCM access mode. */
static const char *pcm_access_name(snd_pcm_access_t access)
{
    switch (access) {
        case SNDRV_PCM_ACCESS_MMAP_INTERLEAVED :
            return "MMAP_INTERLEAVED";
        case SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED :
            return "MMAP_NONINTERLEAVED";
        case SNDRV_PCM_ACCESS_MMAP_COMPLEX :
            return "MMAP_COMPLEX";
        case SNDRV_PCM_ACCESS_RW_INTERLEAVED :
            return "RW_INTERLEAVED";
        case SNDRV_PCM_ACCESS_RW_NONINTERLEAVED :
            return "RW_NONINTERLEAVED";
        default :
            return "UNKNOWN";
    }
}

/* Return the ALSA name for a PCM subformat. */
static const char *pcm_subformat_name(snd_pcm_subformat_t subfmt)
{
    switch (subfmt) {
        case SNDRV_PCM_SUBFORMAT_STD :
            return "STD";
        case SNDRV_PCM_SUBFORMAT_MSBITS_MAX :
            return "MSBITS_MAX";
        case SNDRV_PCM_SUBFORMAT_MSBITS_20 :
            return "MSBITS_20";
        case SNDRV_PCM_SUBFORMAT_MSBITS_24 :
            return "MSBITS_24";
        default :
            return "UNKNOWN";
    }
}

/* Return the ALSA name for a PCM sample format. */
static const char *pcm_format_name(snd_pcm_format_t fmt)
{
    static const char *const names[SNDRV_PCM_FORMAT_LAST + 1] = {
        [SNDRV_PCM_FORMAT_S8]                 = "S8",
        [SNDRV_PCM_FORMAT_U8]                 = "U8",
        [SNDRV_PCM_FORMAT_S16_LE]             = "S16_LE",
        [SNDRV_PCM_FORMAT_S16_BE]             = "S16_BE",
        [SNDRV_PCM_FORMAT_U16_LE]             = "U16_LE",
        [SNDRV_PCM_FORMAT_U16_BE]             = "U16_BE",
        [SNDRV_PCM_FORMAT_S24_LE]             = "S24_LE",
        [SNDRV_PCM_FORMAT_S24_BE]             = "S24_BE",
        [SNDRV_PCM_FORMAT_U24_LE]             = "U24_LE",
        [SNDRV_PCM_FORMAT_U24_BE]             = "U24_BE",
        [SNDRV_PCM_FORMAT_S32_LE]             = "S32_LE",
        [SNDRV_PCM_FORMAT_S32_BE]             = "S32_BE",
        [SNDRV_PCM_FORMAT_U32_LE]             = "U32_LE",
        [SNDRV_PCM_FORMAT_U32_BE]             = "U32_BE",
        [SNDRV_PCM_FORMAT_FLOAT_LE]           = "FLOAT_LE",
        [SNDRV_PCM_FORMAT_FLOAT_BE]           = "FLOAT_BE",
        [SNDRV_PCM_FORMAT_FLOAT64_LE]         = "FLOAT64_LE",
        [SNDRV_PCM_FORMAT_FLOAT64_BE]         = "FLOAT64_BE",
        [SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE] = "IEC958_SUBFRAME_LE",
        [SNDRV_PCM_FORMAT_IEC958_SUBFRAME_BE] = "IEC958_SUBFRAME_BE",
        [SNDRV_PCM_FORMAT_MU_LAW]             = "MU_LAW",
        [SNDRV_PCM_FORMAT_A_LAW]              = "A_LAW",
        [SNDRV_PCM_FORMAT_IMA_ADPCM]          = "IMA_ADPCM",
        [SNDRV_PCM_FORMAT_MPEG]               = "MPEG",
        [SNDRV_PCM_FORMAT_GSM]                = "GSM",
        [SNDRV_PCM_FORMAT_S20_LE]             = "S20_LE",
        [SNDRV_PCM_FORMAT_S20_BE]             = "S20_BE",
        [SNDRV_PCM_FORMAT_U20_LE]             = "U20_LE",
        [SNDRV_PCM_FORMAT_U20_BE]             = "U20_BE",
        [SNDRV_PCM_FORMAT_SPECIAL]            = "SPECIAL",
        [SNDRV_PCM_FORMAT_S24_3LE]            = "S24_3LE",
        [SNDRV_PCM_FORMAT_S24_3BE]            = "S24_3BE",
        [SNDRV_PCM_FORMAT_U24_3LE]            = "U24_3LE",
        [SNDRV_PCM_FORMAT_U24_3BE]            = "U24_3BE",
        [SNDRV_PCM_FORMAT_S20_3LE]            = "S20_3LE",
        [SNDRV_PCM_FORMAT_S20_3BE]            = "S20_3BE",
        [SNDRV_PCM_FORMAT_U20_3LE]            = "U20_3LE",
        [SNDRV_PCM_FORMAT_U20_3BE]            = "U20_3BE",
        [SNDRV_PCM_FORMAT_S18_3LE]            = "S18_3LE",
        [SNDRV_PCM_FORMAT_S18_3BE]            = "S18_3BE",
        [SNDRV_PCM_FORMAT_U18_3LE]            = "U18_3LE",
        [SNDRV_PCM_FORMAT_U18_3BE]            = "U18_3BE",
        [SNDRV_PCM_FORMAT_G723_24]            = "G723_24",
        [SNDRV_PCM_FORMAT_G723_24_1B]         = "G723_24_1B",
        [SNDRV_PCM_FORMAT_G723_40]            = "G723_40",
        [SNDRV_PCM_FORMAT_G723_40_1B]         = "G723_40_1B",
        [SNDRV_PCM_FORMAT_DSD_U8]             = "DSD_U8",
        [SNDRV_PCM_FORMAT_DSD_U16_LE]         = "DSD_U16_LE",
        [SNDRV_PCM_FORMAT_DSD_U32_LE]         = "DSD_U32_LE",
        [SNDRV_PCM_FORMAT_DSD_U16_BE]         = "DSD_U16_BE",
        [SNDRV_PCM_FORMAT_DSD_U32_BE]         = "DSD_U32_BE",
    };

    if (fmt >= 0 && fmt <= SNDRV_PCM_FORMAT_LAST && names[fmt]) return names[fmt];
    return "UNKNOWN";
}

/* Return the ALSA name for a PCM stream state. */
static const char *pcm_state_name(snd_pcm_state_t state)
{
    static const char *const names[] = {
        [SNDRV_PCM_STATE_OPEN] = "OPEN",       [SNDRV_PCM_STATE_SETUP] = "SETUP",         [SNDRV_PCM_STATE_PREPARED] = "PREPARED",
        [SNDRV_PCM_STATE_RUNNING] = "RUNNING", [SNDRV_PCM_STATE_XRUN] = "XRUN",           [SNDRV_PCM_STATE_DRAINING] = "DRAINING",
        [SNDRV_PCM_STATE_PAUSED] = "PAUSED",   [SNDRV_PCM_STATE_SUSPENDED] = "SUSPENDED", [SNDRV_PCM_STATE_DISCONNECTED] = "DISCONNECTED",
    };

    if (state >= 0 && state <= SNDRV_PCM_STATE_LAST && names[state]) return names[state];
    return "UNKNOWN";
}

/* Return the human-readable device type description for /proc/asound/devices. */
static const char *minor_type_name(enum ulsa_device_type type)
{
    switch (type) {
        case ULSA_DEVICE_CONTROL :
            return "control";
        case ULSA_DEVICE_PCM_PLAYBACK :
            return "digital audio playback";
        case ULSA_DEVICE_PCM_CAPTURE :
            return "digital audio capture";
        case ULSA_DEVICE_TIMER :
            return "timer";
        default :
            return "?";
    }
}

/* Locate a PCM substream by packed cookie, or NULL if absent. */
static ulsa_pcm_substream_t *lookup_substream(uint32_t packed_id)
{
    int                   card_idx, pcm_dev, stream;
    unsigned int          sub_idx;
    ulsa_card_t          *card;
    ulsa_pcm_t           *pcm;
    ulsa_pcm_substream_t *s;

    info_unpack_id(packed_id, &card_idx, &pcm_dev, &stream, &sub_idx);
    if (pcm_dev < 0 || pcm_dev >= SNDRV_MINOR_PCMS || stream < 0 || stream > 1) return NULL;
    card = ulsa_card_get(card_idx);
    if (!card) return NULL;
    pcm = card->pcms[pcm_dev];
    if (!pcm) return NULL;
    s = pcm->streams[stream].substream;
    for (unsigned int i = 0; s && i < sub_idx; i++) s = s->next;
    return s;
}

/* Format /proc/asound/cards. */
static void info_read_cards(struct info_cursor *c)
{
    int count = 0;

    for (int i = 0; i < SNDRV_CARDS; i++) {
        ulsa_card_t *card = ulsa_card_get(i);
        if (!card) continue;
        count++;
        info_printf(c, "%2i [%-15s]: %s - %s\n", i, card->id, card->driver, card->shortname);
        info_printf(c, "                      %s\n", card->longname);
    }
    if (!count) info_printf(c, "--- no soundcards ---\n");
}

/* Format /proc/asound/devices. */
static void info_read_devices(struct info_cursor *c)
{
    for (int m = 0; m < 256; m++) {
        ulsa_minor_t *minor = ulsa_minor_get(m);
        if (!minor || !minor->published) continue;
        if (minor->card) {
            if (minor->pcm) {
                info_printf(c, "%3i: [%2i-%2i]: %s\n", m, minor->card->number, minor->pcm->device, minor_type_name(minor->type));
            } else {
                info_printf(c, "%3i: [%2i]   : %s\n", m, minor->card->number, minor_type_name(minor->type));
            }
        } else {
            info_printf(c, "%3i:        : %s\n", m, minor_type_name(minor->type));
        }
    }
}

/* Format /proc/asound/pcm. */
static void info_read_pcm(struct info_cursor *c)
{
    for (int i = 0; i < SNDRV_CARDS; i++) {
        ulsa_card_t *card = ulsa_card_get(i);
        if (!card) continue;
        for (int d = 0; d < SNDRV_MINOR_PCMS; d++) {
            ulsa_pcm_t *pcm = card->pcms[d];
            if (!pcm) continue;
            info_printf(c, "%02i-%02i: %s : %s", card->number, pcm->device, pcm->id, pcm->name);
            if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream_count) info_printf(c, " : playback %u", pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream_count);
            if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream_count) info_printf(c, " : capture %u", pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream_count);
            info_printf(c, "\n");
        }
    }
}

/* Format /proc/asound/timers. */
static void info_read_timers(struct info_cursor *c)
{
    const unsigned long res = 1000000000UL / CONFIG_TIMER_HZ;

    info_printf(c, "G0: system timer : %lu.%03luus (10000000 ticks)\n", res / 1000UL, res % 1000UL);
    for (int i = 0; i < SNDRV_CARDS; i++) {
        ulsa_card_t *card = ulsa_card_get(i);
        if (!card) continue;
        for (int d = 0; d < SNDRV_MINOR_PCMS; d++) {
            ulsa_pcm_t *pcm = card->pcms[d];
            if (!pcm) continue;
            for (int st = 0; st < 2; st++) {
                for (ulsa_pcm_substream_t *s = pcm->streams[st].substream; s; s = s->next) {
                    if (!s->timer) continue;
                    int subdev = (s->number << 1) | (s->stream & 1);
                    info_printf(c, "P%i-%i-%i: PCM %s %i-%i-%i : SLAVE\n", card->number, pcm->device, subdev, st == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture", card->number, pcm->device,
                                subdev);
                }
            }
        }
    }
}

/* Format card<N>/pcm<D>{p,c}/info or .../sub<S>/info. */
static void info_read_pcm_info(struct info_cursor *c, uint32_t packed_id)
{
    ulsa_pcm_substream_t *substream = lookup_substream(packed_id);
    struct snd_pcm_info   info;
    int                   err;

    if (!substream) return;
    err = ulsa_pcm_info(substream, &info);
    if (err < 0) {
        info_printf(c, "error %d\n", err);
        return;
    }
    info_printf(c, "card: %d\n", info.card);
    info_printf(c, "device: %d\n", info.device);
    info_printf(c, "subdevice: %d\n", info.subdevice);
    info_printf(c, "stream: %s\n", info.stream == SNDRV_PCM_STREAM_PLAYBACK ? "PLAYBACK" : "CAPTURE");
    info_printf(c, "id: %s\n", info.id);
    info_printf(c, "name: %s\n", info.name);
    info_printf(c, "subname: %s\n", info.subname);
    info_printf(c, "class: %d\n", info.dev_class);
    info_printf(c, "subclass: %d\n", info.dev_subclass);
    info_printf(c, "subdevices_count: %d\n", info.subdevices_count);
    info_printf(c, "subdevices_avail: %d\n", info.subdevices_avail);
}

/* Format card<N>/pcm<D>{p,c}/sub<S>/hw_params. */
static void info_read_sub_hw_params(struct info_cursor *c, uint32_t packed_id)
{
    ulsa_pcm_substream_t *substream = lookup_substream(packed_id);
    ulsa_pcm_runtime_t   *runtime;

    if (!substream) return;
    mutex_lock(&substream->pcm->open_mutex);
    runtime = substream->runtime;
    if (!runtime) {
        info_printf(c, "closed\n");
    } else if (runtime->status->state == SNDRV_PCM_STATE_OPEN) {
        info_printf(c, "no setup\n");
    } else {
        info_printf(c, "access: %s\n", pcm_access_name(runtime->access));
        info_printf(c, "format: %s\n", pcm_format_name(runtime->format));
        info_printf(c, "subformat: %s\n", pcm_subformat_name(runtime->subformat));
        info_printf(c, "channels: %u\n", runtime->channels);
        info_printf(c, "rate: %u (%u/%u)\n", runtime->rate, runtime->rate_num, runtime->rate_den);
        info_printf(c, "period_size: %lu\n", (unsigned long)runtime->period_size);
        info_printf(c, "buffer_size: %lu\n", (unsigned long)runtime->buffer_size);
    }
    mutex_unlock(&substream->pcm->open_mutex);
}

/* Format card<N>/pcm<D>{p,c}/sub<S>/sw_params. */
static void info_read_sub_sw_params(struct info_cursor *c, uint32_t packed_id)
{
    ulsa_pcm_substream_t *substream = lookup_substream(packed_id);
    ulsa_pcm_runtime_t   *runtime;

    if (!substream) return;
    mutex_lock(&substream->pcm->open_mutex);
    runtime = substream->runtime;
    if (!runtime) {
        info_printf(c, "closed\n");
    } else if (runtime->status->state == SNDRV_PCM_STATE_OPEN) {
        info_printf(c, "no setup\n");
    } else {
        info_printf(c, "tstamp_mode: %s\n", runtime->tstamp_mode == SNDRV_PCM_TSTAMP_ENABLE ? "ENABLE" : "NONE");
        info_printf(c, "period_step: %u\n", runtime->period_step);
        info_printf(c, "avail_min: %lu\n", (unsigned long)runtime->control->avail_min);
        info_printf(c, "start_threshold: %lu\n", (unsigned long)runtime->start_threshold);
        info_printf(c, "stop_threshold: %lu\n", (unsigned long)runtime->stop_threshold);
        info_printf(c, "silence_threshold: %lu\n", (unsigned long)runtime->silence_threshold);
        info_printf(c, "silence_size: %lu\n", (unsigned long)runtime->silence_size);
        info_printf(c, "boundary: %lu\n", (unsigned long)runtime->boundary);
    }
    mutex_unlock(&substream->pcm->open_mutex);
}

/* Format card<N>/pcm<D>{p,c}/sub<S>/status. */
static void info_read_sub_status(struct info_cursor *c, uint32_t packed_id)
{
    ulsa_pcm_substream_t *substream = lookup_substream(packed_id);
    ulsa_pcm_runtime_t   *runtime;
    uint64_t              flags;

    if (!substream) return;
    mutex_lock(&substream->pcm->open_mutex);
    runtime = substream->runtime;
    if (!runtime) {
        info_printf(c, "closed\n");
        mutex_unlock(&substream->pcm->open_mutex);
        return;
    }

    flags = spin_lock_irqsave(&substream->lock);
    if (ulsa_pcm_running(substream)) (void)ulsa_pcm_update_hw_ptr(substream);
    const snd_pcm_state_t     state     = runtime->status->state;
    const snd_pcm_uframes_t   avail     = ulsa_pcm_avail(substream);
    const snd_pcm_sframes_t   hw_avail  = ulsa_pcm_hw_avail(substream);
    const snd_pcm_sframes_t   delay     = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? hw_avail : (snd_pcm_sframes_t)avail;
    const snd_pcm_uframes_t   avail_max = runtime->avail_max;
    const snd_pcm_uframes_t   hw_ptr    = runtime->status->hw_ptr;
    const snd_pcm_uframes_t   appl_ptr  = runtime->control->appl_ptr;
    const struct snd_timespec trig      = runtime->trigger_tstamp;
    struct snd_timespec       tstamp    = runtime->status->tstamp;
    if (runtime->tstamp_mode != SNDRV_PCM_TSTAMP_ENABLE) ulsa_pcm_gettime(runtime, &tstamp);
    spin_unlock_irqrestore(&substream->lock, flags);
    mutex_unlock(&substream->pcm->open_mutex);

    info_printf(c, "state: %s\n", pcm_state_name(state));
    info_printf(c, "owner_pid   : 0\n");
    info_printf(c, "trigger_time: %lld.%09lld\n", (long long)trig.tv_sec, (long long)trig.tv_nsec);
    info_printf(c, "tstamp      : %lld.%09lld\n", (long long)tstamp.tv_sec, (long long)tstamp.tv_nsec);
    info_printf(c, "delay       : %ld\n", (long)delay);
    info_printf(c, "avail       : %lu\n", (unsigned long)avail);
    info_printf(c, "avail_max   : %lu\n", (unsigned long)avail_max);
    info_printf(c, "-----\n");
    info_printf(c, "hw_ptr      : %lu\n", (unsigned long)hw_ptr);
    info_printf(c, "appl_ptr    : %lu\n", (unsigned long)appl_ptr);
}

/* Render any /proc/asound file into `buf` and return the byte count written. */
size_t ulsa_info_format(ulsa_proc_file_kind_t kind, uint32_t packed_id, char *buf, size_t cap)
{
    struct info_cursor c = {.buf = buf, .cap = cap, .pos = 0};

    if (!buf || !cap) return 0;
    buf[0] = '\0';

    switch (kind) {
        case ULSA_PROC_VERSION :
            info_printf(&c, "Advanced Linux Sound Architecture Driver Version k%s.\n", KERNEL_VERSION);
            break;
        case ULSA_PROC_CARDS :
            info_read_cards(&c);
            break;
        case ULSA_PROC_DEVICES :
            info_read_devices(&c);
            break;
        case ULSA_PROC_PCM :
            info_read_pcm(&c);
            break;
        case ULSA_PROC_TIMERS :
            info_read_timers(&c);
            break;
        case ULSA_PROC_CARD_ID : {
            int          card_idx, pcm_dev, stream;
            unsigned int sub_idx;
            info_unpack_id(packed_id, &card_idx, &pcm_dev, &stream, &sub_idx);
            ulsa_card_t *card = ulsa_card_get(card_idx);
            if (card) info_printf(&c, "%s\n", card->id);
            break;
        }
        case ULSA_PROC_PCM_INFO :
        case ULSA_PROC_SUB_INFO :
            info_read_pcm_info(&c, packed_id);
            break;
        case ULSA_PROC_SUB_HW_PARAMS :
            info_read_sub_hw_params(&c, packed_id);
            break;
        case ULSA_PROC_SUB_SW_PARAMS :
            info_read_sub_sw_params(&c, packed_id);
            break;
        case ULSA_PROC_SUB_STATUS :
            info_read_sub_status(&c, packed_id);
            break;
        default :
            break;
    }
    return c.pos;
}

/* Report whether card `card_idx` is registered and optionally copy its string id. */
bool ulsa_info_card_exists(int card_idx, char *id_out, size_t id_cap)
{
    ulsa_card_t *card = ulsa_card_get(card_idx);

    if (!card) return false;
    if (id_out && id_cap) {
        strncpy(id_out, card->id, id_cap - 1);
        id_out[id_cap - 1] = '\0';
    }
    return true;
}

/* Resolve a card id string (used for /proc/asound/<id> symlinks) to a card index, or -1. */
int ulsa_info_find_card_by_id(const char *id)
{
    if (!id || !id[0]) return -1;
    for (int i = 0; i < SNDRV_CARDS; i++) {
        ulsa_card_t *card = ulsa_card_get(i);
        if (card && strcmp(card->id, id) == 0) return i;
    }
    return -1;
}

/* Report whether a PCM direction exists on `(card_idx, pcm_dev, stream)` and return substream count. */
bool ulsa_info_pcm_exists(int card_idx, int pcm_dev, int stream, unsigned int *sub_count_out)
{
    ulsa_card_t *card = ulsa_card_get(card_idx);
    ulsa_pcm_t  *pcm;

    if (!card || pcm_dev < 0 || pcm_dev >= SNDRV_MINOR_PCMS || (stream != SNDRV_PCM_STREAM_PLAYBACK && stream != SNDRV_PCM_STREAM_CAPTURE)) return false;
    pcm = card->pcms[pcm_dev];
    if (!pcm || !pcm->streams[stream].substream_count) return false;
    if (sub_count_out) *sub_count_out = pcm->streams[stream].substream_count;
    return true;
}

/* Report whether a substream `sub_idx` exists under `(card_idx, pcm_dev, stream)`. */
bool ulsa_info_sub_exists(int card_idx, int pcm_dev, int stream, unsigned int sub_idx)
{
    unsigned int count = 0;

    if (!ulsa_info_pcm_exists(card_idx, pcm_dev, stream, &count)) return false;
    return sub_idx < count;
}

/* Initialize global /proc/asound state (no-op: procfs queries ULSA dynamically). */
int ulsa_info_init(void)
{
    return EOK;
}

/* Register per-card /proc/asound state (no-op: procfs enumerates live cards). */
int ulsa_info_card_register(ulsa_card_t *card)
{
    (void)card;
    return EOK;
}

/* Release per-card /proc/asound state (no-op: procfs enumerates live cards). */
void ulsa_info_card_free(ulsa_card_t *card)
{
    (void)card;
}

#endif
