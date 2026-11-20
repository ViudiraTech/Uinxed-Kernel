/*
 *
 *      sb16.h
 *      Sound Blaster 16 driver header file
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_AUDIO_SOUNDBLASTER_SB16_H_
#define INCLUDE_DRIVERS_AUDIO_SOUNDBLASTER_SB16_H_

#include <drivers/audio/ulsa/ulsa.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

/* ISA port bases probed for Sound Blaster 16 */
#define SB16_BASE_220 0x220U
#define SB16_BASE_240 0x240U
#define SB16_BASE_260 0x260U
#define SB16_BASE_280 0x280U

/* Register offsets relative to card I/O base */
#define SB16_MIXER_ADDR     0x04U
#define SB16_MIXER_DATA     0x05U
#define SB16_DSP_RESET      0x06U
#define SB16_DSP_READ       0x0aU
#define SB16_DSP_WRITE      0x0cU
#define SB16_DSP_DATA_AVAIL 0x0eU
#define SB16_DSP_ACK_8      0x0eU
#define SB16_DSP_ACK_16     0x0fU

/* DSP 4.x commands */
#define SB_DSP_SAMPLE_RATE_OUT 0x41U
#define SB_DSP_SAMPLE_RATE_IN  0x42U
#define SB_DSP4_OUT16_AI       0xb6U
#define SB_DSP4_IN16_AI        0xbeU
#define SB_DSP4_OUT8_AI        0xc6U
#define SB_DSP4_IN8_AI         0xceU
#define SB_DSP_DMA8_OFF        0xd0U
#define SB_DSP_SPEAKER_ON      0xd1U
#define SB_DSP_SPEAKER_OFF     0xd3U
#define SB_DSP_DMA8_ON         0xd4U
#define SB_DSP_DMA16_OFF       0xd5U
#define SB_DSP_DMA16_ON        0xd6U
#define SB_DSP_DMA16_EXIT      0xd9U
#define SB_DSP_DMA8_EXIT       0xdaU
#define SB_DSP_GET_VERSION     0xe1U

/* DSP 4.x sample format mode bytes */
#define SB_DSP4_MODE_UNS_MONO    0x00U
#define SB_DSP4_MODE_SIGN_MONO   0x10U
#define SB_DSP4_MODE_UNS_STEREO  0x20U
#define SB_DSP4_MODE_SIGN_STEREO 0x30U

/* CT1745 Mixer registers */
#define SB_DSP4_RESET       0x00U
#define SB_DSP4_MASTER_DEV  0x30U
#define SB_DSP4_PCM_DEV     0x32U
#define SB_DSP4_SYNTH_DEV   0x34U
#define SB_DSP4_CD_DEV      0x36U
#define SB_DSP4_LINE_DEV    0x38U
#define SB_DSP4_MIC_DEV     0x3aU
#define SB_DSP4_SPEAKER_DEV 0x3bU
#define SB_DSP4_OUTPUT_SW   0x3cU
#define SB_DSP4_INPUT_LEFT  0x3dU
#define SB_DSP4_INPUT_RIGHT 0x3eU
#define SB_DSP4_IGAIN_DEV   0x3fU
#define SB_DSP4_OGAIN_DEV   0x41U
#define SB_DSP4_MIC_AGC     0x43U
#define SB_DSP4_TREBLE_DEV  0x44U
#define SB_DSP4_BASS_DEV    0x46U
#define SB_DSP4_IRQSETUP    0x80U
#define SB_DSP4_DMASETUP    0x81U
#define SB_DSP4_IRQSTATUS   0x82U

/* ISA 8237 DMA auto-init transfer modes */
#define SB16_DMA_MODE_PLAYBACK 0x58U
#define SB16_DMA_MODE_CAPTURE  0x54U

/* DMA ring sizing (< 16 MiB, 64 KiB boundary aligned) */
#define SB16_DMA_BUFFER_SIZE (64UL * 1024UL)
#define SB16_DMA_ALLOC_PAGES 32U

#if CONFIG_AUDIO && CONFIG_AUDIO_SB16

/* Detect and initialize the Sound Blaster 16 ISA sound card. */
void sb16_init(void);

/* Play a short square-wave beep through the SB16 DSP at freq Hz for ms milliseconds. */
void sb16_beep(uint16_t freq, uint32_t ms);

#else

/* Detect and initialize the Sound Blaster 16 ISA sound card (no-op when disabled). */
static inline void sb16_init(void) {}

/* Play a short square-wave beep (no-op when disabled). */
static inline void sb16_beep(uint16_t freq, uint32_t ms)
{
    (void)freq;
    (void)ms;
}

#endif // CONFIG_AUDIO && CONFIG_AUDIO_SB16

#endif // INCLUDE_DRIVERS_AUDIO_SOUNDBLASTER_SB16_H_
