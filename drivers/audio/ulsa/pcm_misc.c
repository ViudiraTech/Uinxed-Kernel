/*
 *
 *      pcm_misc.c
 *      ULSA sample format table and silence helpers
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <libs/std/string.h>

#if CONFIG_AUDIO

/* Per-format width data: significant bits, container bits, endianness, signedness. */
struct ulsa_format_data {
        uint8_t width;
        uint8_t phys;
        int8_t  le;    // 1 little, 0 big, -1 n/a
        int8_t  signd; // 1 signed, 0 unsigned, -1 n/a (float/compressed)
};

static const struct ulsa_format_data format_table[SNDRV_PCM_FORMAT_LAST + 1] = {
    [SNDRV_PCM_FORMAT_S8]                 = {8,  8,  -1, 1 },
    [SNDRV_PCM_FORMAT_U8]                 = {8,  8,  -1, 0 },
    [SNDRV_PCM_FORMAT_S16_LE]             = {16, 16, 1,  1 },
    [SNDRV_PCM_FORMAT_S16_BE]             = {16, 16, 0,  1 },
    [SNDRV_PCM_FORMAT_U16_LE]             = {16, 16, 1,  0 },
    [SNDRV_PCM_FORMAT_U16_BE]             = {16, 16, 0,  0 },
    [SNDRV_PCM_FORMAT_S24_LE]             = {24, 32, 1,  1 },
    [SNDRV_PCM_FORMAT_S24_BE]             = {24, 32, 0,  1 },
    [SNDRV_PCM_FORMAT_U24_LE]             = {24, 32, 1,  0 },
    [SNDRV_PCM_FORMAT_U24_BE]             = {24, 32, 0,  0 },
    [SNDRV_PCM_FORMAT_S32_LE]             = {32, 32, 1,  1 },
    [SNDRV_PCM_FORMAT_S32_BE]             = {32, 32, 0,  1 },
    [SNDRV_PCM_FORMAT_U32_LE]             = {32, 32, 1,  0 },
    [SNDRV_PCM_FORMAT_U32_BE]             = {32, 32, 0,  0 },
    [SNDRV_PCM_FORMAT_FLOAT_LE]           = {32, 32, 1,  -1},
    [SNDRV_PCM_FORMAT_FLOAT_BE]           = {32, 32, 0,  -1},
    [SNDRV_PCM_FORMAT_FLOAT64_LE]         = {64, 64, 1,  -1},
    [SNDRV_PCM_FORMAT_FLOAT64_BE]         = {64, 64, 0,  -1},
    [SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE] = {32, 32, 1,  -1},
    [SNDRV_PCM_FORMAT_IEC958_SUBFRAME_BE] = {32, 32, 0,  -1},
    [SNDRV_PCM_FORMAT_MU_LAW]             = {8,  8,  -1, -1},
    [SNDRV_PCM_FORMAT_A_LAW]              = {8,  8,  -1, -1},
    [SNDRV_PCM_FORMAT_IMA_ADPCM]          = {4,  4,  -1, -1},
    [SNDRV_PCM_FORMAT_MPEG]               = {0,  0,  -1, -1},
    [SNDRV_PCM_FORMAT_GSM]                = {0,  0,  -1, -1},
    [SNDRV_PCM_FORMAT_S20_LE]             = {20, 32, 1,  1 },
    [SNDRV_PCM_FORMAT_S20_BE]             = {20, 32, 0,  1 },
    [SNDRV_PCM_FORMAT_U20_LE]             = {20, 32, 1,  0 },
    [SNDRV_PCM_FORMAT_U20_BE]             = {20, 32, 0,  0 },
    [SNDRV_PCM_FORMAT_SPECIAL]            = {0,  0,  -1, -1},
    [SNDRV_PCM_FORMAT_S24_3LE]            = {24, 24, 1,  1 },
    [SNDRV_PCM_FORMAT_S24_3BE]            = {24, 24, 0,  1 },
    [SNDRV_PCM_FORMAT_U24_3LE]            = {24, 24, 1,  0 },
    [SNDRV_PCM_FORMAT_U24_3BE]            = {24, 24, 0,  0 },
    [SNDRV_PCM_FORMAT_S20_3LE]            = {20, 24, 1,  1 },
    [SNDRV_PCM_FORMAT_S20_3BE]            = {20, 24, 0,  1 },
    [SNDRV_PCM_FORMAT_U20_3LE]            = {20, 24, 1,  0 },
    [SNDRV_PCM_FORMAT_U20_3BE]            = {20, 24, 0,  0 },
    [SNDRV_PCM_FORMAT_S18_3LE]            = {18, 24, 1,  1 },
    [SNDRV_PCM_FORMAT_S18_3BE]            = {18, 24, 0,  1 },
    [SNDRV_PCM_FORMAT_U18_3LE]            = {18, 24, 1,  0 },
    [SNDRV_PCM_FORMAT_U18_3BE]            = {18, 24, 0,  0 },
    [SNDRV_PCM_FORMAT_G723_24]            = {3,  3,  -1, -1},
    [SNDRV_PCM_FORMAT_G723_24_1B]         = {3,  8,  -1, -1},
    [SNDRV_PCM_FORMAT_G723_40]            = {5,  5,  -1, -1},
    [SNDRV_PCM_FORMAT_G723_40_1B]         = {5,  8,  -1, -1},
    [SNDRV_PCM_FORMAT_DSD_U8]             = {8,  8,  1,  0 },
    [SNDRV_PCM_FORMAT_DSD_U16_LE]         = {16, 16, 1,  0 },
    [SNDRV_PCM_FORMAT_DSD_U32_LE]         = {32, 32, 1,  0 },
    [SNDRV_PCM_FORMAT_DSD_U16_BE]         = {16, 16, 0,  0 },
    [SNDRV_PCM_FORMAT_DSD_U32_BE]         = {32, 32, 0,  0 },
};

