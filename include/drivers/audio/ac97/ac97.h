/*
 *
 *      ac97.h
 *      AC'97 codec bus and Intel 8x0 controller driver header file
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_AUDIO_AC97_AC97_H_
#define INCLUDE_DRIVERS_AUDIO_AC97_AC97_H_

#include <drivers/audio/ulsa/ulsa.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/mutex.h>

/* AC'97 codec register map */
#define AC97_RESET              0x00 // Reset and basic capabilities
#define AC97_MASTER             0x02 // Master volume
#define AC97_HEADPHONE          0x04 // Headphone volume
#define AC97_MASTER_MONO        0x06 // Master volume mono
#define AC97_MASTER_TONE        0x08 // Master tone (bass & treble)
#define AC97_PC_BEEP            0x0a // PC beep volume
#define AC97_PHONE              0x0c // Phone volume
#define AC97_MIC                0x0e // Mic volume
#define AC97_LINE               0x10 // Line-in volume
#define AC97_CD                 0x12 // CD volume
#define AC97_VIDEO              0x14 // Video volume
#define AC97_AUX                0x16 // Aux volume
#define AC97_PCM                0x18 // PCM out volume
#define AC97_REC_SEL            0x1a // Record select
#define AC97_REC_GAIN           0x1c // Record gain
#define AC97_REC_GAIN_MIC       0x1e // Record gain mic
#define AC97_GENERAL_PURPOSE    0x20 // General purpose
#define AC97_3D_CONTROL         0x22 // 3D control
#define AC97_INT_PAGING         0x24 // Audio interrupt & paging
#define AC97_POWERDOWN          0x26 // Powerdown control / status
#define AC97_EXTENDED_ID        0x28 // Extended audio ID
#define AC97_EXTENDED_STATUS    0x2a // Extended audio status and control
#define AC97_PCM_FRONT_DAC_RATE 0x2c // PCM front DAC rate
#define AC97_PCM_SURR_DAC_RATE  0x2e // PCM surround DAC rate
#define AC97_PCM_LFE_DAC_RATE   0x30 // PCM LFE DAC rate
#define AC97_PCM_LR_ADC_RATE    0x32 // PCM L/R ADC rate
#define AC97_PCM_MIC_ADC_RATE   0x34 // PCM Mic ADC rate
#define AC97_CENTER_LFE_MASTER  0x36 // Center + LFE master volume
#define AC97_SURROUND_MASTER    0x38 // Surround master volume
#define AC97_SPDIF              0x3a // S/PDIF control
#define AC97_VENDOR_ID1         0x7c // Vendor ID1
#define AC97_VENDOR_ID2         0x7e // Vendor ID2 / revision

/* Volume mute masks */
#define AC97_MUTE_MASK_MONO   0x8000
#define AC97_MUTE_MASK_STEREO 0x8080

/* Basic capabilities (AC97_RESET) */
#define AC97_BC_DEDICATED_MIC 0x0001
#define AC97_BC_BASS_TREBLE   0x0004
#define AC97_BC_SIM_STEREO    0x0008
#define AC97_BC_HEADPHONE     0x0010
#define AC97_BC_LOUDNESS      0x0020

/* Extended audio ID bits (AC97_EXTENDED_ID) */
#define AC97_EI_VRA        0x0001 // Variable bit rate audio
#define AC97_EI_DRA        0x0002 // Double rate audio
#define AC97_EI_SPDIF      0x0004 // S/PDIF out
#define AC97_EI_VRM        0x0008 // Variable rate mic ADC
#define AC97_EI_CDAC       0x0040 // Center DAC
#define AC97_EI_SDAC       0x0080 // Surround DACs
#define AC97_EI_LDAC       0x0100 // LFE DAC
#define AC97_EI_ADDR_MASK  0xc000
#define AC97_EI_ADDR_SHIFT 14

