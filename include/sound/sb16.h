/*
 *
 *      sb16.h
 *      Sound Blaster 16 driver header file
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SOUND_SB16_H_
#define INCLUDE_SOUND_SB16_H_

#include <stdint.h>
#include <page.h>

/* SB16 default I/O ports */
#define SB16_BASE_220  0x220
#define SB16_BASE_240  0x240
#define SB16_BASE_260  0x260
#define SB16_BASE_280  0x280

/* SB16 DSP port offsets (relative to base) */
#define SB16_DSP_RESET  0x6  /* Write 1 then 0 to trigger DSP reset */
#define SB16_DSP_READ   0xA  /* Read DSP response data */
#define SB16_DSP_WRITE  0xC  /* Write DSP commands (bit 7 = busy) */
#define SB16_DSP_STATUS 0xE  /* Read data available status (bit 7) */
#define SB16_DSP_ACK    0xE  /* IRQ acknowledge (same port as status) */

/* SB16 Mixer port offsets (relative to base) */
#define SB16_MIXER_ADDR  0x4 /* Select mixer register index */
#define SB16_MIXER_DATA  0x5 /* Read/write selected mixer register */

/* DSP commands */
#define SB16_DSP_CMD_DMA8_OUT       0x14 /* 8-bit single-cycle DMA playback */
#define SB16_DSP_CMD_SET_TIME_CONST 0x40 /* Set 8-bit time constant (1 byte) */
#define SB16_DSP_CMD_SET_RATE16     0x41 /* Set 16-bit sample rate (2 bytes) */
#define SB16_DSP_CMD_DMA16_OUT      0xB0 /* 16-bit single-cycle DMA playback */
#define SB16_DSP_CMD_HALT_DMA       0xD0 /* Halt DMA operation */
#define SB16_DSP_CMD_CONT_DMA       0xD1 /* Continue DMA operation */
#define SB16_DSP_CMD_GET_VERSION    0xE1 /* Get DSP version (2 bytes) */
#define SB16_DSP_CMD_VERSION        0xE1 /* Alias for GET_VERSION */
#define SB16_DSP_CMD_IRQ_STATUS     0xF8 /* Read IRQ status */

/* Mixer registers */
#define SB16_MIXER_MASTER_L     0x00
#define SB16_MIXER_MASTER_R     0x01
#define SB16_MIXER_DAC_L        0x02
#define SB16_MIXER_DAC_R        0x03
#define SB16_MIXER_FM_L         0x04
#define SB16_MIXER_FM_R         0x05
#define SB16_MIXER_CD_L         0x06
#define SB16_MIXER_CD_R         0x07
#define SB16_MIXER_LINE_L       0x08
#define SB16_MIXER_LINE_R       0x09
#define SB16_MIXER_MIC          0x0A
#define SB16_MIXER_SPKR         0x0B
#define SB16_MIXER_OUT_SRC      0x10
#define SB16_MIXER_RESET        0x80

/* Mixer output source */
#define SB16_MIXER_SRC_DAC      0x01
#define SB16_MIXER_SRC_CD       0x02
#define SB16_MIXER_SRC_LINE     0x04
#define SB16_MIXER_SRC_MIC      0x08

/* IRQ values */
#define SB16_IRQ_5  5
#define SB16_IRQ_7  7
#define SB16_IRQ_9  9
#define SB16_IRQ_10 10

/* DMA channels */
#define SB16_DMA8  1
#define SB16_DMA16 5

/* Buffer alignment */
#define SB16_DMA_ALIGN 0x1000

/* SB16 device state */
typedef struct sb16_device {
        uint16_t base;           /* I/O port base address */
        uint8_t  irq;            /* IRQ line */
        uint8_t  dma8;           /* 8-bit DMA channel */
        uint8_t  dma16;          /* 16-bit DMA channel */
        uint8_t  detected;       /* Non-zero if card found */
        uint32_t dma_buffer_phys; /* Physical address of DMA buffer */
        uint8_t *dma_buffer_virt; /* Virtual address of DMA buffer */
        uint32_t dma_buffer_size; /* Size of DMA buffer */
        uint32_t sample_rate;    /* Current sample rate */
        uint8_t  bits;           /* Sample bit depth */
        uint8_t  channels;       /* Number of audio channels */
} sb16_device_t;

/* Initialize SB16 driver and probe hardware */
void sb16_init(void);

/* Probe all legacy I/O ports for SB16 hardware */
int sb16_detect(sb16_device_t *dev);

/* Reset DSP, returns 0 on success (0xAA received) */
int sb16_dsp_reset(sb16_device_t *dev);

/* Query DSP version, returns 0 on success */
int sb16_dsp_version(sb16_device_t *dev, uint8_t *major, uint8_t *minor);

/* Wait until DSP is ready to accept a command */
int sb16_dsp_wait_write(sb16_device_t *dev);

/* Write a command byte to DSP, returns 0 on success */
int sb16_dsp_write(sb16_device_t *dev, uint8_t cmd);

/* Read a data byte from DSP, returns 0 on success */
int sb16_dsp_read(sb16_device_t *dev, uint8_t *val);

/* Set sample rate for 8-bit playback via time constant (4000-23000 Hz) */
int sb16_set_rate8(sb16_device_t *dev, uint16_t rate);

/* Set sample rate for 16-bit playback (4000-48000 Hz) */
int sb16_set_rate16(sb16_device_t *dev, uint16_t rate);

/* Start single-cycle 8-bit DMA playback */
int sb16_play_8bit(sb16_device_t *dev, uint8_t *buffer, uint32_t size);

/* Start single-cycle 16-bit DMA playback */
int sb16_play_16bit(sb16_device_t *dev, uint8_t *buffer, uint32_t size);

/* Stop all DMA audio playback */
void sb16_stop(sb16_device_t *dev);

/* Set master volume (0-255 each channel) */
void sb16_set_master_volume(sb16_device_t *dev, uint8_t left, uint8_t right);

/* Set DAC/PCM volume (0-255 each channel) */
void sb16_set_dac_volume(sb16_device_t *dev, uint8_t left, uint8_t right);

/* Read a mixer register */
uint8_t sb16_mixer_read(sb16_device_t *dev, uint8_t reg);

/* Write a mixer register */
void sb16_mixer_write(sb16_device_t *dev, uint8_t reg, uint8_t value);

/* Play a simple square-wave beep tone */
void sb16_beep(uint16_t freq, uint32_t ms);

#endif /* INCLUDE_SOUND_SB16_H_ */