/* Return the table entry of a defined format, or NULL. */
static const struct ulsa_format_data *format_data(snd_pcm_format_t format)
{
    if (format < 0 || format > SNDRV_PCM_FORMAT_LAST) return NULL;
    if (!format_table[format].phys && format != SNDRV_PCM_FORMAT_MPEG && format != SNDRV_PCM_FORMAT_GSM && format != SNDRV_PCM_FORMAT_SPECIAL) return NULL;
    return &format_table[format];
}

/* Significant bits of a sample. */
int ulsa_pcm_format_width(snd_pcm_format_t format)
{
    const struct ulsa_format_data *d = format_data(format);
    if (!d || !d->width) return -EINVAL;
    return d->width;
}

/* Bits a sample occupies in memory. */
int ulsa_pcm_format_physical_width(snd_pcm_format_t format)
{
    const struct ulsa_format_data *d = format_data(format);
    if (!d || !d->phys) return -EINVAL;
    return d->phys;
}

/* 1 = signed, 0 = unsigned, -EINVAL = not a linear integer format. */
int ulsa_pcm_format_signed(snd_pcm_format_t format)
{
    const struct ulsa_format_data *d = format_data(format);
    if (!d || d->signd < 0) return -EINVAL;
    return d->signd;
}

/* True for linear integer PCM. */
int ulsa_pcm_format_linear(snd_pcm_format_t format)
{
    return ulsa_pcm_format_signed(format) >= 0;
}

/* Write `samples` silent samples of `format` to buf. */
int ulsa_pcm_format_set_silence(snd_pcm_format_t format, void *buf, size_t samples)
{
    const struct ulsa_format_data *d = format_data(format);
    uint8_t                        pattern[8];
    size_t                         bytes;
    uint8_t                       *dst = buf;

    if (!d || !d->phys) return -EINVAL;
    if (!samples) return EOK;
    if (d->phys < 8) {
        memset(buf, 0, (samples * d->phys + 7) / 8);
        return EOK;
    }
    bytes = d->phys / 8;
    memset(pattern, 0, sizeof(pattern));

    if (format == SNDRV_PCM_FORMAT_MU_LAW) {
        memset(buf, 0x7f, samples);
        return EOK;
    }
    if (format == SNDRV_PCM_FORMAT_A_LAW) {
        memset(buf, 0x55, samples);
        return EOK;
    }
    if (d->signd != 0) {
        /* Signed integers, floats and IEC958 frames are silent at all-zero. */
        memset(buf, 0, samples * bytes);
        return EOK;
    }

    /* Unsigned linear and DSD: the midpoint, i.e. the top bit of the significant width. */
    if (format >= SNDRV_PCM_FORMAT_DSD_U8 && format <= SNDRV_PCM_FORMAT_DSD_U32_BE) {
        memset(buf, 0x69, samples * bytes);
        return EOK;
    }
    {
        uint64_t mid = 1ULL << (d->width - 1);
        for (size_t b = 0; b < bytes; b++) {
            size_t shift = d->le ? b : bytes - 1 - b;
            pattern[b]   = (uint8_t)(mid >> (8 * shift));
        }
    }
    for (size_t s = 0; s < samples; s++) memcpy(dst + s * bytes, pattern, bytes);
    return EOK;
}

#endif