/* Extended audio status and control bits (AC97_EXTENDED_STATUS) */
#define AC97_EA_VRA   0x0001 // Enable VRA
#define AC97_EA_DRA   0x0002 // Enable DRA
#define AC97_EA_SPDIF 0x0004 // Enable S/PDIF
#define AC97_EA_VRM   0x0008 // Enable VRM
#define AC97_EA_CDAC  0x0040 // Center DAC ready
#define AC97_EA_SDAC  0x0080 // Surround DAC ready
#define AC97_EA_LDAC  0x0100 // LFE DAC ready

/* General purpose register bits */
#define AC97_GP_DRSS_MASK 0x0c00
#define AC97_GP_DRSS_78   0x0400

/* Codec flags */
#define AC97_DOUBLE_RATE  (1U << 0)
#define AC97_STEREO_MUTES (1U << 1)

/* Maximum codecs on one AC'97 bus */
#define AC97_BUS_MAX_CODECS 4

typedef struct ac97_bus   ac97_bus_t;
typedef struct ac97_codec ac97_codec_t;

typedef struct ac97_bus_ops {
        void (*write)(ac97_codec_t *codec, uint16_t reg, uint16_t val);
        uint16_t (*read)(ac97_codec_t *codec, uint16_t reg);
        void (*reset)(ac97_codec_t *codec);
        void (*warm_reset)(ac97_codec_t *codec);
} ac97_bus_ops_t;

struct ac97_codec {
        ac97_bus_t  *bus;
        unsigned int num;
        unsigned int addr;
        uint32_t     id;
        uint16_t     caps;
        uint16_t     ext_id;
        uint32_t     flags;
        unsigned int rates_dac;
        unsigned int rates_adc;
        unsigned int rates_mic;
        uint16_t     regs[128];
        bool         reg_valid[128];
        mutex_t      reg_mutex;
        char         name[64];
        void        *private_data;
};

struct ac97_bus {
        ulsa_card_t          *card;
        const ac97_bus_ops_t *ops;
        ac97_codec_t         *codecs[AC97_BUS_MAX_CODECS];
        unsigned int          num_codecs;
        unsigned int          clock;
        bool                  no_vra;
        bool                  dra;
        void                 *private_data;
};

#if CONFIG_AUDIO && CONFIG_AUDIO_AC97
/* Create an AC'97 codec bus attached to card. */
ac97_bus_t *ac97_bus_create(ulsa_card_t *card, const ac97_bus_ops_t *ops, void *private_data);

/* Free an AC'97 codec bus and all probed codecs. */
void ac97_bus_destroy(ac97_bus_t *bus);

/* Probe codec index num, configure VRA/rates, and register ULSA mixer controls. */
int ac97_codec_probe(ac97_bus_t *bus, unsigned int num, void *private_data, ac97_codec_t **out_codec);

/* Read a 16-bit AC'97 codec register directly. */
uint16_t ac97_read(ac97_codec_t *codec, uint16_t reg);

/* Write a 16-bit AC'97 codec register directly without updating cache. */
void ac97_write(ac97_codec_t *codec, uint16_t reg, uint16_t val);

/* Write a 16-bit AC'97 codec register and update software cache. */
void ac97_write_cache(ac97_codec_t *codec, uint16_t reg, uint16_t val);

/* Update masked bits in an AC'97 codec register. */
int ac97_update_bits(ac97_codec_t *codec, uint16_t reg, uint16_t mask, uint16_t val);

/* Program a codec sample-rate register (DAC/ADC/MIC). */
int ac97_set_rate(ac97_codec_t *codec, uint16_t reg, unsigned int rate);

/* Format a human-readable AC'97 codec name into buf. */
void ac97_codec_short_name(uint32_t id, char *buf, size_t len);

/* Probe all Intel 8x0 / AC'97 PCI controllers and register ULSA cards. */
void ac97_init(void);
#else
static inline void ac97_init(void) {}
#endif

#endif // INCLUDE_DRIVERS_AUDIO_AC97_AC97_H_
